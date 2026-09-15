// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 91-ImageShow-BaseOn20：SD 卡 FAT 文件系统枚举 JPG 图片，硬件 JPEG 解码后显示到 RGB-LCD，
 * 通过方向键在图片之间切换。
 *
 * 依赖：
 *   - SDMMC1（板载 SD 卡座）：SecureDigitalCard_t SDCard1
 *   - FAT 文件系统：FilesysFAT
 *   - 硬件 JPEG 解码：JPEGCodecHard（内部 JPEG_HARD + DMA2D YCbCr->RGB）
 *   - LTDC + SDRAM：_opendev/RGB-LCD.hpp（RGB-LCD.cpp / SDRAM.cpp 加入工程）
 *
 * 使用说明：
 *   1) SD 卡根目录放置若干 .jpg 图片（建议宽高不超过屏幕 800x480，宽高为 16 的倍数）。
 *   2) 上电初始化 SD + FAT + LCD，枚举根目录 JPG，显示第一张。
 *   3) 按键 KEY0(右)/KEY2(左) 切换上一张/下一张；KEY_UP 重新枚举。
 *   4) LTDC 层为 RGB565，帧缓冲位于 SDRAM（0xC0000000），解码输出 RGB565 直接写入。
 */
#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SD.hpp>
#include <cpp/Device/GPU>
#include <c/format/filesys/FAT.h>
#include <c/format/picture/JPEG.h>
#include <c/format/picture/PNG.h>
#include <c/format/picture/BMP.h>
#include <c/mempool.h>
#include <cpp/Device/DBG>
#include "../_opendev/RGB-LCD.hpp"
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;


extern uni::Mempool mempool;

// 板载外设
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// Up   (WK_UP)
GPIN& KEYL = GPIOC[13];// Left (KEY2)
GPIN& KEYD = GPIOH[ 2];// Down (KEY1)
GPIN& KEYR = GPIOH[ 3];// Right(KEY0)

void outtxt(const char* str, stduint len) {XART1.out(str, len);}


// Mempool 管理的内存切片（照 80-Memman：SDRAM + SRAM12 + SRAM4，不用 AXI/ITCM）。
// 避开 LTDC 帧缓冲（SDRAM_BANK1_BASE 起 ~0xC0000）与下方 FAT 缓冲（+0x100000 起 1KB）。
#define SDRAM_POOL_BASE (SDRAM_BANK1_BASE + 0x100000 + 0x1000)
#define SDRAM_POOL_SIZE (4U * 1024U * 1024U)
#define SRAM12_ADDR ((stduint)0x30000000)
#define SRAM4_ADDR  ((stduint)0x38000000)

#define IMAGE_CARD_BLOCK_SIZE 512
#define IMAGE_FILE_BLOCK_SIZE IMAGE_CARD_BLOCK_SIZE
#define PNG_FILE_BLOCK_SIZE 4096
#define IMAGE_PROFILE 1
#define IMAGE_CARD_CACHE 1
#define IMAGE_CARD_CACHE_BLOCKS 32
#define IMAGE_CARD_CACHE_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_CACHE_BLOCKS)
#define IMAGE_CARD_CACHE_INVALID_BLOCK ((stduint)-1)
#define IMAGE_CARD_READAHEAD 0 // !!
#define IMAGE_CARD_READAHEAD_BLOCKS 8
#define IMAGE_CARD_READ_TIMEOUT_MS 1000
#define IMAGE_CARD_READAHEAD_BYTES (IMAGE_CARD_BLOCK_SIZE * IMAGE_CARD_READAHEAD_BLOCKS)
#define IMAGE_FAT_BUFFER_SIZE IMAGE_CARD_BLOCK_SIZE
#if IMAGE_CARD_READAHEAD_BLOCKS > IMAGE_CARD_CACHE_BLOCKS
#error IMAGE_CARD_READAHEAD_BLOCKS must not exceed IMAGE_CARD_CACHE_BLOCKS
#endif
// FAT / JPEG 缓冲（外部 SDRAM，避开 LTDC 帧缓冲与 MEMPOOL 切片）
static byte* sector_buf = (byte*)(SDRAM_BANK1_BASE + 0x100000); // 扇区缓冲 512B（1MB 处）
static byte* fat_buf    = sector_buf + IMAGE_FAT_BUFFER_SIZE;    // FAT 表缓冲 512B
#define MAX_PIC 64
static const char* pic_name[MAX_PIC];                   // 图片路径数组
static stduint pic_count = 0;                           // 图片数量

// LTDC 帧缓冲：RGB565，800x480，位于 SDRAM
#define LCD_W 800
#define LCD_H 480
#define LCD_FB ((uint16*)FMC_SDRAM_BANK1_BASE)

#include "../_opendev/_ImageProfile.hpp"
#include "../_opendev/_FileBlockDevice.hpp"   // 共享: FAT 文件 → 按块 StorageTrait (原 86-144 行本地类已抽出)



// (FileBlockDevice 已抽到 ../_opendev/_FileBlockDevice.hpp, 见上方 include)

// 枚举回调：_tocall_ft 是变参函数指针，FAT 按 (is_dir, name) 两参调用（见 FAT.cpp:531）
// C++ 中 lambda/普通函数不能隐式转变参函数指针，故用真正的变参函数。
static void on_seg(void* is_dir, ...) {
	if (is_dir) return;
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	stduint len = StrLength(nm);
	XART1.OutFormat("  file: '%s' len=%u\r\n", nm, (unsigned)len);
	if (len < 4) return;
	const char* ext = nm + len - 4;
	// XART1.OutFormat("  ext: '%s'\r\n", ext);
	// 只收集 .jpg/.jpeg/.png/.bmp（不区分大小写，FAT 短名为大写如 .JPG）
	if (StrCompareInsensitive(ext, ".jpg") && StrCompareInsensitive(ext, ".jpeg") &&
		StrCompareInsensitive(ext, ".png") && StrCompareInsensitive(ext, ".bmp")) return;
	// XART1.OutFormat("  -> jpg\r\n");
	if (pic_count >= MAX_PIC) return;
	char* full = (char*)mempool.allocate(len + 2);
	full[0] = '/';
	MemCopyN(full + 1, nm, len);
	full[len + 1] = 0;
	pic_name[pic_count++] = full;
}

// 枚举根目录下所有 .jpg 文件，收集到 pic_name
static stduint collect_jpg(FilesysFAT& fs) {
	pic_count = 0;
	FAT_FileHandle h;
	FilesysSearchArgs sargs{};
	sargs.handle_buffer = &h;
	if (!fs.search("/", &sargs)) return 0;
	fs.enumer(&h, on_seg);
	return pic_count;
}



// Stream PNG scanlines as RGB565 and scale directly into the LCD framebuffer.
static ImageResult show_png_stream(FileBlockDevice& mem, PNGCodec& png, trait::Malloc& allocator, const ImageDecodeOptions& dopt) {
	IImageSurface* surface = nullptr;
#if IMAGE_PROFILE
	uint64 t0 = profile_now();
#endif
	ImageResult r = png.OpenSurface(mem, surface, allocator, dopt, ImageAccessMode::READ_ONLY);
#if IMAGE_PROFILE
	image_profile.png_open_ms += profile_now() - t0;
#endif
	if (r != ImageResult::OK || !surface) return r;

	ImageInfo info;
	r = surface->GetInfo(info);
	if (r != ImageResult::OK || !info.width || !info.height) {
		surface->Release();
		return r == ImageResult::OK ? ImageResult::INVALID_FORMAT : r;
	}

	stduint dst_w = 0, dst_h = 0;
	PictureOperation::FitAspect(info.width, info.height, LCD_W, LCD_H, dst_w, dst_h);
	stduint row_size = info.width * sizeof(uint16);
	uint16* src_row = (uint16*)allocator.allocate(row_size, 1);
	if (!src_row) {
		surface->Release();
		return ImageResult::OUT_OF_MEMORY;
	}

	ImageBuffer row;
	ImageBufferClear(row);
	row.width = info.width;
	row.height = 1;
	row.stride = (uint32)row_size;
	row.format = PixelFormat::RGB565;
	row.colorSpace = ColorSpace::SRGB;
	row.alphaMode = ImageAlphaMode::NONE;
	row.pixels = src_row;
	row.size = row_size;
	row.allocator = nullptr;

	stduint ox = (LCD_W > dst_w) ? (LCD_W - dst_w) / 2 : 0;
	stduint oy = (LCD_H > dst_h) ? (LCD_H - dst_h) / 2 : 0;
	stduint last_sy = (stduint)-1;
	for (stduint dy = 0; dy < dst_h; dy++) {
		stduint sy = dy * info.height / dst_h;
		if (sy != last_sy) {
			Rectangle src_rect(Point(0, sy), Size2(info.width, 1));
#if IMAGE_PROFILE
			t0 = profile_now();
#endif
			r = surface->ReadPixels(src_rect, row, allocator);
#if IMAGE_PROFILE
			image_profile.png_read_ms += profile_now() - t0;
			image_profile.png_rows++;
#endif
			if (r != ImageResult::OK) break;
			last_sy = sy;
		}
		uint16* dst = LCD_FB + (oy + dy) * LCD_W + ox;
		uint32 sx_acc = 0;
		uint32 sx_step = ((uint32)info.width << 16) / (uint32)dst_w;
#if IMAGE_PROFILE
		t0 = profile_now();
#endif
		for (stduint dx = 0; dx < dst_w; dx++) {
			dst[dx] = src_row[sx_acc >> 16];
			sx_acc += sx_step;
		}
#if IMAGE_PROFILE
		image_profile.png_draw_ms += profile_now() - t0;
		image_profile.png_pixels += dst_w;
#endif
	}

	allocator.deallocate(src_row, row_size);
	surface->Release();
	if (r == ImageResult::OK) {
		XART1.OutFormat("show PNG %ux%u -> %ux%u\r\n",
			(unsigned)info.width, (unsigned)info.height, (unsigned)dst_w, (unsigned)dst_h);
	}
	return r;
}

// ---- 按键中断（EXTI）回调：只置事件标志，处理放主循环 ----
static volatile byte key_event = 0;
static void on_key_next() { key_event = 1; }// KEYR：下一张
static void on_key_prev() { key_event = 2; }// KEYL：上一张
static void on_key_enum(){ key_event = 3; }// KEYU：重新枚举

int main() {
	L1C.enAble();// I+D cache 全开（照 20-LTDC-DMA2D：SDRAM + LTDC + DMA2D 场景）
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();

	// 初始化 Mempool：登记 SDRAM + SRAM12 + SRAM4 切片，关闭自动扩展
	mempool.enable_auto_expand = false;
	mempool.Append(Slice{ SDRAM_POOL_BASE, SDRAM_POOL_SIZE });
	mempool.Append(Slice{ SRAM12_ADDR, 64U * 1024U });
	mempool.Append(Slice{ SRAM4_ADDR,  32U * 1024U });

	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);

	KEYR.setMode(GPIORupt::Negedge, on_key_next);
	KEYR.setInterruptPriority(2, 0);
	KEYR.enInterrupt();
	KEYL.setMode(GPIORupt::Negedge, on_key_prev);
	KEYL.setInterruptPriority(2, 0);
	KEYL.enInterrupt();
	KEYU.setMode(GPIORupt::Posedge, on_key_enum);
	KEYU.setInterruptPriority(2, 0);
	KEYU.enInterrupt();

	XART1.setMode(115200);
	XART1.OutFormat("ImageShow TEST\r\n");

	sdram_init();
	ltdc_init();
	XART1.OutFormat("LTDC OK\r\n");

	DBGMCU.enDBGStandby(true);         // Domain1 待机调试（DBGMCU_CR.STANDBYD1）
	DBGMCU.enDBGStandbyDomain3(true);  // Domain3 待机调试（保持 SWD 口）

	// SD 卡初始化
	if (!SDCard1.setMode()) {
		XART1.OutFormat("SD Error!\r\n");
		while (1) { SysDelay_ms(1000); }
	}
	SDCard1.Block_Size = IMAGE_CARD_BLOCK_SIZE;

	// 挂载 FAT
#if IMAGE_CARD_CACHE
	CachedStorageDevice cached_sd(SDCard1);
	FilesysFAT fs(0, cached_sd, sector_buf, fat_buf);
#elif IMAGE_PROFILE
	ProfileStorageDevice profiled_sd(SDCard1);
	FilesysFAT fs(0, profiled_sd, sector_buf, fat_buf);
#else
	FilesysFAT fs(0, SDCard1, sector_buf, fat_buf);
#endif
	if (!fs.loadfs()) {
		XART1.OutFormat("FAT mount FAIL %u\r\n", (unsigned)fs.error_number);
		while (1) { SysDelay_ms(1000); }
	}
	XART1.OutFormat("FAT%u OK\r\n", (unsigned)fs.fat_type);

	// 枚举图片
	collect_jpg(fs);
	XART1.OutFormat("%u JPG found\r\n", (unsigned)pic_count);
	if (!pic_count) {
		XART1.OutFormat("No JPG\r\n");
		while (1) { SysDelay_ms(1000); }
	}

	stduint cur = 0;
	JPEGCodecHard hw(JPEG);// 硬件 JPEG codec（用户注入 JPEG 对象）
	JPEGCodec soft;// 软件 JPEG codec（硬件失败时回退）
	PNGCodec png;// 软件 PNG codec
	BMPCodec bmp;// 软件 BMP codec
	ImageDecodeOptions dopt;
	ImageDecodeOptionsInit(dopt);
	dopt.preferredFormat = PixelFormat::RGB565;// 输出 RGB565（对齐 LTDC 层）

	while (true) {
		LTDC[1].DrawRectangle(GrafRect(0, 0, LCD_W, LCD_H, Color::Black));
		// 读取当前图片文件
		FAT_FileHandle fh;
		FilesysSearchArgs sargs{};
		sargs.handle_buffer = &fh;
#if IMAGE_PROFILE
		profile_reset();
		uint64 t0 = profile_now();
#endif
		void* fh2 = fs.search(pic_name[cur], &sargs);
#if IMAGE_PROFILE
		image_profile.search_ms += profile_now() - t0;
#endif
		if (!fh2 || fh.is_dir || !fh.size) {
			XART1.OutFormat("open %s FAIL\r\n", pic_name[cur]);
		} else {
			stduint fsize = (stduint)fh.size;
			XART1.OutFormat("open %s %uB\r\n", pic_name[cur], (unsigned)fsize);

			// 按块读取（不整文件进内存），解码器按需 readfl
			ImageBuffer out;
			ImageBufferClear(out);
			ImageResult r;
			stduint nlen = StrLength(pic_name[cur]);
			const char* ext = pic_name[cur] + nlen - 4;
			if (StrCompareInsensitive(ext, ".png") == 0) {
				FileBlockDevice mem(fs, fh2, fsize, PNG_FILE_BLOCK_SIZE);
#if IMAGE_PROFILE
				t0 = profile_now();
#endif
				r = show_png_stream(mem, png, mempool, dopt);
#if IMAGE_PROFILE
				image_profile.decode_ms += profile_now() - t0;
#endif
			} else if (StrCompareInsensitive(ext, ".bmp") == 0) {
				FileBlockDevice mem(fs, fh2, fsize, IMAGE_FILE_BLOCK_SIZE);
#if IMAGE_PROFILE
				t0 = profile_now();
#endif
				r = bmp.Decode(mem, out, mempool, dopt);
#if IMAGE_PROFILE
				image_profile.decode_ms += profile_now() - t0;
#endif
			} else {
				FileBlockDevice mem(fs, fh2, fsize, IMAGE_FILE_BLOCK_SIZE);
				// 硬件 JPEG，失败则回退软件 JPEG
#if IMAGE_PROFILE
				t0 = profile_now();
#endif
				r = hw.Decode(mem, out, mempool, dopt);
				if (r != ImageResult::OK || !out.pixels) {
					ImageBufferFree(out);
					XART1.OutFormat("hw FAIL %d -> soft\r\n", (int)r);
					r = soft.Decode(mem, out, mempool, dopt);
				}
#if IMAGE_PROFILE
				image_profile.decode_ms += profile_now() - t0;
#endif
			}
			if (r == ImageResult::OK && out.pixels) {
				// 保持宽高比算目标尺寸，再用库最近邻缩放，最后居中写帧缓冲
				stduint dst_w = 0, dst_h = 0;
				PictureOperation::FitAspect(out.width, out.height, LCD_W, LCD_H, dst_w, dst_h);

				ImageBuffer scaled;
				ImageBufferClear(scaled);
#if IMAGE_PROFILE
				t0 = profile_now();
#endif
				r = PictureOperation::ScaleNearest(out, scaled, dst_w, dst_h, PixelFormat::RGB565, mempool);
				if (r == ImageResult::OK && scaled.pixels) {
					uint16* px = (uint16*)scaled.pixels;
					stduint ox = (LCD_W > dst_w) ? (LCD_W - dst_w) / 2 : 0;
					stduint oy = (LCD_H > dst_h) ? (LCD_H - dst_h) / 2 : 0;
					for (stduint dy = 0; dy < dst_h; dy++) {
						uint16* dst = LCD_FB + (oy + dy) * LCD_W + ox;
						for (stduint dx = 0; dx < dst_w; dx++) {
							dst[dx] = px[dy * dst_w + dx];
						}
					}
					XART1.OutFormat("show %ux%u -> %ux%u\r\n",
						(unsigned)out.width, (unsigned)out.height, (unsigned)dst_w, (unsigned)dst_h);
					ImageBufferFree(scaled);
				}
#if IMAGE_PROFILE
				image_profile.scale_ms += profile_now() - t0;
#endif
				ImageBufferFree(out);
			} else if (r != ImageResult::OK) {
				XART1.OutFormat("decode FAIL %d\r\n", (int)r);
			}
#if IMAGE_PROFILE
			profile_print(pic_name[cur], r);
#endif
		}
		key_event = 0;// 反复消抖
		LEDB.Toggle();
		while (key_event == 0) {
			SysDelay_ms(10, true);
		}
		stduint key = key_event;
		key_event = 0;
		if (key == 1) { cur = (cur + 1 < pic_count) ? cur + 1 : 0; }
		else if (key == 2) { cur = (cur > 0) ? cur - 1 : pic_count - 1; }
		else if (key == 3) { collect_jpg(fs); XART1.OutFormat("%u JPG\r\n", (unsigned)pic_count); if (pic_count) cur = 0; }
		key_event = 0;
	}
}
void printlog(loglevel_t level, const char* fmt, ...) {}
void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
