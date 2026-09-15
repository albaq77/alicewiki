// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 92-AudioPlay：基于 WM8978 与 SAI1 的 SD 卡 WAV 音乐播放器 (UNI库)
 *
 * 预期现象：
 *   1) 上电初始化 Mempool、SDRAM、LTDC、WM8978 音频芯片、SAI1 接口与 SD 卡 FAT 文件系统。
 *   2) LCD 显示播放器界面（Alice.Wiki 92-AudioPlay、曲目信息、播放进度/时间、采样率、码率）。
 *   3) 自动从 SD 卡扫描 WAV 音乐文件（根目录 .wav 文件）。
 *   4) 按键操作：
 *      - KEY0 (PH3): 下一曲
 *      - KEY2 (PC13): 上一曲
 *      - WK_UP (PA0): 暂停 / 播放
 *   5) DS0 (LEDB / PB0) 翻转指示运行状态。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/SAI>
#include <cpp/Device/DMA>
#include <cpp/Device/DBG>
#include <c/format/filesys/FAT.h>
#include <c/format/audio/WAV.h>   // uni::WAVCodec / WAVStream (流式按块解析, 供后续替代手写解析)
#include <c/format/audio/MP3.h>   // uni::MP3Codec
#include <c/format/audio/FLAC.h>  // uni::FLACCodec
#include <c/mempool.h>
#include <c/data.h>
#include <c/consio.h>
#include <c/ustring.h>
#include "../../../device/audio/wm8978.hpp"
#include "../../../device/audio/es8388.hpp"
#include "../_opendev/RGB-LCD.hpp"
#include "../_opendev/SDRAM.hpp"
#include <new>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

extern uni::Mempool mempool;// 由 _opendev/_MEMMAN.cpp 定义

// 板载指示灯与按键
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// WK_UP (PA0)
GPIN& KEYL = GPIOC[13];// KEY2  (PC13)
GPIN& KEYD = GPIOH[ 2];// KEY1  (PH2)
GPIN& KEYR = GPIOH[ 3];// KEY0  (PH3)

void outtxt(const char* str, stduint len) { XART1.out(str, len); }

// Mempool 管理的内存切片（照 80-Memman / 91-ImageShow：SDRAM + SRAM12 + SRAM4，不用 AXI/ITCM）。
// 避开 LTDC 帧缓冲（SDRAM_BANK1_BASE 起 ~0xC0000）与下方 FAT 缓冲（+0x100000 起 1KB）。
#define SDRAM_POOL_BASE      (SDRAM_BANK1_BASE + 0x100000 + 0x10000)
#define SDRAM_POOL_SIZE      (4U * 1024U * 1024U)
#define SRAM12_ADDR          ((stduint)0x30000000)
#define SRAM4_ADDR           ((stduint)0x38000000)

#define IMAGE_CARD_BLOCK_SIZE 512
#define IMAGE_FILE_BLOCK_SIZE IMAGE_CARD_BLOCK_SIZE
#define IMAGE_PROFILE         0
#define IMAGE_CARD_CACHE      1
#define IMAGE_CARD_CACHE_BLOCKS 32
#define IMAGE_CARD_CACHE_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_CACHE_BLOCKS)
#define IMAGE_CARD_CACHE_INVALID_BLOCK ((stduint)-1)
#define IMAGE_CARD_READAHEAD  0
#define IMAGE_CARD_READAHEAD_BLOCKS 8
#define IMAGE_CARD_READ_TIMEOUT_MS 1000
#define IMAGE_CARD_READAHEAD_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_READAHEAD_BLOCKS)
#define IMAGE_FAT_BUFFER_SIZE IMAGE_CARD_BLOCK_SIZE

// FAT 缓冲（外部 SDRAM，避开 LTDC 帧缓冲与 MEMPOOL 切片）
static byte* sector_buf      = (byte*)(SDRAM_BANK1_BASE + 0x100000);        // 扇区缓冲 512B
static byte* fat_buf         = sector_buf + IMAGE_FAT_BUFFER_SIZE;          // FAT 表缓冲 512B

#include "../_opendev/_ImageProfile.hpp"
#include "../_opendev/_FileBlockDevice.hpp"   // 共享: FAT 文件 → 按块 StorageTrait (WAVCodec 流式解析用)

// 音频 DMA 缓冲（必须位于 D2 域 SRAM1/2，以供 DMA1 访问，不能放在 SDRAM）
#define AUDIO_DMA_BUF_SIZE   8192                                           // DMA 传输缓冲区大小（字节）
#define AUDIO_RING_SLOTS     4
static byte* ring_buf[AUDIO_RING_SLOTS] = {
	(byte*)(0x30000000 + 0x0000),// slot 0: DMA M0 buffer
	(byte*)(0x30000000 + 0x2000),// slot 1: DMA M1 buffer
	(byte*)(0x30000000 + 0x4000),// slot 2: read-ahead buffer
	(byte*)(0x30000000 + 0x6000),// slot 3: read-ahead buffer
};
static byte* audio_temp_buf  = nullptr;                                     // 格式转换临时缓冲，FAT 挂载后由 mempool 分配
#define saibuf0 ring_buf[0]
#define saibuf1 ring_buf[1]
#include <cpp/System/Audiosys/AudioBridge.hpp>   // uni::AudioBridge: 数据面 SAI1+DMA1, 控制面 SubACI

// 软件 I2C 延时与驱动实例（放慢延时以适配 480MHz 主频下的 I2C 上升时间）
static void iic_delay() {
	for (volatile int i = 0; i < 500; i++) {}
}

class AudioIIC : public uni::IIC_SOFT {
public:
	AudioIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

enum class AudioCodecType {
	None,
	WM8978,
	ES8388
};

static AudioCodecType active_codec = AudioCodecType::None;
static byte audio_wire_buf[byteof(AudioIIC)];
static byte wm8978_buf[byteof(WM8978_t)];
static byte es8388_buf[byteof(ES8388_t)];
static AudioIIC* audio_iic = nullptr;
static WM8978_t* wm8978 = nullptr;
static ES8388_t* es8388 = nullptr;

static void codec_set_i2s(byte bits_per_sample) {
	if (active_codec == AudioCodecType::ES8388 && es8388) {
		ES8388_t::I2SLength len = (bits_per_sample == 24) ? ES8388_t::I2SLength::Bit24 : ES8388_t::I2SLength::Bit16;
		es8388->SetI2S(ES8388_t::I2SFormat::Standard, len);
	} else if (active_codec == AudioCodecType::WM8978 && wm8978) {
		WM8978_t::I2SLength len = (bits_per_sample == 24) ? WM8978_t::I2SLength::Bit24 : WM8978_t::I2SLength::Bit16;
		wm8978->SetI2S(WM8978_t::I2SFormat::Standard, len);
	}
}


// 1kHz standard sine wave power-on test (走 AudioBridge)
static uint32 beep_bytes_left = 0;
static uint32 beep_phase = 0;


// WAV 文件格式解析结构体
struct WAVCtrl {
	uint16 audio_format;
	uint16 num_channels;
	uint32 sample_rate;
	uint32 byte_rate;
	uint16 block_align;
	uint16 bits_per_sample;
	uint32 data_size;
	uint32 data_start;
	uint32 total_sec;
	uint32 cur_sec;
};

// 从 uni::IAudioStream 拉 PCM 并归一到“引擎格式 = 16bit 立体声 S16LE”(2ch × 2B/帧 = 4B/帧)。
// 引擎恒定输出立体声 16bit, 故 DMA/SAI 恒为 16-bit; 这里只做 声道复制/8bit 抬位, 不涉及 WAV 容器解析。
// 返回写入 dst 的引擎字节数(<=cap); 0 = 流结束/无可读。
static uint32 wav_stream_fill(uni::IAudioStream& st, byte* dst, uint32 cap, const uni::AudioInfo& ai) {
	bool is_u8  = (ai.format.sample_format == uni::AudioSampleFormat::U8);
	bool is_mono = (ai.format.channels == 1);
	uint32 out_frames = cap / 4;

	if (!is_u8 && !is_mono) {
		// S16LE 立体声: 直接拉取
		uint32 rd = 0;
		st.ReadSamples(dst, cap, rd);
		if (rd < cap) for (uint32 i = rd; i < cap; i++) dst[i] = 0;
		return rd;
	}

	// 需要转换: 先读入临时缓冲(引擎输出 4B/帧, 源帧 = mono?2:4 / 8bit?1/2 B)
	uint32 src_bytes = is_u8
		? out_frames * (is_mono ? 1 : 2)      // U8: 每帧 mono=1B / stereo=2B
		: out_frames * 2;                      // S16 mono: 每帧 2B
	uint32 rd = 0;
	st.ReadSamples(audio_temp_buf, src_bytes, rd);

	if (is_u8) {
		int16* d = (int16*)dst;
		if (is_mono) {
			uint32 n = rd;                      // U8 mono: rd 字节 = 采样数
			for (uint32 i = 0; i < n; i++) {
				int16 s = (int16)(((int32)audio_temp_buf[i] - 128) << 8);
				d[2 * i + 0] = s; d[2 * i + 1] = s;
			}
			for (uint32 i = n; i < out_frames; i++) { d[2 * i + 0] = 0; d[2 * i + 1] = 0; }
			return n * 4;
		} else {
			uint32 n = rd / 2;                  // U8 stereo: 每 2B 一帧
			for (uint32 i = 0; i < n; i++) {
				int16 l = (int16)(((int32)audio_temp_buf[2 * i + 0] - 128) << 8);
				int16 r = (int16)(((int32)audio_temp_buf[2 * i + 1] - 128) << 8);
				d[2 * i + 0] = l; d[2 * i + 1] = r;
			}
			for (uint32 i = n; i < out_frames; i++) { d[2 * i + 0] = 0; d[2 * i + 1] = 0; }
			return n * 4;
		}
	} else {
		// S16LE mono → 双声道复制
		uint32 n = rd / 2;
		int16* s = (int16*)audio_temp_buf;
		int16* d = (int16*)dst;
		for (uint32 i = 0; i < n; i++) { d[2 * i + 0] = s[i]; d[2 * i + 1] = s[i]; }
		for (uint32 i = n; i < out_frames; i++) { d[2 * i + 0] = 0; d[2 * i + 1] = 0; }
		return n * 4;
	}
}

// LCD 文本绘制辅助（16x8 点阵）
#define LCD_FB_STRIDE 800
#define LCD_FB_LINES  480
#define LCD_FB ((uint16*)FMC_SDRAM_BANK1_BASE)

static void lcd_draw_char(stduint x, stduint y, char ch, uint16 color, uint16 bg) {
	if (ch < 32 || ch > 126) ch = '?';
	ch -= 0x20;
	const uint16* datptr = (const uint16*)&_BITFONT_ASCII_16x8[(byte)ch];
	for (byte col = 0; col < 8; col++) {
		uint16 dat = datptr[col];
		for (byte row = 0; row < 16; row++) {
			stduint px = x + col, py = y + (row ^ 0b111);
			if (px < LCD_FB_STRIDE && py < LCD_FB_LINES) {
				LCD_FB[py * LCD_FB_STRIDE + px] = (dat & _IMM1S(row)) ? color : bg;
			}
		}
	}
}

static void lcd_draw_string(stduint x, stduint y, const char* str, uint16 color = 0xFFFF, uint16 bg = 0x0000) {
	while (*str) {
		lcd_draw_char(x, y, *str++, color, bg);
		x += 8;
	}
}

static void lcd_fill_rect(stduint x, stduint y, stduint w, stduint h, uint16 color) {
	for (stduint r = y; r < y + h && r < LCD_FB_LINES; r++) {
		for (stduint c = x; c < x + w && c < LCD_FB_STRIDE; c++) {
			LCD_FB[r * LCD_FB_STRIDE + c] = color;
		}
	}
}

// 刷新主界面与状态
static void lcd_update_ui(const char* song_name, uint32 cur_idx, uint32 total_songs, const WAVCtrl& wav, bool playing) {
	char line[64];
	// 标题栏
	lcd_draw_string(20, 20, "========================================", 0x07E0, 0x0000);
	lcd_draw_string(20, 40, "   Alice.Wiki 92-AudioPlay              ", 0xFFE0, 0x0000);
	lcd_draw_string(20, 60, "========================================", 0x07E0, 0x0000);

	// 曲目序号与歌名
	outsfmtbuf(line, "Track : [%02d / %02d]", (int)(cur_idx + 1), (int)total_songs);
	lcd_draw_string(20, 100, line, 0xFFFF, 0x0000);
	outsfmtbuf(line, "Title : %-30s", song_name);
	lcd_draw_string(20, 130, line, 0x07FF, 0x0000);

	// 播放时间与状态
	outsfmtbuf(line, "Time  : %02d:%02d / %02d:%02d    Status: %s",
		(int)(wav.cur_sec / 60), (int)(wav.cur_sec % 60),
		(int)(wav.total_sec / 60), (int)(wav.total_sec % 60),
		playing ? "PLAYING" : "PAUSED ");
	lcd_draw_string(20, 160, line, 0xFFFF, 0x0000);

	// 采样格式与码率
	outsfmtbuf(line, "Format: %u Hz, %d-bit, %s, %u Kbps",
		(unsigned)wav.sample_rate, (int)wav.bits_per_sample,
		(wav.num_channels == 1) ? "Mono" : "Stereo",
		(unsigned)(wav.byte_rate * 8 / 1000));
	lcd_draw_string(20, 190, line, 0xFD20, 0x0000);

	// 按键提示
	lcd_draw_string(20, 250, "----------------------------------------", 0x8410, 0x0000);
	lcd_draw_string(20, 270, "KEY0: NEXT   |  KEY2: PREV   |  WK_UP: PAUSE", 0x07E0, 0x0000);
	lcd_draw_string(20, 290, "----------------------------------------", 0x8410, 0x0000);
}

// 播放列表管理
#define MAX_SONGS 64
static const char* song_paths[MAX_SONGS];
static stduint song_count = 0;

// 文件枚举回调：FAT 按 (is_dir, name) 两参调用
static void on_wav_seg(void* is_dir, ...) {
	if (is_dir) return;
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	stduint len = StrLength(nm);
	if (len < 4) return;
	bool is_wav  = (StrCompareInsensitive(nm + len - 4, ".wav") == 0);
	bool is_mp3  = (StrCompareInsensitive(nm + len - 4, ".mp3") == 0);
	bool is_flac = (len >= 5) && (StrCompareInsensitive(nm + len - 5, ".flac") == 0);
	if (!is_wav && !is_mp3 && !is_flac) return;
	if (song_count >= MAX_SONGS) return;

	char* full = (char*)mempool.allocate(len + 2);
	full[0] = '/';
	MemCopyN(full + 1, nm, len);
	full[len + 1] = '\0';
	song_paths[song_count++] = full;
}

// 扫描 SD 卡根目录下的所有 .wav / .mp3 / .flac 文件
static stduint collect_wav_files(FilesysFAT& fs) {
	song_count = 0;
	FAT_FileHandle h;
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	if (!fs.search("/", &sargs)) return 0;
	fs.enumer(&h, on_wav_seg);
	return song_count;
}

// 错误死机闪烁
void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i = 0; i < 1000000; i++) {}
	}
}

// 引擎回调: 把 uni::IAudioStream 的 PCM 拉进 DMA 缓冲(适配 AudioPcmRefillHandler 签名)
struct PcmRefillContext {
	uni::IAudioStream* stream;
	uni::AudioInfo info;
};

static uint32 pcm_refill(void* context, uint8* destination, uint32 byte_count) {
	PcmRefillContext* ctx = (PcmRefillContext*)context;
	if (!ctx || !ctx->stream) return 0;
	return wav_stream_fill(*ctx->stream, destination, byte_count, ctx->info);
}

int main() {
	L1C.enAbleICacheAll();// 开启 I-Cache 加速指令执行，保持 D-Cache 关闭以确保 DMA 物理内存一致性
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	GPIOA.enClock(true); GPIOB.enClock(true); GPIOC.enClock(true); GPIOH.enClock(true);
	LEDB.setMode(GPIOMode::OUT) = !false;// DS0
	LEDR.setMode(GPIOMode::OUT) = !false;// DS1
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// WK_UP (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true); // KEY2  (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true); // KEY1  (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true); // KEY0  (PH3)

	XART1.setMode(115200);
	outsfmt("\n========================================\n");
	outsfmt("APOLLO STM32H7 - 92-AudioPlay (WM8978 / ES8388)\n");
	outsfmt("========================================\n");

	// 初始化 SDRAM 并注册 Mempool 内存切片
	sdram_init();
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{ SDRAM_POOL_BASE, SDRAM_POOL_SIZE });
	mempool.Append(Slice{ SRAM12_ADDR + 0x8000, 32U * 1024U });
	mempool.Append(Slice{ SRAM4_ADDR,  32U * 1024U });

	ltdc_init();
	lcd_fill_rect(0, 0, 800, 480, 0x0000);
	lcd_draw_string(20, 20, "System initializing...", 0xFFFF, 0x0000);

	DBGMCU.enDBGStandby(true);         // Domain1 待机调试
	DBGMCU.enDBGStandbyDomain3(true);  // Domain3 待机调试

	// 初始化 SDMMC1 与 FAT32 文件系统（照 91-ImageShow 使用 CachedStorageDevice）
	if (!SDCard1.setMode()) {
		outsfmt("SD Card Init Error!\n");
		lcd_draw_string(20, 80, "SD Card Error! Please insert SD card.", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}
	SDCard1.Block_Size = IMAGE_CARD_BLOCK_SIZE;
	outsfmt("SD Card OK!\n");

	CachedStorageDevice cached_sd(SDCard1);
	FilesysFAT fs(0, cached_sd, sector_buf, fat_buf);
	if (!fs.loadfs()) {
		outsfmt("FAT mount Error: %u!\n", (unsigned)fs.error_number);
		lcd_draw_string(20, 80, "FAT Mount Failed!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}
	outsfmt("FAT%u Mounted OK!\n", (unsigned)fs.fat_type);

	// 扫描歌曲文件
	collect_wav_files(fs);
	outsfmt("Found %u songs.\n", (unsigned)song_count);
	if (song_count == 0) {
		lcd_draw_string(20, 110, "No WAV files found in root dir!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}

	audio_temp_buf = (byte*)mempool.allocate(AUDIO_DMA_BUF_SIZE);
	if (!audio_temp_buf) {
		outsfmt("Audio temp buffer allocate failed!\n");
		lcd_draw_string(20, 110, "Audio buffer alloc failed!", 0xF800, 0x0000);
		while (1) SysDelay_ms(1000);
	}

	// 初始化 I2C 接口
	audio_iic = new (audio_wire_buf) AudioIIC(GPIOH[5], GPIOH[4]);
	audio_iic->func_delay = iic_delay;

	// 探测 ES8388 (0x10)：尝试读取控制寄存器 0
	es8388 = new (es8388_buf) ES8388_t(*audio_iic);
	byte es_r0 = es8388->Read(0);
	bool es_detected = (audio_iic->getError() == ERR_IIC_NONE);

	if (es_detected) {
		active_codec = AudioCodecType::ES8388;
		// 上电冷态即完成 ES8388 全量初始化（I2C 干净、SAI/PLL2/DMA 尚未启动），只做一次
		bool ok = es8388->Initialize();
		es8388->SetADDA(true, false);
		es8388->SetOutput(true, true);
		es8388->SetHPVol(10);
		es8388->SetSPKVol(10);
		outsfmt("Audio Codec Detected: ES8388 (R0=0x%02X, Init=%s)\r\n",
			(unsigned)es_r0, ok ? "OK" : "FAIL");
	} else {
		// 未检出 ES8388，探测 WM8978 (0x1A)
		wm8978 = new (wm8978_buf) WM8978_t(*audio_iic);
		bool wm_ok = wm8978->Initialize();
		if (wm_ok) {
			active_codec = AudioCodecType::WM8978;
			wm8978->SetADDA(true, false);
			wm8978->SetInput(false, false, false);
			wm8978->SetOutput(true, false);
			wm8978->SetHPVol(35, 35); // 适中耳机音量（官方默认40，最大63）
			wm8978->SetSPKVol(28);    // 适中喇叭音量（官方默认30，最大63）
			outsfmt("Audio Codec Detected: WM8978 (Init=OK)\r\n");
		} else {
			active_codec = AudioCodecType::None;
			outsfmt("Audio Codec Detection FAILED: neither ES8388 nor WM8978 responded!\r\n");
		}
	}

	// 板级桥: 数据面 SAI1 + DMA1, 控制面转发给检出的 codec(可为 nullptr)
	uni::AudioDeviceInterface* audio_chip = nullptr;
	if (active_codec == AudioCodecType::ES8388) audio_chip = es8388;
	else if (active_codec == AudioCodecType::WM8978) audio_chip = wm8978;
	uni::AudioBridge bridge(ring_buf, 2, AUDIO_DMA_BUF_SIZE, audio_chip);// DMA 双缓冲: M0=ring_buf[0], M1=ring_buf[1]

	stduint cur_song_idx = 0;
	WAVCtrl wav{};

	// 播放器主循环
	while (true) {
		const char* current_file = song_paths[cur_song_idx];
		FAT_FileHandle fh;
		FilesysSearchArgs sargs{};
		sargs.handle_buffer = &fh;
		void* file_ptr = fs.search(current_file, &sargs);
		if (!file_ptr || fh.is_dir || !fh.size) {
			outsfmt("Failed to open file: %s\n", current_file);
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}

		// ---- uni 流式解析: FAT文件 → FileBlockDevice → 按格式选 Codec (WAV / MP3 / FLAC) ----
		FileBlockDevice song_file(fs, file_ptr, (stduint)fh.size, 512);
		uni::WAVCodec  wav_codec;
		uni::MP3Codec  mp3_codec;
		uni::FLACCodec flac_codec;
		const uni::IAudioCodec* codec = nullptr;
		bool matched = false;
		if (wav_codec.Probe(song_file, matched) == uni::AudioResult::OK && matched) codec = &wav_codec;
		else if (mp3_codec.Probe(song_file, matched) == uni::AudioResult::OK && matched) codec = &mp3_codec;
		else if (flac_codec.Probe(song_file, matched) == uni::AudioResult::OK && matched) codec = &flac_codec;
		if (!codec) {
			outsfmt("Unsupported audio file: %s\n", current_file);
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}
		uni::IAudioStream* wstream = nullptr;
		if (codec->OpenStream(song_file, wstream, mempool) != uni::AudioResult::OK || !wstream) {
			outsfmt("OpenStream FAILED: %s\n", current_file);
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}
		uni::AudioInfo ai{};
		wstream->GetInfo(ai);

		// SAI/PLL2 只能精确生成表里那几档速率; 源率不在其中(如 11025/11000)时重采样到 44100
		uint32 source_rate = ai.format.sample_rate;
		uni::IAudioStream* active_stream = wstream;
		if (!uni::AudioBridge::isRateSupported(source_rate)) {
			void* rs_mem = mempool.allocate(sizeof(uni::ResamplerStream), 3);
			uni::ResamplerStream* rs = rs_mem ? new (rs_mem) uni::ResamplerStream(wstream, 44100, mempool) : nullptr;
			if (rs && rs->IsValid()) {
				active_stream = rs;
				active_stream->GetInfo(ai);// 重取: 采样率已变为 44100
				outsfmt("[Audio] Resample %u Hz -> 44100 Hz\r\n", (unsigned)source_rate);
			} else {
				if (rs) rs->Release();
				outsfmt("[Audio] Resampler unavailable, unsupported rate %u Hz\r\n", (unsigned)source_rate);
			}
		}

		// 引擎输出恒定 16bit 立体声(4B/帧); WAVCtrl 按引擎口径换算, 供 LCD/进度/结束判定
		const uint32 ENGINE_FRAME_BYTES = 4;
		wav.audio_format = 1;
		wav.num_channels   = ai.format.channels;   // 显示源声道(引擎都按立体声播)
		wav.sample_rate    = ai.format.sample_rate;
		wav.bits_per_sample = (ai.bitsPerSample) ? (uint16)ai.bitsPerSample : 16;
		wav.byte_rate      = ai.format.sample_rate * ENGINE_FRAME_BYTES;
		wav.block_align    = ENGINE_FRAME_BYTES;
		wav.data_size      = ai.totalSamples * ENGINE_FRAME_BYTES;
		wav.data_start     = 0;
		wav.total_sec      = ai.format.sample_rate ? (uint32)((uint64)ai.totalSamples / ai.format.sample_rate) : 0;
		wav.cur_sec        = 0;

		outsfmt("Playing: %s (Rate=%u, Bits=%u, Ch=%u)\n",
			current_file, (unsigned)wav.sample_rate, (unsigned)wav.bits_per_sample, (unsigned)wav.num_channels);

		// 引擎由 AudioBridge 持有: codec 格式/I2S、SAI+PLL2 时钟、DMA 双缓冲与启动都在桥内完成
		bool is_playing = true;
		PcmRefillContext refill_ctx = { active_stream, ai };
		if (!bridge.StartStream(ai.format, pcm_refill, &refill_ctx)) {
			outsfmt("Stream empty or start failed: %s\n", current_file);
			active_stream->Release();
			cur_song_idx = (cur_song_idx + 1) % song_count;
			continue;
		}

		outsfmt("[Audio] Playback started! (DMA1_CR=0x%08X, SAI1_SR=0x%08X)\r\n",
			(unsigned)*(volatile uint32*)0x40020088, (unsigned)(uint32)SAI1[1][SAIReg::SR]);

		stduint played_data_pos = 0;
		stduint last_update_sec = (stduint)-1;
		bool song_switch = false;
		stduint next_song_idx = cur_song_idx;

		lcd_update_ui(current_file, cur_song_idx, song_count, wav, is_playing);

		// 单曲播放主流转: 桥每轮消费 DMA 完成事件并回填两块, 这里只管进度/按键/界面
		while (!song_switch) {
			bridge.ServicePlayback();

			played_data_pos = bridge.getPlayedBytes();
			uint32 played_clamped = (played_data_pos > wav.data_size) ? wav.data_size : played_data_pos;
			if (wav.byte_rate > 0) wav.cur_sec = played_clamped / wav.byte_rate;

			// 流耗尽即到曲尾: 此刻 DMA 里还压着最多两块真实数据, 等它播完再切曲。
			// MP3 的总样本数是按码率估算的(MP3Codec::OpenStream), 与 WAV/FLAC 的精确值不同, 故不拿它当结束依据。
			if (bridge.isStreamOver()) {
				SysDelay_ms(120);
				song_switch = true;
				next_song_idx = (cur_song_idx + 1) % song_count;
				break;
			}

			// 按键交互检测
			if (KEYR == 0) {// KEY0: 下一曲
				SysDelay_ms(20);
				if (KEYR == 0) {
					while (KEYR == 0);
					song_switch = true;
					next_song_idx = (cur_song_idx + 1) % song_count;
					break;
				}
			}
			if (KEYL == 0) {// KEY2: 上一曲
				SysDelay_ms(20);
				if (KEYL == 0) {
					while (KEYL == 0);
					song_switch = true;
					next_song_idx = (cur_song_idx == 0) ? (song_count - 1) : (cur_song_idx - 1);
					break;
				}
			}
			if (KEYU == 1) {// WK_UP: 暂停/继续
				SysDelay_ms(20);
				if (KEYU == 1) {
					while (KEYU == 1);
					is_playing = !is_playing;
					if (is_playing) bridge.ResumeStream(); else bridge.PauseStream();
				}
			}

			// 仅在秒数改变或状态切换时刷新界面，避免频繁刷屏霸占 SDRAM 总线导致音频卡顿断续
			if (wav.cur_sec != last_update_sec) {
				last_update_sec = wav.cur_sec;
				LEDB.Toggle();
				lcd_update_ui(current_file, cur_song_idx, song_count, wav, is_playing);
			}
		}

		bridge.StopStream();
		// ResamplerStream 的析构会一并 Release 源流, 所以只释放最外层
		if (active_stream) { active_stream->Release(); active_stream = nullptr; }
		cur_song_idx = next_song_idx;
	}
}
