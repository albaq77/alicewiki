// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 93-VideoPlay (Phase 2): Standard AVICodec & IVideoStream Video Playback
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/GPU>
#include <c/format/filesys/FAT.h>
#include <c/format/video/AVI.h>
#include <c/format/audio/WAV.h>
#include <c/format/picture/JPEG.h>
#include <c/mempool.h>
#include <c/data.h>
#include <c/consio.h>
#include <c/ustring.h>
#include <c/prochip/CortexM7.h>
#include <cpp/Device/DBG>
#include <cpp/Device/SAI>
#include <cpp/Device/DMA>
#include <cpp/System/Audiosys/AudioBridge.hpp>
#include <new>
#include "../../../device/audio/wm8978.hpp"
#include "../../../device/audio/es8388.hpp"
#include "../_opendev/RGB-LCD.hpp"
#include "../_opendev/SDRAM.hpp"

extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

extern uni::Mempool mempool;

extern "C" {
void outtxt(const char* str, stduint len) { XART1.out(str, len); }
}

// On-board LEDs and Direction Keys
GPIN& LEDB = GPIOB[ 0]; // DS0
GPIN& LEDR = GPIOB[ 1]; // DS1
GPIN& KEYU = GPIOA[ 0]; // Up
GPIN& KEYL = GPIOC[13]; // Lf
GPIN& KEYD = GPIOH[ 2]; // Dn
GPIN& KEYR = GPIOH[ 3]; // Rt

// Mempool slices: 16MB SDRAM
// The 32MB SDRAM is split by the MPU into two 16MB halves: the low half holds the LTDC framebuffer and the SD/FAT buffers (hardware reads both, so they stay non-cacheable), the high half holds the mempool (only the CPU touches it, so it is cacheable write-back write-allocate).
#define MPU_SDRAM_CACHEABLE   1
#define SDRAM_UNCACHED_BASE   SDRAM_BANK1_BASE
#define SDRAM_CACHED_BASE     (SDRAM_BANK1_BASE + 0x1000000)
#if MPU_SDRAM_CACHEABLE
#define SDRAM_POOL_BASE       SDRAM_CACHED_BASE
#else
#define SDRAM_POOL_BASE       (SDRAM_BANK1_BASE + 0x100000 + 0x1000)
#endif
#define SDRAM_POOL_SIZE       (16U * 1024U * 1024U)
#define SRAM12_ADDR           ((stduint)0x30000000)
#define SRAM4_ADDR            ((stduint)0x38000000)

// Audio DMA ring: D2 SRAM where DMA1 can reach it, never in the mempool. ServicePlayback() refills at most ONE
// buffer per call, so a buffer must outlast the gap between calls: B >= 176400 B/s * (decode + blit). The heavy
// frames here reach 170+26 = 196 ms, so 49152 B (278.6 ms) is used rather than the 32768 B that 155 ms frames need.
#define AUDIO_DMA_BUF_SIZE    49152
#define AUDIO_RING_SLOTS      2
#define AUDIO_SRAM_BYTES      ((stduint)(AUDIO_DMA_BUF_SIZE * AUDIO_RING_SLOTS))
static byte* audio_ring[AUDIO_RING_SLOTS] = {
	(byte*)(SRAM12_ADDR + 0 * AUDIO_DMA_BUF_SIZE),
	(byte*)(SRAM12_ADDR + 1 * AUDIO_DMA_BUF_SIZE),
};

// Cortex-M7 MPU registers; RASR SIZE=23 means 16MB, TEX=001 means Normal, and C/B pick the cache policy.
#define _MPU_CTRL_ADDR 0xE000ED94
#define _MPU_RNR_ADDR  0xE000ED98
#define _MPU_RBAR_ADDR 0xE000ED9C
#define _MPU_RASR_ADDR 0xE000EDA0
#define _MPU_RASR_16M  ((1u << 0) | (23u << 1) | (1u << 19) | (3u << 24))
#define _MPU_RASR_NC   _MPU_RASR_16M
#define _MPU_RASR_WBWA (_MPU_RASR_16M | (1u << 16) | (1u << 17))

static void mpu_sdram_init() {
	Reference(_MPU_CTRL_ADDR).rstof(0);// program regions with the MPU off
	__DSB();
	Reference(_MPU_RNR_ADDR) = 0;
	Reference(_MPU_RBAR_ADDR) = SDRAM_UNCACHED_BASE;
	Reference(_MPU_RASR_ADDR) = _MPU_RASR_NC;
	Reference(_MPU_RNR_ADDR) = 1;
	Reference(_MPU_RBAR_ADDR) = SDRAM_CACHED_BASE;
	Reference(_MPU_RASR_ADDR) = _MPU_RASR_WBWA;
	Reference(_MPU_CTRL_ADDR) = (1u << 0) | (1u << 2);// ENABLE | PRIVDEFENA
	__DSB();
	__ISB();
}

// DWT cycle counter: SysTick::getTick() only resolves 1 ms, too coarse to split the decode stages.
#define _DEMCR_ADDR      0xE000EDFC
#define _DWT_CTRL_ADDR   0xE0001000
#define _DWT_CYCCNT_ADDR 0xE0001004
#define _DWT_LAR_ADDR    0xE0001FB0

static inline uint32 dwt_cyccnt() {
	return (uint32)Reference(_DWT_CYCCNT_ADDR);
}

static stduint g_sysclk_mhz = 480;// overwritten at startup with the measured HCLK; CYCCNT counts core cycles

static bool dwt_init() {
	Reference(_DEMCR_ADDR) |= (1u << 24);// DEMCR.TRCENA
	Reference(_DWT_LAR_ADDR) = 0xC5ACCE55u;// unlock DWT before CYCCNT can be enabled
	Reference(_DWT_CYCCNT_ADDR) = 0;
	Reference(_DWT_CTRL_ADDR) |= (1u << 0);// DWT_CTRL.CYCCNTENA
	uint32 a = dwt_cyccnt();// verify it actually runs: some parts leave CYCCNT frozen
	for (volatile int i = 0; i < 1000; i++) {}
	return dwt_cyccnt() != a;
}

// ---- Audio output: codec over software I2C (PH4/PH5) plus SAI1 + DMA1 ring, wired as in 92-AudioPlay ----
static void audio_iic_delay() {
	for (volatile int i = 0; i < 500; i++) {}
}

class AudioIIC : public uni::IIC_SOFT {
public:
	AudioIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

static byte audio_wire_buf[byteof(AudioIIC)];
static byte wm8978_buf[byteof(WM8978_t)];
static byte es8388_buf[byteof(ES8388_t)];
static byte bridge_buf[byteof(uni::AudioBridge)];
static AudioIIC* audio_iic = nullptr;
static WM8978_t* wm8978 = nullptr;
static ES8388_t* es8388 = nullptr;
static uni::AudioDeviceInterface* audio_chip = nullptr;

// The engine always consumes 16-bit stereo, so S16LE stereo passes straight through and mono is expanded in place.
struct AviPcmContext {
	uni::IVideoStream* stream;
	bool mono;
	uint32 passes;// log-only: completed DMA buffer passes, for the audio-position column
	uint32 partial;
	uint32 ct;
	bool started;
};

static uint32 avi_pcm_refill(void* context, uint8* destination, uint32 byte_count) {
	AviPcmContext* ctx = (AviPcmContext*)context;
	if (!ctx || !ctx->stream || !destination || byte_count < 4) return 0;
	if (!ctx->mono) {
		uint32 rd = 0;
		ctx->stream->ReadAudioSamples(destination, byte_count, rd);
		return rd;
	}
	uint32 frames = byte_count / 4;
	uint32 rd = 0;
	ctx->stream->ReadAudioSamples(destination, frames * 2, rd);
	uint32 n = rd / 2;
	int16* d = (int16*)destination;
	for (uint32 i = n; i-- > 0; ) {// backwards: 2*i >= i, so no source sample is overwritten before it is read
		int16 s = d[i];
		d[2 * i + 0] = s;
		d[2 * i + 1] = s;
	}
	return n * 4;
}

// Audio bytes the DMA has clocked out, from CT + NDTR. Reports only; the schedule runs on SysTick, so a missed
// buffer pass here costs a wrong log column and nothing else. Two signals are used because one sampling interval
// can exceed a whole buffer pass: CT (CR bit 19) flips once per pass, NDTR reloads at the same moment.
#define _DMA1_S5_CR_ADDR   0x40020088u
#define _DMA1_S5_NDTR_ADDR 0x4002008Cu

static uint32 audio_played_bytes(AviPcmContext& ctx) {
	uint32 items = AUDIO_DMA_BUF_SIZE / 2;
	uint32 cr = *(volatile uint32*)_DMA1_S5_CR_ADDR;
	uint32 ct = (cr >> 19) & 1u;
	uint32 ndtr = *(volatile uint32*)_DMA1_S5_NDTR_ADDR & 0xFFFFu;
	if (ndtr > items) ndtr = items;
	uint32 partial = (items - ndtr) * 2u;
	if (!ctx.started) {
		ctx.started = true;
		ctx.ct = ct;
		ctx.partial = partial;
	} else {
		if (ct != ctx.ct) ctx.passes += 1;// odd number of passes, and one in practice
		else if (partial < ctx.partial) ctx.passes += 2;// same CT yet NDTR reloaded: a whole cycle went by
		ctx.ct = ct;
	}
	ctx.partial = partial;
	return ctx.passes * AUDIO_DMA_BUF_SIZE + partial;
}

#define IMAGE_CARD_BLOCK_SIZE 512
#define IMAGE_FILE_BLOCK_SIZE IMAGE_CARD_BLOCK_SIZE
#define IMAGE_PROFILE         1
#define IMAGE_CARD_CACHE      1
#define IMAGE_CARD_CACHE_BLOCKS 32
#define IMAGE_CARD_CACHE_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_CACHE_BLOCKS)
#define IMAGE_CARD_CACHE_INVALID_BLOCK ((stduint)-1)
#define IMAGE_CARD_READAHEAD  0
#define IMAGE_CARD_READAHEAD_BLOCKS 8
#define IMAGE_CARD_READ_TIMEOUT_MS 1000
#define IMAGE_CARD_READAHEAD_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_READAHEAD_BLOCKS)
#define IMAGE_FAT_BUFFER_SIZE IMAGE_CARD_BLOCK_SIZE

static byte* sector_buf       = (byte*)(SDRAM_BANK1_BASE + 0x100000);
static byte* fat_buf          = sector_buf + IMAGE_FAT_BUFFER_SIZE;
static byte* file_cache_buf   = fat_buf + IMAGE_FAT_BUFFER_SIZE;

#include "../_opendev/_ImageProfile.hpp"

// Cached Block Device that keeps Block_buffer synchronized for StorageReadExact in AVIStream
class VideoFileBlockDevice : public uni::StorageTrait {
private:
	FilesysFAT* fs;
	void* file_handle;
	stduint m_size;
	stduint m_cache_block;
	byte*   m_cache_buf;
public:
	VideoFileBlockDevice(FilesysFAT& f, void* fh, stduint size, byte* buf, stduint blockSize = 512)
		: fs(&f), file_handle(fh), m_size(size), m_cache_block((stduint)-1), m_cache_buf(buf) {
		Block_Size = blockSize;
		Block_buffer = m_cache_buf;
		readable = true;
		writable = false;
	}
	using uni::BlockTrait::Read;
	using uni::BlockTrait::Write;

	bool Read(stduint BlockIden, void* Dest, stduint Times = 1) override {
		if (BlockIden >= getUnits() || BlockIden + Times > getUnits()) return false;
		stduint off = BlockIden * Block_Size;
		if (off >= m_size) return false;
		stduint want = Block_Size * Times;
		if (off + want > m_size) want = m_size - off;
		stduint rd = fs->readfl(file_handle, uni::Slice{ off, want }, (byte*)Dest);
		return rd == want;
	}

	bool Write(stduint BlockIden, const void* Sors, stduint Times = 1) override {
		(void)BlockIden; (void)Sors; (void)Times;
		return false;
	}

	stduint getUnits() override {
		return (m_size + Block_Size - 1) / Block_Size;
	}

	int operator[](uint64 bytid) override {
		if (bytid >= m_size || !m_cache_buf) return -1;
		stduint blk = (stduint)(bytid / Block_Size);
		stduint off = (stduint)(bytid % Block_Size);
		if (blk != m_cache_block) {
			if (!Read(blk, m_cache_buf, 1)) return -1;
			m_cache_block = blk;
		}
		return m_cache_buf[off];
	}
};

// LCD Parameters
#define LCD_W 800
#define LCD_H 480
#define LCD_FB ((uint16*)FMC_SDRAM_BANK1_BASE)

// Colors (RGB565)
#define COLOR_BG        0x0000 // Black
#define COLOR_TOPBAR    0x18E4 // Dark Slate Blue
#define COLOR_WHITE     0xFFFF
#define COLOR_YELLOW    0xFFE0
#define COLOR_GREEN     0x07E0
#define COLOR_RED       0xF800

// AVI File List
#define MAX_VIDEO_FILES 64
static const char* video_filenames[MAX_VIDEO_FILES];
static stduint video_file_count = 0;
static stduint current_video_idx = 0;

static void on_file_enum(void* is_dir, ...) {
	if (is_dir) return;
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	stduint len = StrLength(nm);
	if (len < 4) return;
	const char* ext = nm + len - 4;
	if (StrCompareInsensitive(ext, ".avi") != 0) return;

	if (video_file_count >= MAX_VIDEO_FILES) return;
	char* full = (char*)mempool.allocate(len + 2);
	full[0] = '/';
	MemCopyN(full + 1, nm, len);
	full[len + 1] = 0;
	video_filenames[video_file_count++] = full;
	XART1.OutFormat("  Found AVI [%u]: %s\r\n", (unsigned)(video_file_count - 1), full);
}

static stduint scan_avi_files(FilesysFAT& fs) {
	video_file_count = 0;
	FAT_FileHandle h{};
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	if (!fs.search("/", &sargs)) return 0;
	fs.enumer(&h, on_file_enum);
	return video_file_count;
}

static void draw_text(stduint x, stduint y, const char* str, uint16 fg, uint16 bg) {
	const char* p = str;
	while (*p) {
		char ch = *p++;
		if (ch < 32 || ch > 126) ch = '?';
		ch -= 0x20;
		const uint16* datptr = (const uint16*)&_BITFONT_ASCII_16x8[(byte)ch];
		for (byte col = 0; col < 8; col++) {
			uint16 dat = datptr[col];
			for (byte row = 0; row < 16; row++) {
				stduint px = x + col;
				stduint py = y + (row ^ 0b111);
				if (px < LCD_W && py < LCD_H) {
					LCD_FB[py * LCD_W + px] = (dat & _IMM1S(row)) ? fg : bg;
				}
			}
		}
		x += 8;
	}
}

static void fill_rect(stduint x, stduint y, stduint w, stduint h, uint16 col) {
	for (stduint r = y; r < y + h && r < LCD_H; r++) {
		for (stduint c = x; c < x + w && c < LCD_W; c++) {
			LCD_FB[r * LCD_W + c] = col;
		}
	}
}

void erro(const char* str) {
	(void)str;
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i{ 0 }; i < 1000000; i++) {}
	}
}

// ---- Phase 4 OSD ----
// The video is blitted strictly between VID_TOP and VID_OSD_TOP, so the bars are never painted over by a frame.
#define VID_TOP        30
#define VID_OSD_TOP    (LCD_H - 60)
#define OSD_BAR_X      40
#define OSD_BAR_Y      (LCD_H - 50)
#define OSD_BAR_W      720
#define OSD_BAR_H      10
#define OSD_TIME_Y     (LCD_H - 34)
#define COLOR_BAR_SLOT 0x4208
#define COLOR_BAR_FILL 0x07E0
#define COLOR_OSD_BG   0x0000

static void fmt_mmss(char* out, uint32 sec) {// always 5 characters so a padded redraw leaves no residue
	uint32 m = sec / 60, s = sec % 60;
	out[0] = (char)('0' + (m / 10) % 10);
	out[1] = (char)('0' + m % 10);
	out[2] = ':';
	out[3] = (char)('0' + s / 10);
	out[4] = (char)('0' + s % 10);
	out[5] = 0;
}

// unisym's formatter has NO case '-' (lib/cpp/stream.cpp:122 only carries a comment for it) and its %s ignores
// the width field entirely, so "%-4s" is emitted literally as "-4s". Pad by hand instead.
static void pad_to(char* buf, stduint width) {
	stduint n = StrLength(buf);
	while (n < width && n < 120) buf[n++] = ' ';
	buf[n] = 0;
}

static void copy_pad(char* dst, const char* src, stduint width) {
	stduint n = StrLength(src);
	if (n > width) n = width;
	MemCopyN(dst, src, n);
	dst[n] = 0;
	pad_to(dst, width);
}

static void draw_progress(uint32 done, uint32 total) {
	if (!total) return;
	uint32 w = (uint32)(((uint64)done * OSD_BAR_W) / total);
	if (w > OSD_BAR_W) w = OSD_BAR_W;
	fill_rect(OSD_BAR_X, OSD_BAR_Y, OSD_BAR_W, OSD_BAR_H, COLOR_BAR_SLOT);
	if (w) fill_rect(OSD_BAR_X, OSD_BAR_Y, w, OSD_BAR_H, COLOR_BAR_FILL);
}

// ---- Phase 4 keys: EXTI interrupt driven, following the 03-Interrupt pattern ----
// The ISR latches the press edge and its timestamp and does nothing else. Debouncing and the action stay in the
// main loop: the loop can be a whole frame (154 ms) away, so a short tap would be invisible to a level poll, and
// a blocking wait inside the ISR would delay the audio DMA.
#define KEY_DEBOUNCE_MS 20
#define KEY_REPEAT_MS   600

struct KeyDeb {
	GPIN* pin;
	bool active_high;
	volatile uint32 cnt;// ISR: press edges seen
	volatile uint32 ms;// ISR: tick of the latest one
	uint32 seen;// main loop: edges consumed so far
	uint32 t_press;// main loop: when the current press was accepted
	bool down;
	bool fresh;
};

static void key_isr(KeyDeb* k) {
	k->cnt++;
	k->ms = (uint32)SysTick::getTick();
}

static KeyDeb k_up { &KEYU, true,  0, 0, 0, 0, false, false };
static KeyDeb k_dn { &KEYD, false, 0, 0, 0, 0, false, false };
static KeyDeb k_lf { &KEYL, false, 0, 0, 0, 0, false, false };
static KeyDeb k_rt { &KEYR, false, 0, 0, 0, 0, false, false };

static void on_key_up() { key_isr(&k_up); }
static void on_key_dn() { key_isr(&k_dn); }
static void on_key_lf() { key_isr(&k_lf); }
static void on_key_rt() { key_isr(&k_rt); }

static void key_poll(KeyDeb& k, uint32 now) {
	if (k.cnt != k.seen) {
		k.seen = k.cnt;
		// Accept only once the line has been quiet for the debounce window and the previous press is older than it
		if ((now - k.ms) >= KEY_DEBOUNCE_MS && (now - k.t_press) >= KEY_DEBOUNCE_MS) {
			k.t_press = now;
			k.fresh = true;
		}
	}
	k.down = k.active_high ? (bool)*(k.pin) : !(bool)*(k.pin);
}

static bool key_take(KeyDeb& k) {
	if (!k.fresh) return false;
	k.fresh = false;
	return true;
}

// Repeat-capable variant for LT/RT: one step on the press, then one more every KEY_REPEAT_MS while it is held.
// A tap that press+releases between two polls still steps once, because the ISR latched the edge.
static bool key_steps(KeyDeb& k, uint32 now) {
	if (k.fresh) {
		k.fresh = false;
		k.t_press = now;
		return true;
	}
	if (k.down && (now - k.t_press) >= KEY_REPEAT_MS) {
		k.t_press = now;
		return true;
	}
	return false;
}

/*
 * 93-VideoPlay (Phase 4): 交互控制与 OSD
 *
 * 按键规则（全部走 EXTI 中断触发，ISR 只锁存边沿+时间戳，消抖与动作在主循环）：
 *   KEY_DN  PH2  低有效   短按：播放 / 暂停 切换
 *   KEY_LT  PC13 低有效   快退 −10 秒（按住每 600ms 再退一次）
 *   KEY_RT  PH3  低有效   快进 +10 秒（按住每 600ms 再进一次）
 *   KEY_UP  PA0  高有效   短按：下一个视频
 *
 * 判定说明：
 *   主循环离一次可能 154ms，短按若只靠轮询电平会整个丢掉，所以 ISR 锁存边沿与时间戳，主循环按锁存消费。
 *   快进/快退后必须重锚 t_play_start —— 调度器是跟着内容时钟走的，不重锚则下一轮会跳回原处。
 *
 * 画面布局：
 *   y 0..28     顶栏：文件名 / 分辨率 / 帧率 / 第几个视频
 *   y 30..419   视频视口（blit 严格裁剪在此区间，OSD 不会被帧覆盖）
 *   y 430..440  进度条    y 446..462  时间与状态    y 466..480  按键提示
 *
 * 中断优先级：按键 2，音频 DMA 0（按键绝不迟延音频回填）。
 */
int main() {
#if MPU_SDRAM_CACHEABLE
	mpu_sdram_init();// must precede the D-Cache enable, else lines are allocated under the old attributes
#endif
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro("Clock config failed");

	// Mempool setup
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{ SDRAM_POOL_BASE, SDRAM_POOL_SIZE });
	mempool.Append(Slice{ SRAM12_ADDR + AUDIO_SRAM_BYTES, 32U * 1024U });// audio ring takes SRAM1's lower 96KB, this is the rest
	mempool.Append(Slice{ SRAM4_ADDR,  32U * 1024U });

	// IO Setup (Up, Dn, Lf, Rt)
	LEDB.setMode(GPIOMode::OUT) = !false; // DS0
	LEDR.setMode(GPIOMode::OUT) = !false; // DS1
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false); // Up
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true); // Lf
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true); // Dn
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true); // Rt

	// Keys via EXTI (03-Interrupt pattern): arm the press edge, attach the latch ISR, enable NVIC.
	// Priority 2 sits below the audio DMA's 0, so a key press can never delay an audio refill.
	KEYU.setMode(GPIORupt::Posedge); // WK_UP is active-high
	KEYU.setInterrupt(on_key_up);
	KEYU.setInterruptPriority(2, 0);
	KEYU.enInterrupt();
	KEYD.setMode(GPIORupt::Negedge);
	KEYD.setInterrupt(on_key_dn);
	KEYD.setInterruptPriority(2, 0);
	KEYD.enInterrupt();
	KEYL.setMode(GPIORupt::Negedge);
	KEYL.setInterrupt(on_key_lf);
	KEYL.setInterruptPriority(2, 0);
	KEYL.enInterrupt();
	KEYR.setMode(GPIORupt::Negedge);
	KEYR.setInterrupt(on_key_rt);
	KEYR.setInterruptPriority(2, 0);
	KEYR.enInterrupt();

	XART1.setMode(115200);
	XART1.OutFormat("\r\n========================================\r\n");
	XART1.OutFormat("  Alice.Wiki 93-VideoPlay (AVICodec)\r\n");
	XART1.OutFormat("========================================\r\n");
	XART1.OutFormat("SDRAM pool @0x%08X, MPU cache: %s\r\n", (unsigned)SDRAM_POOL_BASE, MPU_SDRAM_CACHEABLE ? "0xC1000000 WBWA / 0xC0000000 NC" : "OFF");
	g_sysclk_mhz = RCC.Sysclock.getCoreFrequency() / 1000000U;
	if (!g_sysclk_mhz) g_sysclk_mhz = 480;
	XART1.OutFormat("SYSCLK %u MHz, DWT cyccnt: %s\r\n", (unsigned)g_sysclk_mhz, dwt_init() ? "on" : "OFF");

	// Display & SDRAM setup
	sdram_init();
	ltdc_init();
	XART1.OutFormat("LTDC & SDRAM OK\r\n");

	// Audio codec: ES8388 at 0x10, otherwise WM8978 at 0x1A, over software I2C on PH4/PH5
	audio_iic = new (audio_wire_buf) AudioIIC(GPIOH[5], GPIOH[4]);
	audio_iic->func_delay = audio_iic_delay;
	es8388 = new (es8388_buf) ES8388_t(*audio_iic);
	(void)es8388->Read(0);
	if (audio_iic->getError() == ERR_IIC_NONE && es8388->Initialize()) {
		es8388->SetADDA(true, false);
		es8388->SetOutput(true, true);
		es8388->SetHPVol(10);
		es8388->SetSPKVol(10);
		audio_chip = es8388;
		XART1.OutFormat("Audio Codec: ES8388 OK\r\n");
	} else {
		wm8978 = new (wm8978_buf) WM8978_t(*audio_iic);
		if (wm8978->Initialize()) {
			wm8978->SetADDA(true, false);
			wm8978->SetInput(false, false, false);
			wm8978->SetOutput(true, false);
			wm8978->SetHPVol(35, 35);
			wm8978->SetSPKVol(28);
			audio_chip = wm8978;
			XART1.OutFormat("Audio Codec: WM8978 OK\r\n");
		} else {
			XART1.OutFormat("Audio Codec: none detected, playback will be silent\r\n");
		}
	}

	// SD Card & FATFS setup
	if (!SDCard1.setMode()) {
		XART1.OutFormat("SD Card Init Failed!\r\n");
		erro("SD Init Failed");
	}
	SDCard1.Block_Size = IMAGE_CARD_BLOCK_SIZE;

#if IMAGE_CARD_CACHE
	CachedStorageDevice cached_sd(SDCard1);
	FilesysFAT fs(0, cached_sd, sector_buf, fat_buf);
#else
	FilesysFAT fs(0, SDCard1, sector_buf, fat_buf);
#endif

	if (!fs.loadfs()) {
		XART1.OutFormat("FATFS Load Failed! err=%u\r\n", (unsigned)fs.error_number);
		erro("FAT Load Failed");
	}
	XART1.OutFormat("FAT%u OK\r\n", (unsigned)fs.fat_type);

	scan_avi_files(fs);
	if (video_file_count == 0) {
		XART1.OutFormat("No .avi files found!\r\n");
		fill_rect(0, 0, LCD_W, LCD_H, 0x3180);
		draw_text(20, 20, "No .avi files found on SD card root.", COLOR_WHITE, 0x3180);
		while (true) { LEDB.Toggle(); SysDelay_ms(500); }
	}

	AVICodec avi_codec;
	current_video_idx = 0;
	bool paused = false;

	while (true) {
		const char* current_file = video_filenames[current_video_idx];

		FAT_FileHandle fh{};
		FilesysSearchArgs sargs{};
		sargs.handle_buffer = &fh;

		void* fh_ptr = fs.search(current_file, &sargs);
		if (!fh_ptr || fh.is_dir || !fh.size) {
			XART1.OutFormat("Failed to open file: %s\r\n", current_file);
			current_video_idx = (current_video_idx + 1) % video_file_count;
			SysDelay_ms(1000);
			continue;
		}

		VideoFileBlockDevice file_dev(fs, fh_ptr, (stduint)fh.size, file_cache_buf, IMAGE_FILE_BLOCK_SIZE);
		IVideoStream* stream = nullptr;

		XART1.OutFormat("Opening AVI stream for %s (size %u B)...\r\n", current_file, (unsigned)fh.size);

		VideoResult vr = avi_codec.OpenStream(file_dev, stream, mempool);
		if (vr != VideoResult::OK || !stream) {
			XART1.OutFormat("avi_codec.OpenStream failed: %d\r\n", (int)vr);
			current_video_idx = (current_video_idx + 1) % video_file_count;
			SysDelay_ms(1000);
			continue;
		}

		VideoInfo vinfo{};
		stream->GetInfo(vinfo);

		uint32 fps = (vinfo.videoFormat.frame_rate_den > 0)
			? (vinfo.videoFormat.frame_rate_num / vinfo.videoFormat.frame_rate_den) : 30;
		if (fps == 0) fps = 30;

		// Real-time pacing: frame i is due at i*1000*den/num ms of wall clock, and a frame whose slot already passed is skipped without being decoded (MJPEG is all-intra, so SeekFrame is just a counter assignment).
		uint32 rate_num = vinfo.videoFormat.frame_rate_num ? vinfo.videoFormat.frame_rate_num : 30;
		uint32 rate_den = vinfo.videoFormat.frame_rate_den ? vinfo.videoFormat.frame_rate_den : 1;
		uint32 t_play_start = SysTick::getTick();
		uint32 prev_cost_ms = 0;// decision -> on-screen cost of the previous frame, excluding any waiting
		stduint dropped_last = 0;
		stduint dropped_total = 0;

		// Audio: start the SAI1 + DMA1 ring for this file; the engine consumes 16-bit stereo, so only S16LE is fed directly
		uni::AudioBridge* bridge = nullptr;
		AviPcmContext pcm_ctx{};
		uint32 audio_byte_rate = 0;
		bool audio_on = false;
		if (vinfo.hasAudio && vinfo.audioInfo.format.sample_format == AudioSampleFormat::S16LE
			&& vinfo.audioInfo.format.sample_rate) {
			uint32 arate = vinfo.audioInfo.format.sample_rate;
			if (!uni::AudioBridge::isRateSupported(arate)) {
				XART1.OutFormat("[AUDIO] %u Hz unsupported by SAI/PLL2, silent\r\n", (unsigned)arate);
			} else {
				pcm_ctx.stream = stream;
				pcm_ctx.mono = (vinfo.audioInfo.format.channels == 1);
				audio_byte_rate = arate * 4u;// 16-bit stereo
				bridge = new (bridge_buf) uni::AudioBridge(audio_ring, AUDIO_RING_SLOTS, AUDIO_DMA_BUF_SIZE, audio_chip);
				if (bridge->StartStream(vinfo.audioInfo.format, avi_pcm_refill, &pcm_ctx)) {
					audio_on = true;
					XART1.OutFormat("[AUDIO] %u Hz, %u ch, S16LE, playing\r\n",
						(unsigned)arate, (unsigned)vinfo.audioInfo.format.channels);
				} else {
					XART1.OutFormat("[AUDIO] StartStream failed, silent\r\n");
					bridge = nullptr;
				}
			}
		} else if (vinfo.hasAudio) {
			XART1.OutFormat("[AUDIO] source sample format is not S16LE, silent\r\n");
		} else {
			XART1.OutFormat("[AUDIO] this file has no audio stream\r\n");
		}
		t_play_start = SysTick::getTick();// anchor the video clock where the audio really starts, after the DMA prefill

		XART1.OutFormat("Stream Open OK: %ux%u @ %u FPS, Total: %u frames, CodecType: %d\r\n",
			(unsigned)vinfo.videoFormat.width, (unsigned)vinfo.videoFormat.height,
			(unsigned)fps, (unsigned)vinfo.totalFrames, (int)vinfo.videoFormat.codec_type);

		// Clear screen, draw the top info bar once, and reserve the bottom strip for the OSD
		fill_rect(0, 0, LCD_W, LCD_H, COLOR_BG);
		fill_rect(0, 0, LCD_W, 28, COLOR_TOPBAR);
		char infobuf[128];
		char namebuf[48];
		copy_pad(namebuf, current_file, 20);
		outsfmtbuf(infobuf, " 93-VideoPlay  %s %ux%u @%ufps  [%u/%u]",
			namebuf, (unsigned)vinfo.videoFormat.width, (unsigned)vinfo.videoFormat.height,
			(unsigned)fps, (unsigned)(current_video_idx + 1), (unsigned)video_file_count);
		draw_text(8, 6, infobuf, COLOR_WHITE, COLOR_TOPBAR);
		draw_text(8, LCD_H - 18, "[Dn]Play/Pause  [Lt]<<10s  [Rt]>>10s  [Up]Next", COLOR_WHITE, COLOR_OSD_BG);

		stduint frame_idx = 0;
		bool switch_video = false;
		VideoFrame frame{};
		VideoFrameClear(frame);
		uint32 osd_sec = (uint32)-1;
		bool osd_dirty = true;

		while (!switch_video) {
			// Key controls, debounced and edge-triggered; KEYU is active-high, KEYL/KEYD/KEYR are active-low
			uint32 now_tick = SysTick::getTick();
			key_poll(k_up, now_tick);
			key_poll(k_dn, now_tick);
			key_poll(k_lf, now_tick);
			key_poll(k_rt, now_tick);

			if (key_take(k_dn)) { // Dn: pause / resume
				paused = !paused;
				if (paused) {
					if (audio_on) { bridge->PauseStream(); bridge->ServicePlayback(); }
				} else {
					if (audio_on) bridge->ResumeStream();
				}
				osd_dirty = true;
			}
			if (key_take(k_up)) { // Up: next video
				current_video_idx = (current_video_idx + 1) % video_file_count;
				switch_video = true;
				break;
			}
			// Lt / Rt: step the position back / forward by 10 s, repeating while held. Re-anchoring t_play_start is
			// what makes a seek stick: the scheduler walks the content clock, so without it the next iteration
			// would seek straight back to where the clock still is.
			int32 seek_ms = 0;
			if (key_steps(k_rt, now_tick)) seek_ms += 10000;
			if (key_steps(k_lf, now_tick)) seek_ms -= 10000;
			if (seek_ms) {
				uint32 dur_ms = vinfo.durationMs;
				uint32 cur_ms = (uint32)(((uint64)frame_idx * 1000 * rate_den) / rate_num);
				uint32 dst_ms = (seek_ms > 0)
					? (cur_ms + (uint32)seek_ms)
					: ((cur_ms > (uint32)(-seek_ms)) ? (cur_ms - (uint32)(-seek_ms)) : 0u);
				if (dur_ms && dst_ms + 100 >= dur_ms) dst_ms = dur_ms - 100;
				if (dst_ms != cur_ms && stream->SeekTime(dst_ms) == VideoResult::OK) {
					frame_idx = (uint32)(((uint64)dst_ms * rate_num) / ((uint64)1000 * rate_den));
					t_play_start = now_tick - dst_ms;
					prev_cost_ms = 0;
					dropped_last = 0;
					osd_dirty = true;
				}
			}

			if (paused) {
				if (audio_on) { bridge->PauseStream(); bridge->ServicePlayback(); }// keeps silence feeding the DMA
				LEDB.Toggle();
				SysDelay_ms(50);
				t_play_start += 50;// hold the wall clock so the schedule does not advance while paused
				continue;
			}
			if (audio_on) bridge->ResumeStream();
			if (audio_on) (void)audio_played_bytes(pcm_ctx);// sample twice per frame so the log column cannot miss a whole buffer

			// Chase the wall clock: skip whatever is already past its slot, without decoding it. Aim at the instant the
			// frame will actually appear, using the previous frame's decision->on-screen cost. That cost must NOT
			// include the wait below, or waiting inflates the next jump and the loop feeds back on itself.
			uint32 t_dec = SysTick::getTick();
			uint32 now_ms = t_dec - t_play_start;
			uint32 target = (uint32)(((uint64)(now_ms + prev_cost_ms) * rate_num) / ((uint64)1000 * rate_den));
			if (vinfo.totalFrames && target >= vinfo.totalFrames) {
				XART1.OutFormat("[END] %u frames played, %u dropped\r\n", (unsigned)vinfo.totalFrames, (unsigned)dropped_total);
				break;
			}
			dropped_last = 0;
			if (target > frame_idx) {
				dropped_last = (stduint)(target - frame_idx);
				stream->SeekFrame(target);
				frame_idx = target;
				dropped_total += dropped_last;
			}

			// Read and decode next frame via AVICodec
			uint32 sd_ms_before = (uint32)image_profile.card_read_ms;
			uint32 sd_blk_before = (uint32)image_profile.card_read_block_count;
			uint32 t_dec_start = dwt_cyccnt();

			vr = stream->ReadVideoFrame(frame, mempool);
			if (vr != VideoResult::OK || !frame.image.pixels) {
				XART1.OutFormat("ReadVideoFrame FAILED: vr=%d, frame_idx=%u, pixels=%p\r\n",
					(int)vr, (unsigned)frame_idx, frame.image.pixels);
				SysDelay_ms(1000);
				break;
			}

			uint32 dec_cyc = dwt_cyccnt() - t_dec_start;
			uint32 sd_ms = (uint32)image_profile.card_read_ms - sd_ms_before;
			uint32 sd_blk = (uint32)image_profile.card_read_block_count - sd_blk_before;
			frame_idx++;

			stduint dst_w = frame.width;
			stduint dst_h = frame.height;
			stduint ox = (LCD_W > dst_w) ? (LCD_W - dst_w) / 2 : 0;
			stduint oy = (VID_OSD_TOP > dst_h + VID_TOP) ? VID_TOP + (VID_OSD_TOP - VID_TOP - dst_h) / 2 : VID_TOP;

			uint32 t_blit_start = dwt_cyccnt();

			// Direct blit pixels to SDRAM framebuffer, clipped to the viewport so the OSD strips survive
			if (frame.image.format == PixelFormat::RGB565) {
				const uint16* src = (const uint16*)frame.image.pixels;
				for (stduint r = 0; r < dst_h && (oy + r) < VID_OSD_TOP; r++) {
					MemCopyN(LCD_FB + (oy + r) * LCD_W + ox, src + r * dst_w, dst_w * sizeof(uint16));
				}
			} else {
				const Color* src = (const Color*)frame.image.pixels;
				for (stduint r = 0; r < dst_h && (oy + r) < VID_OSD_TOP; r++) {
					uint16* dst_row = LCD_FB + (oy + r) * LCD_W + ox;
					const Color* src_row = src + r * dst_w;
					for (stduint c = 0; c < dst_w; c++) {
						dst_row[c] = src_row[c].ToRGB565();
					}
				}
			}

			uint32 blit_cyc = dwt_cyccnt() - t_blit_start;

			// Second service point per frame: the decode phase is the longest gap the DMA has to survive
			if (audio_on) bridge->ServicePlayback();
			prev_cost_ms = SysTick::getTick() - t_dec;// decision -> on-screen, measured after the blit and the refill

			// Audio content position vs this frame's presentation time: the lip-sync offset, and whether it grows
			int32 av_off_ms = 0;
			if (audio_on) {
				uint32 audio_ms = (uint32)(((uint64)audio_played_bytes(pcm_ctx) * 1000) / audio_byte_rate);
				uint32 video_ms = (uint32)(((uint64)frame_idx * 1000 * rate_den) / rate_num);
				av_off_ms = (int32)audio_ms - (int32)video_ms;
			}

			XART1.OutFormat("[FRAME] %u: Decode %u us (SD %u ms/%u blk), Blit %u us, skipped %u/%u, audio %d ms\r\n",
				(unsigned)frame_idx,
				(unsigned)(dec_cyc / g_sysclk_mhz), (unsigned)sd_ms, (unsigned)sd_blk,
				(unsigned)(blit_cyc / g_sysclk_mhz),
				(unsigned)dropped_last, (unsigned)dropped_total,
				(int)av_off_ms);

			if (frame_idx == 1) {
				XART1.OutFormat("Frame 1 displayed! (%ux%u at %u,%u)\r\n", (unsigned)dst_w, (unsigned)dst_h, (unsigned)ox, (unsigned)oy);
			}

			VideoFrameFree(frame);

			// OSD: redraw only when the displayed second or the play state changes, so this costs ~2.5 ms per second
			uint32 cur_sec = frame_idx / (fps ? fps : 30);
			if (osd_dirty || cur_sec != osd_sec) {
				osd_sec = cur_sec;
				osd_dirty = false;
				draw_progress(frame_idx, vinfo.totalFrames);
				char tb[64], ta[8], tz[8];
				uint32 total_sec = fps ? (vinfo.totalFrames / fps) : 0;
				fmt_mmss(ta, cur_sec);
				fmt_mmss(tz, total_sec);
				outsfmtbuf(tb, "%s / %s   %s   %u/%u", ta, tz, paused ? "PAUSE" : "PLAY",
					(unsigned)frame_idx, (unsigned)vinfo.totalFrames);
				pad_to(tb, 40);
				draw_text(OSD_BAR_X, OSD_TIME_Y, tb, COLOR_WHITE, COLOR_OSD_BG);
				LEDB.Toggle();
			}

			// If the picture is ahead of the clock, wait for it; sleep in short steps so the DMA stays fed
			uint32 this_pts = (uint32)(((uint64)(frame_idx - 1) * 1000 * rate_den) / rate_num);
			uint32 slot_ms = (uint32)(((uint64)1000 * rate_den) / rate_num);
			uint32 waited = 0;
			while (waited < slot_ms) {// capped at one frame: this only absorbs sub-frame rounding, never a real lag
				uint32 now2 = SysTick::getTick() - t_play_start;
				if (now2 >= this_pts) break;
				uint32 step = this_pts - now2;
				if (step > slot_ms - waited) step = slot_ms - waited;
				if (!step) break;
				SysDelay_ms(step);
				waited += step;
				if (audio_on) bridge->ServicePlayback();
			}
		}

		if (audio_on) bridge->StopStream();// stop the DMA before the stream it refills goes away
		stream->Release();

		if (!switch_video) {
			current_video_idx = (current_video_idx + 1) % video_file_count;
		}
	}
}
