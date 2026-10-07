// ==================== 本项目未彻底完成（82-USBM-Hub） ====================
// 目标：STM32H743 OTG2_FS 主机口 -> 一个 USB2.0 HUB（Corechips 3431:6243）-> 下游一个低速鼠标 + 一个低速键盘。
// 现状：HUB 与两个低速子设备都能枚举完成，键鼠报文能收到（[MOUSE]/[KBD] 有输出），但【会偶发整条总线掉死】，
//       靠复位或本文件里的端口自救恢复，属于"能用但不稳定"，未达到可交付状态。
//
// 已知问题 1（根因，硬件相关）：HUB 的低速(PRE)转发会把根口搞死。
//   现象：HPRT0 = PCSTS=1 / PENCHNG=1 / PENA=0 / POCA=0（不是过流、设备还在），
//         同时 HFNUM.FRNUM 停走 = 核不再发 SOF；此后核一个 token 都不发。
//   判据：FRNUM 停走比看 PENA 更硬（见 [USBH] PORT DEAD / [USBH] HPRT0 两行）。
//   恢复：只有断 VBUS 或整片复位有效；在原端口上重试永远无效（本文件已按此做了自救）。
//   为什么"接电脑就正常"：电脑根口是高速，HUB 在电脑上走 HS + 内部 TT 的 split 事务，主机从不在线上发 LS；
//   而 OTG2_FS 只有全速，必须自己生成 PRE + 1.5Mbps 低速时序，这个 HUB 在这种用法下转发不稳。
//   已试无效：换调度节拍（帧首/帧中/每帧一笔/LS 帧间隔）、TRDT 调整、第二个 HUB（Terminus，低速完全不转发）。
//
// 已知问题 2（软件已兜底）：枚举第二个低速设备时，核会"吞掉一笔传输且不产生任何中断"。
//   现象：HCTSIZ 里挂着一个没执行的 SETUP，HCCHAR 是 CHENA|CHDIS，而软件侧 state/urb 全为 0。
//   兜底：库内控制通道 10ms 看门狗（重初始化通道 + 从 SETUP 重发）+ 端点 20ms 看门狗（补一个 NAK 式完成让驱动重挂轮询）。
//
// 已知问题 3（未修）：xxxLock 键会让总线出事。
//   原因：Lock 键是运行期唯一会发低速控制传输的东西（Keyboard -> SetLed -> SET_REPORT），其余按键只走中断 IN。
//   实测：单独插键盘按 CapsLock 完全正常；键鼠同时挂载时按 Lock 会把端口打掉（疑为问题 1 的触发路径）。
//   候选修法（未做）：把 LED 报文与其它 LS 事务串行化，或去掉 LED 上报。
//
// 已知问题 4（未动）：USB 内核时钟用 HSI48，实测约 -1.19%，超出 USB ±0.25% 规范。
//   要精确 48MHz 得改 PLL3 的分频，而 PLL3 同时供 LTDC（在共享文件 _opendev/RGB-LCD.cpp 里配置），牵动面大，暂不处理。
//
// 下一步方向（未做）：换一个低速转发可靠的 HUB / 用带外部供电的 HUB；或把 USB 时钟做成精确 48MHz；或把 LED 控制传输串行化。
// 遗留诊断（留着排查用，非功能代码）：_USBH_DUMP / _USBH_ENUM_TRACE / _USBH_HEARTBEAT 三个开关，
//   以及 ev_urb_trace / ev_itr_trace / ev_rx_* / ev_ch_* 等现场数组（约 4.7KB RAM，其中 ev_urb_bad_cnt 仍在使用）。
// =======================================================================

// 82-USBM-Mouse：USB 主机（OTG2_FS）枚举 + HID 鼠标驱动 + 坐标/按键回显
// 鼠标插到 HOST 口后，XART1（115200，8N1）上预期依次出现下列行：
//   [USBH] Host power : PCF8574 P3 <- 1 : ok
//   [USBH] Host controller started (HPRT0=0x00001000)
//   [USBH] Device attached (event #1)
//   [USBH] Enumerating... (port speed code=1)
//   [USBH] VID=0x.... PID=0x.... class=0x00 sub=0x00 proto=0x00
//   [USBH] Manufacturer: ... / Product: ...
//   [USBH] endpoint configs=1
//   [USBH] Enumeration OK
//   [HID] ep[0] id=1 dir=in type=3 mps=4 interval=10 driver=bound
//   [HID] class driver bound: 1
// 阶段0 的通过标准：driver=bound 那一行 + 配置值 value=1 + 全程无 Stall/Error
// 失败表现：Device attached 之后没有下文、endpoint configs=0、driver=none、或出现 [URB] 段
// 端口速度码 1 是全速（多数鼠标）、2 是低速，两者都应能枚举
// 按键：Up = 重新复位枚举，Down = 打印主机状态快照（含鼠标报文计数），Left/Right = 保留
// [USBH] heartbeat 每 2 秒一行，用来说明主循环没被驱动轮询卡住
// LEDB 随主循环翻转；红灯长闪说明时钟/控制器初始化失败或出现 [FAULT]
// 回显：[MOUSE] x/y = 累计位移，btn 位 = 0x01 左 / 0x02 右 / 0x04 中，dx/dy = 最近一包位移
// 打印节奏：按键变化立刻一行，纯移动每 200ms 一行
// 阶段5：开机先 sdram_init() + ltdc_init() 点亮 4.3 寸 RGB 屏（800x480，RGB565，帧缓冲在 SDRAM）
//         并画 8 条竖色条 + 白边框 + 两行字；屏幕正常应看到串口打印 [LCD] LTDC OK, PixClk=...
// Down 键快照另给 clicks=L/R/M（按下沿计数）和 pos=(x,y)
// 阶段6：屏上 16x16 箭头光标（白心黑边），按屏坐标夹在 800x480 内，重枚举回屏幕中心
// 实测本机：INSTANT USB GAMING MOUSE (18F8:1286)，低速 PSPD=2、mps=8、报文字节数 3
// 本 demo 不做：滚轮（3 字节 boot 报文没有滚轮字节，要滚轮得走 report 协议并解析描述符）、屏幕坐标夹取、光标绘制

#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/IIC>
#include <cpp/Device/USB/HCD.hpp>
#include <cpp/Device/USB/OTGHostDevice.hpp>
#include <cpp/Device/USB/USBHost-HID.hpp>
#include <c/driver/mouse.h>
#include <c/driver/keyboard.h>
#include <c/data.h>
#include <c/mempool.h>
#include <c/consio.h>
#include <c/ustring.h>
#include <c/system/debug.h>
#include "../_opendev/RGB-LCD.hpp"// 共享 LTDC + SDRAM 初始化（RGB-LCD.cpp / SDRAM.cpp 要加进工程）

extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;
using namespace uni::device::SpaceUSB;

// ---- 板载 LED 与按键 ----
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// Up
GPIN& KEYL = GPIOC[13];// Left
GPIN& KEYD = GPIOH[ 2];// Down
GPIN& KEYR = GPIOH[ 3];// Right

static byte _heap_pool[256 * 1024];

extern "C" void outtxt(const char* str, stduint len) { XART1.out(str, len); }

// 框架要符号的接口
char* StrHeap(const char* valit_str) { return (char*)valit_str; }
char* StrHeapAppendChars(char* dest, char chr, size_t n) { return dest + n + chr; }
char* salc(size_t size) { return 0; }
extern "C" void memf(void* m) { if (m) free((char*)m); }

// ---- 按键中断：ISR 只计数，动作留给主循环 ----
static volatile uint32 key_ev_up = 0, key_ev_dn = 0, key_ev_lf = 0, key_ev_rt = 0;
void key_isr_up() { key_ev_up++; }
void key_isr_dn() { key_ev_dn++; }
void key_isr_lf() { key_ev_lf++; }
void key_isr_rt() { key_ev_rt++; }

// HPRT0 = 主机端口寄存器（OTG 基址 + USB_OTG_HOST_PORT_BASE）
#define _USBH_HPRT0 (*(volatile uint32*)(_OTG2_FS_ADDR + USB_OTG_HOST_PORT_BASE))
// 主机通道 n 的寄存器（HCCHAR +0x00 / HCINT +0x08）
#define _USBH_HCREG(n, off) (*(volatile uint32*)(_OTG2_FS_ADDR + USB_OTG_HOST_CHANNEL_BASE + (n) * USB_OTG_HOST_CHANNEL_SIZE + (off)))

// ==================== 主机口 5V 供电 ====================
// 1 = PCF8574 位写（P3），本板 HOST 口供电就是这么接的
// 2 = GPIO 直驱（PG13）
// 3 = 常供（固件不动任何东西）
#define USBH_PWR_MODE 1

// 1 = 枚举期间打印控制器寄存器现场
#define _USBH_ENUM_TRACE 0
// 1 = 每 2 秒打印一行心跳（默认关闭，只有排查主循环是否被卡住时才打开）
#define _USBH_HEARTBEAT 0
// 1 = 打印 URB / HID 寄存器级调试转储（每次快照 70+ 行，只在 bring-up 排查时开）
#define _USBH_DUMP 0
// 1 = 打印 [MOUSE] 坐标/按键行，0 = 不打印
#define _MOUSE_LOG 1
// 1 = 打印 [KBD] 按键行，0 = 不打印
#define _KBD_LOG 1
// 4.3 寸 RGB 屏：800x480，RGB565，帧缓冲在外部 SDRAM
#define LCD_W 800
#define LCD_H 480

#if USBH_PWR_MODE == 1
#define USBH_PCF_ADDR7   0x20// 7 位器件地址
#define USBH_PCF_RDADDR  0x41// 器件地址左移后补读位
#define USBH_USB_PWR_BIT 3   // P3 = USB 电源

static void iic_delay() { for (volatile int i = 0; i < 500; i++) {} }

// PCF8574 与 ES8388/24C02 挂同一条软 I2C（SDA=PH5，SCL=PH4）
class PcfIIC : public uni::IIC_SOFT {
public:
	// IIC_SOFT 声明了 Send(byte*, stduint, bool)，会把基类的 Send(byte, bool) 整名遮住
	using uni::IIC_t::Send;

	PcfIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

static byte _pcf_wire_buf[byteof(PcfIIC)];
static PcfIIC* pcf_iic = nullptr;

// 读 8 位 IO；SendStart 只发写地址，所以读地址要手发
static bool PcfReadByte(byte& out) {
	pcf_iic->SendStart(0);
	pcf_iic->Send(USBH_PCF_RDADDR, true);
	if (pcf_iic->getError() != ERR_IIC_NONE) return false;
	out = pcf_iic->ReadByte(true, false);// 只读 1 字节 -> 末尾回 NACK
	pcf_iic->SendStop();
	return true;
}

static bool PcfWriteByte(byte val) {
	pcf_iic->SendStart(USBH_PCF_ADDR7);
	pcf_iic->Send(val, true);
	pcf_iic->SendStop();
	SysDelay_ms(10);
	return (pcf_iic->getError() == ERR_IIC_NONE);
}

// 先读回再只改一位，免得踩到蜂鸣器/DCMI/RS485/以太网复位这几位
static bool PcfWriteBit(byte bit_index, bool val) {
	byte cur = 0xFF;// 读失败时按上电默认（全高）继续
	bool read_ok = PcfReadByte(cur);
	if (val) cur |= (byte)(1u << bit_index);
	else cur &= (byte)~(1u << bit_index);
	bool write_ok = PcfWriteByte(cur);
	return read_ok && write_ok;
}
#endif

static void UsbHostDriverVBUS(bool state) {
#if USBH_PWR_MODE == 1
	bool ok = PcfWriteBit(USBH_USB_PWR_BIT, state);
	XART1.OutFormat("[USBH] Host power : PCF8574 P%u <- %u : %s\r\n",
		(unsigned)USBH_USB_PWR_BIT, (unsigned)(state ? 1 : 0), ok ? "ok" : "FAILED");
#elif USBH_PWR_MODE == 2
	GPIOG[13].setMode(GPIOMode::OUT) = state;
	XART1.OutFormat("[USBH] Host power : PG13 <- %u\r\n", (unsigned)(state ? 1 : 0));
#else
	(void)state;
	XART1.OutFormat("[USBH] Host power : always on (firmware does nothing)\r\n");
#endif
}

// ==================== 主机事件与传输桥 ====================
static OTGHostDevice* usb_otg_host = nullptr;// 插上设备时才构造
alignas(8) static byte _host_bridge_buf[byteof(OTGHostDevice)];

static volatile uint32 ev_conn_cnt = 0;
static volatile uint32 ev_disc_cnt = 0;
static volatile uint32 ev_urb_cnt = 0;
static volatile uint32 ev_irq_cnt = 0;
static volatile uint32 ev_gintsts_seen = 0;

// HPRT0.PCSTS：主机口上有设备
static bool UsbHostPortConnected() {
	return (_USBH_HPRT0 & (1u << USB_OTG_HPRT_PCSTS_Pos)) != 0;
}

void on_hcd_connect() { ev_conn_cnt++; }
void on_hcd_disconnect() { ev_disc_cnt++; }

// ==================== 阶段0/3：鼠标驱动绑定 + 坐标按键记录 ====================
// 库只有在真的建出 HIDMouseDriver 时才会订阅这个观察者，所以计数 >0 就等于绑定成功
static volatile uint32 mouse_report_cnt = 0;
static volatile byte mouse_last[3] = {};// 按键位, X, Y
// 阶段3：相对位移累加成绝对位置（本 demo 没有屏幕，所以不做夹取）
static volatile int32 mouse_x = 0, mouse_y = 0;
static volatile byte mouse_btn = 0;
// 阶段4：按键按下次数（左/右/中），Down 键快照打印
static volatile uint32 mouse_click_cnt[3] = {};

// ==================== 阶段6：屏幕光标（LTDC 第 2 层） ====================
#define CUR_W 16
#define CUR_H 16
#define CUR_WHITE 0xFFFF// ARGB1555 A=1 白
#define CUR_BLACK 0x8000// ARGB1555 A=1 黑
#define CUR_CLEAR 0x0000// ARGB1555 A=0 透明
// 箭头形状：位15 = 最左像素，1 = 光标像素
static const uint16 cur_shape[CUR_H] = {
	0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
	0xFF80, 0xFC00, 0xCC00, 0x0C00, 0x0600, 0x0600, 0x0300, 0x0300
};
// 贴图放在帧缓冲之后的空位（背景 800x480x2 = 768000 字节）
static uint16* const cur_sprite = (uint16*)(SDRAM_BANK1_BASE + LCD_W * LCD_H * 2);
static int32 cur_x = (LCD_W - CUR_W) / 2, cur_y = (LCD_H - CUR_H) / 2;
// 位移队列：中断里入队、主循环出队，按屏坐标夹取，绕不过边界
static volatile int8 cur_dq_x[32] = {}, cur_dq_y[32] = {};
static volatile uint32 cur_dq_cnt = 0;

// 预生成 16x16 贴图：白心 + 黑边，其余 A=0 全透明
static void cursor_build() {
	for (int y = 0; y < CUR_H; y++)
		for (int x = 0; x < CUR_W; x++)
			cur_sprite[y * CUR_W + x] = (cur_shape[y] & (0x8000u >> x)) ? CUR_WHITE : CUR_CLEAR;
	for (int y = 0; y < CUR_H; y++) {
		for (int x = 0; x < CUR_W; x++) {
			if (cur_sprite[y * CUR_W + x] != CUR_WHITE) continue;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int nx = x + dx, ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= CUR_W || ny >= CUR_H) continue;
					if (cur_sprite[ny * CUR_W + nx] == CUR_CLEAR) cur_sprite[ny * CUR_W + nx] = CUR_BLACK;
				}
			}
		}
	}
}

// 移动 = 只改这一层的窗口位置，不写显存
static void cursor_place(int32 x, int32 y) {
	cur_x = x;
	cur_y = y;
	const stduint hbp = LTDC[LTDCReg::BPCR].masof(16, 12);// AHBP
	const stduint vbp = LTDC[LTDCReg::BPCR].masof(0, 12);// AVBP
	LTDC[2][LTDCLayerReg::WHPCR] = (((stduint)(x + CUR_W) + hbp) << 16) | ((stduint)x + hbp + 1);
	LTDC[2][LTDCLayerReg::WVPCR] = (((stduint)(y + CUR_H) + vbp) << 16) | ((stduint)y + vbp + 1);
	LTDC.Reload(LTDCReload::Immediate);
}

// 把第 2 层配成 16x16 光标层：ARGB1555 逐像素 alpha，不需要色键
static void cursor_layer_init() {
	LTDC_LAYER_t::LayerPara lp{};
	LTDC_LAYER_t::layer_param_refer(&lp);
	lp.roleaddr = (pureptr_t)cur_sprite;
	lp.window = Rectangle(Point(cur_x, cur_y), Size2(CUR_W, CUR_H));
	lp.image_size = Size2(CUR_W, CUR_H);
	lp.pixel_format = PixelFormat::ARGB1555;
	lp.backcolor = Color::Black;// CACR.CONSTA = 255，只管乘到像素 alpha 上
	lp.factor1_mode = true;// BF1 = 像素alpha x 常量alpha
	lp.factor2_mode = true;// BF2 = 1 - 像素alpha x 常量alpha
	LTDC[2].setMode(lp);
	LTDC.Reload(LTDCReload::Immediate);
}

// 夹取到屏内，再移动叠层
static void cursor_move(int32 dx, int32 dy) {
	int32 nx = cur_x + dx, ny = cur_y + dy;
	if (nx < 0) nx = 0;
	if (ny < 0) ny = 0;
	if (nx > LCD_W - CUR_W) nx = LCD_W - CUR_W;
	if (ny > LCD_H - CUR_H) ny = LCD_H - CUR_H;
	if (nx == cur_x && ny == cur_y) return;
	cursor_place(nx, ny);
}

// 回到屏幕中心
static void cursor_reset() {
	cursor_place((LCD_W - CUR_W) / 2, (LCD_H - CUR_H) / 2);
}

// 中断里只记录，打印放在主循环
static void on_mouse_report(MouseMessage m) {
	mouse_report_cnt++;
	mouse_last[0] = (byte)((m.BtnLeft ? 1 : 0) | (m.BtnRight ? 2 : 0) | (m.BtnMiddle ? 4 : 0));
	mouse_last[1] = (byte)m.X;
	mouse_last[2] = (byte)m.Y;
	mouse_x += m.X;
	mouse_y += m.Y;
	// 位移入队，主循环出队后按屏坐标夹取
	const uint32 qi = cur_dq_cnt++ & 31;
	cur_dq_x[qi] = m.X;
	cur_dq_y[qi] = m.Y;
	// 只在按下沿计数，按住不放不会一直加
	for (byte i = 0; i < 3; i++) {
		const byte bit = (byte)(1u << i);
		if ((mouse_last[0] & bit) && !(mouse_btn & bit)) mouse_click_cnt[i]++;
	}
	mouse_btn = mouse_last[0];
}

// ==================== 键盘：报文在中断里记录，打印在主循环 ====================
#define KBEV_MAX 16
static volatile uint32 kbd_key_cnt = 0;
static volatile byte kbev_code[KBEV_MAX] = {};
static volatile byte kbev_method[KBEV_MAX] = {};// 0=keydown 1=keyup 2=keyrepeat
static volatile byte kbev_mod[KBEV_MAX] = {};
static volatile uint32 kbev_cnt = 0;

static void on_key_push(keyboard_event_t ev) {
	kbd_key_cnt++;
	const uint32 i = kbev_cnt++ & (KBEV_MAX - 1);
	kbev_code[i] = ev.keycode;
	kbev_method[i] = (byte)ev.method;
	kbev_mod[i] = ev.mod_val;
}

static void PrintKeyEvents(uint32& seen) {
#if _KBD_LOG
	while (seen != kbev_cnt) {
		const uint32 i = seen++ & (KBEV_MAX - 1);
		XART1.OutFormat("[KBD] %s code=0x%02X mod=0x%02X\r\n",
			kbev_method[i] == 1 ? "up  " : (kbev_method[i] == 2 ? "rpt " : "down"),
			(unsigned)kbev_code[i], (unsigned)kbev_mod[i]);
	}
#else
	seen = kbev_cnt;
#endif
}

// 控制传输 trace：中断里记录，主循环里打印
static volatile uint32 ev_urb_trace[64][9] = {};
static volatile byte ev_urb_head[64][8] = {};
static volatile byte ev_urb_setup[64][8] = {};
static volatile uint32 ev_urb_idx = 0;
static volatile uint32 ev_urb_fail_cnt = 0;
static volatile uint32 ev_urb_bad_cnt = 0;// 只有 Stall/Error 才计（真失败）
// 主循环迭代计数：用来测主循环实际频率（每秒多少次），判断 USB ISR 是否把主循环饿住
static volatile uint32 ev_loop_cnt = 0;
// 按通道分类计数：done=收到数据，nak=设备空闲应答(正常)，bad=Stall/Error(真失败)
static volatile uint32 ev_ch_done[16] = {};
static volatile uint32 ev_ch_nak[16] = {};
static volatile uint32 ev_ch_bad[16] = {};
// 再按通道的硬件状态分类：3=NAK(设备回应了) 6=Xacterr(没人应答) 1=XFRC 5=Stall 8=DataTglErr
static volatile uint32 ev_ch_state[16][10] = {};
// 每个通道最后一次控制 SETUP 的 8 个字节（bmRequestType,bRequest,wValueL/H,wIndexL/H,wLengthL/H）
static volatile byte ev_ch_setup[16][8] = {};
static volatile bool ev_ch_setup_valid[16] = {};
// 控制通道最后一次 DATA-IN 回来的前 18 字节：枚举失败时也能认出设备（VID/PID 在偏移 8/10）
static volatile byte ev_ch_reply[16][18] = {};
static volatile byte ev_ch_reply_len[16] = {};
// RX 探测：出口时 RXFLVL 仍悬挂的次数、见到的 IN 数据包数/其中零长度包数（零长度 = ZLP）
static volatile uint32 ev_rx_left_cnt = 0;
static volatile uint32 ev_rx_data_cnt = 0;
static volatile uint32 ev_rx_zero_cnt = 0;
static volatile uint32 ev_rx_bcnt_max = 0;
static volatile uint32 ev_rx_last_pktsts = 0xFFFFFFFF;
static bool urb_trace_reported = false;

// 阶段1：非 0 通道（中断端点）的完成记录，只留最近 8 条（循环覆盖）
static volatile uint32 ev_itr_trace[8][5] = {};// ch, st, chs, count, dir
static volatile uint32 ev_itr_cnt = 0;// 累计条数，取模 8 得槽位
static volatile uint32 ev_itr_fail_cnt = 0;
// 最近一次中断 IN 的现场：只有 st=Done 才说明真的收到过鼠标报文
static volatile uint32 ev_itr_last[4] = {};// st, chs, count, ch
static volatile byte ev_itr_last_data[8] = {};// 报文头 8 字节
static volatile uint32 ev_itr_done_cnt = 0;// st=Done 的累计次数
static bool hid_stage1_reported = false;
static uint32 hid_stage1_ms = 0;

void on_hcd_urb(void* ch_num, ...) {
	byte ch = (byte)(stduint)ch_num;
	URBState st = HCD2.getURBState(ch);
	if (usb_otg_host) {
		// 记录要赶在交给桥之前，桥随后就会改通道
		if (ch == 0 && ev_urb_idx < 64) {
			uint32 i = ev_urb_idx;
			ev_urb_trace[i][0] = (uint32)st;
			ev_urb_trace[i][1] = (uint32)HCD2.hc[0].state;
			ev_urb_trace[i][2] = (uint32)HCD2.getXferCount(0);
			ev_urb_trace[i][3] = (uint32)HCD2.hc[0].ep_is_in;
			ev_urb_trace[i][4] = (uint32)HCD2.hc[0].data_pid;
			ev_urb_trace[i][5] = (uint32)HCD2.hc[0].xfer_len;
			ev_urb_trace[i][6] = (uint32)HCD2.hc[0].ErrCnt;
			ev_urb_trace[i][7] = (uint32)HCD2.hc[0].toggle_out;
			ev_urb_trace[i][8] = (uint32)_USBH_HCREG(0, 0x10);// HCTSIZ：XFRSIZ 与 PKTCNT
			byte* b = usb_otg_host->Buffer();
			for (int k = 0; k < 8; k++) ev_urb_head[i][k] = b ? b[k] : (byte)0xFF;
			// SETUP 阶段 xfer_buff 还指着那 8 个请求字节
			if (!HCD2.hc[0].ep_is_in && HCD2.hc[0].xfer_len == 8) {
				const byte* s = (const byte*)HCD2.hc[0].xfer_buff;
				for (int k = 0; k < 8; k++) ev_urb_setup[i][k] = s ? s[k] : (byte)0xFF;
			}
			ev_urb_idx++;
		}
		else if (ch != 0) {
			// 循环覆盖：只保留最近 8 条
			uint32 i = ev_itr_cnt & 7;
			ev_itr_trace[i][0] = (uint32)ch;
			ev_itr_trace[i][1] = (uint32)st;
			ev_itr_trace[i][2] = (uint32)HCD2.hc[ch].state;
			ev_itr_trace[i][3] = (uint32)HCD2.getXferCount(ch);
			ev_itr_trace[i][4] = (uint32)HCD2.hc[ch].ep_is_in;
			ev_itr_cnt++;
			// 现场只记真正的收包，否则会被紧随其后的空闲 NAK 覆盖掉
			if (st == URBState::Done) {
				ev_itr_done_cnt++;
				// 报文头：收包时 xfer_buff 被前移了，所以从 xfer_buff-count 往回取
				uint32 cnt = (uint32)HCD2.getXferCount(ch);
				byte* end = HCD2.hc[ch].xfer_buff;
				byte* start = (end && cnt) ? (end - cnt) : nullptr;
				ev_itr_last[0] = (uint32)st;
				ev_itr_last[1] = (uint32)HCD2.hc[ch].state;
				ev_itr_last[2] = cnt;
				ev_itr_last[3] = (uint32)ch;
				for (uint32 k = 0; k < 8; k++) {
					ev_itr_last_data[k] = start ? start[k] : (byte)0xFF;
				}
			}
		}
	}
	if (st != URBState::Done) ev_urb_fail_cnt++;
	// NAK（NotReady）和零结果 halt 都是常态，只有 Stall/Error 才算真失败
	if (st == URBState::Stall || st == URBState::Error) ev_urb_bad_cnt++;
	if (ch != 0 && st != URBState::Done) ev_itr_fail_cnt++;
	ev_urb_cnt++;
	// 按通道分类计数：区分"设备正常空闲 NAK"和"事务真失败"
	if (ch) {
		const byte hst = (byte)(HCD2.hc[ch].state & 15);
		if (hst < 10) ev_ch_state[ch & 15][hst]++;
		// 控制 SETUP 阶段：把 8 个请求字节存下来，用来定位枚举卡在哪个请求
		if (HCD2.hc[ch].ep_type == 0 && !HCD2.hc[ch].ep_is_in && HCD2.hc[ch].xfer_len == 8) {
			const byte* s = (const byte*)HCD2.hc[ch].xfer_buff;
			for (int k = 0; k < 8; k++) ev_ch_setup[ch & 15][k] = s ? s[k] : (byte)0xFF;
			ev_ch_setup_valid[ch & 15] = true;
		}
		// 控制 IN 的回复：只抓第一次（那就是 18 字节设备描述符），
		// 后面的字符串/配置描述符会把 VID/PID 覆盖成假值
		if (st == URBState::Done && HCD2.hc[ch].ep_type == 0 && HCD2.hc[ch].ep_is_in
			&& ev_ch_reply_len[ch & 15] < 18) {
			const uint32 cnt = (uint32)HCD2.getXferCount(ch);
			byte* end = HCD2.hc[ch].xfer_buff;
			byte* begin = (end && cnt) ? (end - cnt) : nullptr;
			if (begin && cnt >= 18) {
				for (int k = 0; k < 18; k++) ev_ch_reply[ch & 15][k] = begin[k];
				ev_ch_reply_len[ch & 15] = 18;
			}
		}
		if (st == URBState::Done) ev_ch_done[ch & 15]++;
		else if (st == URBState::NotReady || st == URBState::NYET || st == URBState::Idle) ev_ch_nak[ch & 15]++;
		else ev_ch_bad[ch & 15]++;
	}
	// 一个通道只属于一个设备：Hub 下游设备有自己的桥，必须按 OwnerOfChannel 分发
	OTGHostDevice* owner = OTGHostDevice::OwnerOfChannel(ch);
	if (owner) owner->OnChannelURBCompleted(ch, st);
}

#if _USBH_DUMP
// 打印记录下来的控制传输：st=URB状态 chs=通道状态 count=字节数 in=方向 pid=DPID len=请求长度 err=ErrCnt to=toggle_out
static void PrintUrbTrace() {
	XART1.OutFormat("[URB] %u URBs, %u not done\r\n",
		(unsigned)ev_urb_idx, (unsigned)ev_urb_fail_cnt);
	XART1.OutFormat("[URB] rx: left=%u data=%u zero=%u bcnt_max=%u last_pktsts=%u (2=IN data, 3=IN done, 7=halted)\r\n",
		(unsigned)ev_rx_left_cnt, (unsigned)ev_rx_data_cnt, (unsigned)ev_rx_zero_cnt,
		(unsigned)ev_rx_bcnt_max, (unsigned)ev_rx_last_pktsts);
	for (uint32 i = 0; i < ev_urb_idx && i < 64; i++) {
		// 一条记录一行：st=URB状态 chs=通道状态 count=字节数 in=方向 pid=DPID len=请求长度 err=ErrCnt to=toggle_out
		// setup = 8 字节请求包（bmRequestType, bRequest, wValue 低/高, wIndex 低/高, wLength 低/高），仅 SETUP 阶段有效
		XART1.OutFormat("[URB] %u st=%u chs=%u count=%u in=%u pid=%u len=%u err=%u to=%u tsz=%08X setup=%02X%02X%02X%02X%02X%02X%02X%02X head=%02X%02X%02X%02X\r\n",
			(unsigned)i,
			(unsigned)ev_urb_trace[i][0], (unsigned)ev_urb_trace[i][1],
			(unsigned)ev_urb_trace[i][2], (unsigned)ev_urb_trace[i][3],
			(unsigned)ev_urb_trace[i][4], (unsigned)ev_urb_trace[i][5],
			(unsigned)ev_urb_trace[i][6], (unsigned)ev_urb_trace[i][7],
			(unsigned)ev_urb_trace[i][8],
			(unsigned)ev_urb_setup[i][0], (unsigned)ev_urb_setup[i][1],
			(unsigned)ev_urb_setup[i][2], (unsigned)ev_urb_setup[i][3],
			(unsigned)ev_urb_setup[i][4], (unsigned)ev_urb_setup[i][5],
			(unsigned)ev_urb_setup[i][6], (unsigned)ev_urb_setup[i][7],
			(unsigned)ev_urb_head[i][0], (unsigned)ev_urb_head[i][1],
			(unsigned)ev_urb_head[i][2], (unsigned)ev_urb_head[i][3]);
	}
}

// 阶段1：HID 握手（SET_PROTOCOL）与中断端点首次提交/完成
// 诊断用：把一笔标准请求的结果收回来（枚举之后的完成要靠 issuer 才会派发）
class Stage1Waiter : public ClassDriver {
public:
	Stage1Waiter(USBHostDevice* dev) : ClassDriver{ dev } {}
	Error Initialize() override { return MAKE_ERROR(Error::kSuccess); }
	Error SetEndpoint(const EndpointConfig& config) override {
		(void)config;
		return MAKE_ERROR(Error::kSuccess);
	}
	Error OnEndpointsConfigured() override { return MAKE_ERROR(Error::kSuccess); }
	Error OnControlCompleted(EndpointID ep_id, SetupData setup_data, const void* buf, int len) override {
		(void)ep_id; (void)setup_data;
		last_len = len;
		last_value = (len > 0 && buf) ? *static_cast<const byte*>(buf) : 0xFF;
		done = true;
		return MAKE_ERROR(Error::kSuccess);
	}
	Error OnInterruptCompleted(EndpointID ep_id, const void* buf, int len) override {
		(void)ep_id; (void)buf; (void)len;
		return MAKE_ERROR(Error::kSuccess);
	}
	static bool done;
	static int last_len;
	static byte last_value;
};
bool Stage1Waiter::done = false;
int Stage1Waiter::last_len = 0;
byte Stage1Waiter::last_value = 0;

static byte _waiter_buf[byteof(Stage1Waiter)];
static Stage1Waiter* stage1_waiter = nullptr;
static byte stage1_cfg = 0;

// 问设备当前的配置值：0 表示它还没被 SET_CONFIGURATION 配上，那 EP1 就不会应答
static void Stage1AskConfiguration(OTGHostDevice& dev) {
	stage1_waiter = new (_waiter_buf) Stage1Waiter(&dev);
	SetupData q{};
	q.request_type.bits.direction = request_type::kIn;
	q.request_type.bits.type = request_type::kStandard;
	q.request_type.bits.recipient = request_type::kDevice;
	q.request = static_cast<uint8>(StandardRequest::GetConfiguration);
	q.value = 0;
	q.index = 0;
	q.length = 1;
	Error e = dev.ControlIn(kDefaultControlPipeID, q, &stage1_cfg, 1, stage1_waiter);
	XART1.OutFormat("[HID] GET_CONFIGURATION sent: %s\r\n", e ? e.Name() : "ok");
}

// 最近一次中断 IN 的现场：st=Done 且 count=3 才说明鼠标真回了一包报文
static void PrintItrLast() {
	XART1.OutFormat("[HID] itr last: ch=%u st=%u chs=%u count=%u data=%02X %02X %02X %02X %02X %02X %02X %02X done=%u\r\n",
		(unsigned)ev_itr_last[3], (unsigned)ev_itr_last[0], (unsigned)ev_itr_last[1],
		(unsigned)ev_itr_last[2],
		(unsigned)ev_itr_last_data[0], (unsigned)ev_itr_last_data[1],
		(unsigned)ev_itr_last_data[2], (unsigned)ev_itr_last_data[3],
		(unsigned)ev_itr_last_data[4], (unsigned)ev_itr_last_data[5],
		(unsigned)ev_itr_last_data[6], (unsigned)ev_itr_last_data[7],
		(unsigned)ev_itr_done_cnt);
}

static void PrintHidStage1() {
	const int eps = usb_otg_host->NumEndpointConfigs();
	for (int i = 0; i < eps; i++) {
		const EndpointConfig& ep = usb_otg_host->EndpointConfigs()[i];
		if (ep.ep_type != EndpointType::kInterrupt) continue;
		const byte ch = usb_otg_host->ChannelOfEndpoint(ep.ep_id);
		XART1.OutFormat("[HID] interrupt ep id=%d dir=%s mps=%u interval=%u channel=%u\r\n",
			(int)ep.ep_id.Number(), ep.ep_id.IsIn() ? "in" : "out",
			(unsigned)ep.max_packet_size, (unsigned)ep.interval, (unsigned)ch);
		// 通道编程现场：HCCHAR 里的 LSDEV/EPTYP/EPDIR/MPSIZ/DAD 决定令牌怎么发
		XART1.OutFormat("[HID] ch%u HCCHAR=0x%08X HCINT=0x%08X HCINTMSK=0x%08X HCTSIZ=0x%08X\r\n",
			(unsigned)ch, (unsigned)_USBH_HCREG(ch, 0x00), (unsigned)_USBH_HCREG(ch, 0x08),
			(unsigned)_USBH_HCREG(ch, 0x0C), (unsigned)_USBH_HCREG(ch, 0x10));
		XART1.OutFormat("[HID] ch%u speed=%u dad=%u epnum=%u epin=%u eptyp=%u mps=%u pid=%u tglin=%u err=%u chstate=%u urb=%u\r\n",
			(unsigned)ch, (unsigned)HCD2.hc[ch].speed, (unsigned)HCD2.hc[ch].dev_addr,
			(unsigned)HCD2.hc[ch].ep_num, (unsigned)HCD2.hc[ch].ep_is_in,
			(unsigned)HCD2.hc[ch].ep_type, (unsigned)HCD2.hc[ch].max_packet,
			(unsigned)HCD2.hc[ch].data_pid, (unsigned)HCD2.hc[ch].toggle_in,
			(unsigned)HCD2.hc[ch].ErrCnt, (unsigned)HCD2.hc[ch].state,
			(unsigned)HCD2.hc[ch].urb_state);
		XART1.OutFormat("[HID] ch0 HCCHAR=0x%08X (compare) HPRT0=0x%08X HFIR=%u HCFG=0x%08X\r\n",
			(unsigned)_USBH_HCREG(0, 0x00), (unsigned)_USBH_HPRT0,
			(unsigned)HCD2.HostReg(OTGHostReg::HFIR), (unsigned)HCD2.HostReg(OTGHostReg::HCFG));
	}
	XART1.OutFormat("[HID] interrupt URBs: %u (%u not done, %u done)\r\n",
		(unsigned)ev_itr_cnt, (unsigned)ev_itr_fail_cnt, (unsigned)ev_itr_done_cnt);
	PrintItrLast();
	XART1.OutFormat("[HID] config: done=%u len=%d value=%u (value 0 = not configured, 255 = no reply, done 0 = still pending)\r\n",
		(unsigned)Stage1Waiter::done, Stage1Waiter::last_len, (unsigned)Stage1Waiter::last_value);
	uint32 shown = ev_itr_cnt < 8 ? (uint32)ev_itr_cnt : 8;
	for (uint32 n = 0; n < shown; n++) {
		uint32 i = (ev_itr_cnt - shown + n) & 7;// 从最旧到最新
		XART1.OutFormat("[HID] itr[%u] ch=%u st=%u chs=%u count=%u dir=%u\r\n",
			(unsigned)n, (unsigned)ev_itr_trace[i][0], (unsigned)ev_itr_trace[i][1],
			(unsigned)ev_itr_trace[i][2], (unsigned)ev_itr_trace[i][3],
			(unsigned)ev_itr_trace[i][4]);
	}
}
#endif // _USBH_DUMP

void on_usb_fs_interrupt() {
	ev_irq_cnt++;
	uint32 gs = (uint32)HCD2.GlobalReg(OTGGlobalReg::GINTSTS);
	ev_gintsts_seen |= gs;
	// RX 探测：ISR 出口若 RXFLVL 还挂着，用只读的 GRXSTSR 偷看 FIFO 顶部（不弹）
	if (gs & _IMM1S(USB_OTG_GINTSTS_RXFLVL_Pos)) {
		ev_rx_left_cnt++;
		uint32 v = (uint32)HCD2.GlobalReg(OTGGlobalReg::GRXSTSR);
		uint32 pktsts = (v & USB_OTG_GRXSTSP_PKTSTS) >> USB_OTG_GRXSTSP_PKTSTS_Pos;
		uint32 bcnt = (v & USB_OTG_GRXSTSP_BCNT) >> USB_OTG_GRXSTSP_BCNT_Pos;
		ev_rx_last_pktsts = pktsts;
		if (pktsts == 2) {// IN data packet
			ev_rx_data_cnt++;
			if (bcnt == 0) ev_rx_zero_cnt++;
			if (bcnt > ev_rx_bcnt_max) ev_rx_bcnt_max = bcnt;
		}
	}
}

// 丢弃传输桥：先摘指针再析构，避免 URB 中断回调摸到半析构对象
static void UsbHostDropBridge() {
	OTGHostDevice* old = usb_otg_host;
	usb_otg_host = nullptr;
	if (old) old->~OTGHostDevice();
}

// 端口复位 + 建传输桥 + 开始枚举；返回 false 表示端口上没有设备
static bool UsbHostStartEnumeration(uint32& enum_deadline) {
	if (!UsbHostPortConnected()) return false;
	HCD2.ResetPort();
	SysDelay_ms(20);// 复位放开后留一点稳定时间
	byte spd = (byte)HCD2.getCurrentSpeed();// HPRT0.PSPD: 0=HS, 1=FS, 2=LS
	UsbHostDropBridge();
	ev_urb_idx = 0;
	ev_urb_fail_cnt = 0;
	ev_urb_bad_cnt = 0;
	ev_itr_cnt = 0;
	ev_itr_fail_cnt = 0;
	ev_itr_done_cnt = 0;
	// 库里灯态随新驱动复位并发 SET_REPORT(0)，demo 的大小写状态要跟着复位
	// 新驱动要重新订阅并重新收报文，计数与坐标跟着复位
	mouse_report_cnt = 0;
	mouse_last[0] = 0;
	mouse_last[1] = 0;
	mouse_last[2] = 0;
	mouse_x = 0;
	mouse_y = 0;
	mouse_btn = 0;
	mouse_click_cnt[0] = 0;
	mouse_click_cnt[1] = 0;
	mouse_click_cnt[2] = 0;
	cursor_reset();
	ev_itr_last[0] = 0;
	ev_itr_last[1] = 0;
	ev_itr_last[2] = 0;
	hid_stage1_reported = false;
	urb_trace_reported = false;
	usb_otg_host = new (_host_bridge_buf) OTGHostDevice(HCD2, 0, spd);
	enum_deadline = (uint32)SysTick::getTick() + 3000;
	Error err = usb_otg_host->StartInitialize();
#if _USBH_ENUM_TRACE
	XART1.OutFormat("[USBH] GetDesc(Device) -> %s @line %d | CH0 HCCHAR=0x%08X HCINT=0x%08X\r\n",
		err.Name(), err.Line(),
		(unsigned)_USBH_HCREG(0, 0x00), (unsigned)_USBH_HCREG(0, 0x08));
#else
	(void)err;
#endif
	XART1.OutFormat("[USBH] Enumerating... (port speed code=%u)\r\n", (unsigned)spd);
	return true;
}

void erro(const char* str) {
	if (str) XART1.OutFormat("[Error] %s\r\n", str);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i{0}; i < 1000000; i++) {}
	}
}

// 硬件异常时打印现场，便于定位
extern "C" void HardFault_Handler(void) {
	volatile uint32* sp;
	__asm volatile ("tst lr, #4\n\tite eq\n\tmrseq %0, msp\n\tmrsne %0, psp" : "=r"(sp));
	XART1.OutFormat("\r\n[FAULT] PC=0x%08X LR=0x%08X R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X R12=0x%08X xPSR=0x%08X\r\n",
		(unsigned)sp[6], (unsigned)sp[5], (unsigned)sp[0], (unsigned)sp[1],
		(unsigned)sp[2], (unsigned)sp[3], (unsigned)sp[4], (unsigned)sp[7]);
	XART1.OutFormat("[FAULT] CFSR=0x%08X HFSR=0x%08X MMFAR=0x%08X BFAR=0x%08X\r\n",
		(unsigned)*(volatile uint32*)0xE000ED28, (unsigned)*(volatile uint32*)0xE000ED2C,
		(unsigned)*(volatile uint32*)0xE000ED34, (unsigned)*(volatile uint32*)0xE000ED38);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i{0}; i < 1000000; i++) {}
	}
}

// 设备树：根设备 + 每个 Hub 下游设备
static void PrintDeviceTree() {
	XART1.OutFormat("[USBH] bus has %d device(s)\r\n", OTGHostDevice::NumBridges());
	for (int i = 0; i < OTGHostDevice::kMaxBridges; i++) {
		OTGHostDevice* dev = OTGHostDevice::BridgeAt(i);
		if (!dev) continue;
		XART1.OutFormat("[USBH]   addr=%u parent=%s port=%u enumerated=%u initialized=%u children=%d\r\n",
			(unsigned)dev->AssignedAddress(),
			dev->IsRootDevice() ? "root" : "hub",
			(unsigned)dev->ParentPort(),
			(unsigned)(dev->IsEnumerated() ? 1 : 0),
			(unsigned)(dev->IsInitialized() ? 1 : 0),
			dev->NumChildren());
		if (!dev->IsHubBridge()) {
			// 控制通道统计：判断 SET_PROTOCOL / SET_CONFIGURATION 有没有真的成功
			const byte cch = dev->ControlChannel();
			if (cch) {
				XART1.OutFormat("[USBH]     ctrl ch=%u done=%u nak=%u bad=%u st: st1=%u st3=%u st6=%u st7=%u\r\n",
					(unsigned)cch, (unsigned)ev_ch_done[cch & 15], (unsigned)ev_ch_nak[cch & 15],
					(unsigned)ev_ch_bad[cch & 15], (unsigned)ev_ch_state[cch & 15][1],
					(unsigned)ev_ch_state[cch & 15][3], (unsigned)ev_ch_state[cch & 15][6],
					(unsigned)ev_ch_state[cch & 15][7]);
				if (ev_ch_setup_valid[cch & 15]) {
					XART1.OutFormat("[USBH]     last SETUP: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
						(unsigned)ev_ch_setup[cch & 15][0], (unsigned)ev_ch_setup[cch & 15][1],
						(unsigned)ev_ch_setup[cch & 15][2], (unsigned)ev_ch_setup[cch & 15][3],
						(unsigned)ev_ch_setup[cch & 15][4], (unsigned)ev_ch_setup[cch & 15][5],
						(unsigned)ev_ch_setup[cch & 15][6], (unsigned)ev_ch_setup[cch & 15][7]);
				}
				if (ev_ch_reply_len[cch & 15] >= 18) {
					// 设备描述符：偏移 8/10 是 VID/PID，偏移 7 是 bMaxPacketSize0
					XART1.OutFormat("[USBH]     devdesc: VID=0x%04X PID=0x%04X mps0=%u\r\n",
						(unsigned)(ev_ch_reply[cch & 15][8] | (ev_ch_reply[cch & 15][9] << 8)),
						(unsigned)(ev_ch_reply[cch & 15][10] | (ev_ch_reply[cch & 15][11] << 8)),
						(unsigned)ev_ch_reply[cch & 15][7]);
				}
			}
			// 每个端点的通道与收发计数：判断中断端点到底有没有在收报文
			const int eps = dev->NumEndpointConfigs();
			for (int e = 0; e < eps; e++) {
				const EndpointConfig& ep = dev->EndpointConfigs()[e];
				const byte ch = dev->ChannelOfEndpoint(ep.ep_id);
				XART1.OutFormat("[USBH]     ep%d %s type=%d mps=%u interval=%u ch=%u done=%u nak=%u bad=%u\r\n",
					(int)ep.ep_id.Number(), ep.ep_id.IsIn() ? "in" : "out",
					(int)ep.ep_type, (unsigned)ep.max_packet_size, (unsigned)ep.interval,
					(unsigned)ch, (unsigned)ev_ch_done[ch & 15],
					(unsigned)ev_ch_nak[ch & 15], (unsigned)ev_ch_bad[ch & 15]);
				if (ch) {
					XART1.OutFormat("[USBH]       ch%u HCCHAR=0x%08X HCINT=0x%08X state=%u urb=%u err=%u count=%u\r\n",
						(unsigned)ch, (unsigned)_USBH_HCREG(ch, 0x00),
						(unsigned)_USBH_HCREG(ch, 0x08), (unsigned)HCD2.hc[ch].state,
						(unsigned)HCD2.hc[ch].urb_state, (unsigned)HCD2.hc[ch].ErrCnt,
						(unsigned)HCD2.hc[ch].xfer_count);
					// GRSTCTL.TXFFLSH(bit5)：置位就说明 TX FIFO 永久卡在 flush 状态，
					// 核心不会再发任何事务；HNPTXSTS 是 NP TX FIFO 的可用空间/队列状态
					XART1.OutFormat("[USBH]       GRSTCTL=0x%08X HNPTXSTS=0x%08X GINTSTS=0x%08X\r\n",
						(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x010)),
						(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x02C)),
						(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x014)));
					// 状态直方图：st3=NAK(设备回应了) st6=Xacterr(没人应答) st1=XFRC st5=Stall st8=翻转错
					XART1.OutFormat("[USBH]       ch%u st: st1=%u st2=%u st3=%u st4=%u st5=%u st6=%u st7=%u st8=%u\r\n",
						(unsigned)ch,
						(unsigned)ev_ch_state[ch & 15][1], (unsigned)ev_ch_state[ch & 15][2],
						(unsigned)ev_ch_state[ch & 15][3], (unsigned)ev_ch_state[ch & 15][4],
						(unsigned)ev_ch_state[ch & 15][5], (unsigned)ev_ch_state[ch & 15][6],
						(unsigned)ev_ch_state[ch & 15][7], (unsigned)ev_ch_state[ch & 15][8]);
				}
			}
		}
		if (!dev->IsHubBridge()) continue;
		XART1.OutFormat("[HUB]   hub addr=%u: %u ports, up=%u down=%u fail=%u, addressing_port=%u, events=%u\r\n",
			(unsigned)dev->AssignedAddress(), (unsigned)dev->HubNumPorts(),
			(unsigned)dev->ChildUpCount(), (unsigned)dev->ChildDownCount(),
			(unsigned)dev->ChildFailCount(), (unsigned)dev->HubAddressingPort(),
			(unsigned)dev->HubPortEventCount());
		for (byte port = 1; port <= dev->HubNumPorts() && port <= 16; port++) {
			OTGHostDevice* child = dev->ChildOfPort(port);
			XART1.OutFormat("[HUB]     port%u status=0x%04X change=0x%04X child=%s\r\n",
				(unsigned)port, (unsigned)dev->HubPortStatus(port),
				(unsigned)dev->HubPortChange(port),
				child ? (child->IsEnumerated() ? "up" : "enum...") : "none");
		}
	}
}

// ==================== 设备表：Hub 下游设备是库在中断里动态建出来的 ====================
// demo 不认设备类型：只按总线上的桥逐个报 VID/PID/class/端点/绑定驱动，
// 所以以后插鼠标、键盘、U 盘、还是再串一个 Hub，都会自动出现在这张表里。
static void PrintDeviceInfo(OTGHostDevice& dev);// 定义在下面

#define MAXCHILD 8
struct ChildSlot {
	OTGHostDevice* dev;
	bool seen;
	bool reported;
	uint32 deadline;
};
static ChildSlot childs[MAXCHILD] = {};

static bool BridgeAlive(OTGHostDevice* dev) {
	for (int i = 0; i < OTGHostDevice::kMaxBridges; i++) {
		if (OTGHostDevice::BridgeAt(i) == dev) return true;
	}
	return false;
}

static ChildSlot* SlotFor(OTGHostDevice* dev) {
	for (int i = 0; i < MAXCHILD; i++) {
		if (childs[i].dev == dev) return &childs[i];
		if (childs[i].dev == nullptr) {
			childs[i] = ChildSlot{};
			childs[i].dev = dev;
			return &childs[i];
		}
	}
	return nullptr;// 设备比表还多：不打印它的枚举结果，但设备本身照常工作
}

// 每毫秒跟随库里的桥表：新设备报接入与枚举结果，消失的设备清槽
static void ScanChildDevices(uint32 now_ms) {
	for (int i = 0; i < MAXCHILD; i++) {
		if (childs[i].dev == nullptr) continue;
		if (BridgeAlive(childs[i].dev)) continue;
		childs[i] = ChildSlot{};// 设备已经不在总线上了
	}
	for (int i = 0; i < OTGHostDevice::kMaxBridges; i++) {
		OTGHostDevice* dev = OTGHostDevice::BridgeAt(i);
		if (dev == nullptr || dev->IsRootDevice()) continue;// 根设备由原有的单设备逻辑报
		ChildSlot* slot = SlotFor(dev);
		if (slot == nullptr) continue;
		if (!slot->seen) {
			slot->seen = true;
			// 比库里的 3s 回收更早，保证超时现场一定打得出来
			slot->deadline = now_ms + 2500;
			XART1.OutFormat("[USBH] Hub device attached on port%u, addr=%u\r\n",
				(unsigned)dev->ParentPort(), (unsigned)dev->AssignedAddress());
		}
		if (!slot->reported && dev->IsEnumerated()) {
			slot->reported = true;
			PrintDeviceInfo(*dev);
		}
		if (!slot->reported && (int32)(now_ms - slot->deadline) >= 0) {
			slot->reported = true;
			XART1.OutFormat("[USBH] addr=%u enumeration did NOT finish in 2.5s\r\n",
				(unsigned)dev->AssignedAddress());
			// 控制通道现场：HCINT 决定是"完全没响应"还是 NAK/Stall
			// HCCHAR bit17 = LSDEV（低速设备），bit31 = CHENA
			const byte ch = dev->ControlChannel();
			XART1.OutFormat("[HUB] child ctrl ch=%u HCCHAR=0x%08X HCINT=0x%08X HCTSIZ=0x%08X\r\n",
				(unsigned)ch, (unsigned)_USBH_HCREG(ch, 0x00),
				(unsigned)_USBH_HCREG(ch, 0x08), (unsigned)_USBH_HCREG(ch, 0x10));
			XART1.OutFormat("[HUB] child ch%u speed=%u dad=%u epnum=%u in=%u eptyp=%u mps=%u state=%u urb=%u err=%u count=%u\r\n",
				(unsigned)ch, (unsigned)HCD2.hc[ch].speed, (unsigned)HCD2.hc[ch].dev_addr,
				(unsigned)HCD2.hc[ch].ep_num, (unsigned)HCD2.hc[ch].ep_is_in,
				(unsigned)HCD2.hc[ch].ep_type, (unsigned)HCD2.hc[ch].max_packet,
				(unsigned)HCD2.hc[ch].state, (unsigned)HCD2.hc[ch].urb_state,
				(unsigned)HCD2.hc[ch].ErrCnt, (unsigned)HCD2.hc[ch].xfer_count);
			XART1.OutFormat("[HUB] HAINT=0x%08X HAINTMSK=0x%08X HNPTXSTS=0x%08X GINTMSK=0x%08X\r\n",
				(unsigned)HCD2.HostReg(OTGHostReg::HAINT),
				(unsigned)HCD2.HostReg(OTGHostReg::HAINTMSK),
				(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x02C)),
				(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTMSK));
			// 端口现场：PENA 掉了就是核把根口关了（HUB 转发把它搞死），PSUSP 是挂起
			XART1.OutFormat("[USBH] HPRT0=0x%08X PENA=%u PCSTS=%u PSUSP=%u POCA=%u PSPD=%u HFIR=0x%08X GINTSTS=0x%08X\r\n",
				(unsigned)_USBH_HPRT0, (unsigned)((_USBH_HPRT0 >> USB_OTG_HPRT_PENA_Pos) & 1),
				(unsigned)((_USBH_HPRT0 >> USB_OTG_HPRT_PCSTS_Pos) & 1),
				(unsigned)((_USBH_HPRT0 >> USB_OTG_HPRT_PSUSP_Pos) & 1),
				(unsigned)((_USBH_HPRT0 >> USB_OTG_HPRT_POCA_Pos) & 1),
				(unsigned)((_USBH_HPRT0 >> USB_OTG_HPRT_PSPD_Pos) & 3),
				(unsigned)HCD2.HostReg(OTGHostReg::HFIR),
				(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTSTS));
			PrintDeviceTree();
		}
	}
}

// 主机状态快照（Down 键 / 枚举超时时打印）
static void PrintHostStatus() {
#if _USBH_DUMP
	uint32 hprt0 = _USBH_HPRT0;
	XART1.OutFormat("[USBH] HPRT0=0x%08X PCSTS=%u PENA=%u PPWR=%u PRST=%u PLSTS=%u PSPD=%u\r\n",
		(unsigned)hprt0,
		(unsigned)((hprt0 >> USB_OTG_HPRT_PCSTS_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PENA_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PPWR_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PRST_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PLSTS_Pos) & 3),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PSPD_Pos) & 3));
	XART1.OutFormat("[USBH] CH0 HCCHAR=0x%08X HCINT=0x%08X HCTSIZ=0x%08X urb=%u chstate=%u ep0mps=%u\r\n",
		(unsigned)_USBH_HCREG(0, 0x00), (unsigned)_USBH_HCREG(0, 0x08),
		(unsigned)_USBH_HCREG(0, 0x10),
		(unsigned)HCD2.getURBState(0), (unsigned)HCD2.getHostChannelState(0),
		(unsigned)HCD2.hc[0].max_packet);
	XART1.OutFormat("[USBH] CH0 in=%u pid=%u len=%u count=%u tgl_in=%u tgl_out=%u errcnt=%u\r\n",
		(unsigned)HCD2.hc[0].ep_is_in, (unsigned)HCD2.hc[0].data_pid,
		(unsigned)HCD2.hc[0].xfer_len, (unsigned)HCD2.hc[0].xfer_count,
		(unsigned)HCD2.hc[0].toggle_in, (unsigned)HCD2.hc[0].toggle_out,
		(unsigned)HCD2.hc[0].ErrCnt);
	XART1.OutFormat("[USBH] ev: IRQ=%u Conn=%u Disc=%u up=%ums URB=%u | bridge=%u enumerated=%u\r\n",
		(unsigned)ev_irq_cnt, (unsigned)ev_conn_cnt, (unsigned)ev_disc_cnt,
		(unsigned)SysTick::getTick(), (unsigned)ev_urb_cnt,
		(unsigned)(usb_otg_host ? 1 : 0),
		(unsigned)((usb_otg_host && usb_otg_host->IsEnumerated()) ? 1 : 0));
	uint32 gs = (uint32)HCD2.GlobalReg(OTGGlobalReg::GINTSTS);
	XART1.OutFormat("[USBH] GINTSTS=0x%08X (CMOD=%u SOF=%u RXFLVL=%u HPRTINT=%u HCINT=%u DISCINT=%u) seen=0x%08X\r\n",
		(unsigned)gs, (unsigned)(gs & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_SOF_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_RXFLVL_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_HPRTINT_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_HCINT_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_DISCINT_Pos) & 1),
		(unsigned)ev_gintsts_seen);
	XART1.OutFormat("[USBH] HAINT=0x%08X HAINTMSK=0x%08X GINTMSK=0x%08X\r\n",
		(unsigned)HCD2.HostReg(OTGHostReg::HAINT),
		(unsigned)HCD2.HostReg(OTGHostReg::HAINTMSK),
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTMSK));
	PrintItrLast();
	PrintUrbTrace();
#else
	// loop = 主循环迭代计数：loop/up 就是主循环实际频率，用来判断 ISR 是否把主循环饿住
	XART1.OutFormat("[USBH] ev: IRQ=%u Conn=%u Disc=%u up=%ums URB=%u loop=%u\r\n",
		(unsigned)ev_irq_cnt, (unsigned)ev_conn_cnt, (unsigned)ev_disc_cnt,
		(unsigned)SysTick::getTick(), (unsigned)ev_urb_cnt, (unsigned)ev_loop_cnt);
	XART1.OutFormat("[USBH] GINTSTS=0x%08X GINTMSK=0x%08X HAINT=0x%08X HAINTMSK=0x%08X HFNUM=0x%08X\r\n",
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTSTS),
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTMSK),
		(unsigned)HCD2.HostReg(OTGHostReg::HAINT),
		(unsigned)HCD2.HostReg(OTGHostReg::HAINTMSK),
		(unsigned)HCD2.HostReg(OTGHostReg::HFNUM));
	XART1.OutFormat("[USBH] HPRT0=0x%08X PCGCCTL=0x%08X GOTGCTL=0x%08X\r\n",
		(unsigned)_USBH_HPRT0,
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + USB_OTG_PCGCCTL_BASE)),
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x000)));
	// GUSBCFG.FHMOD=bit29（是否还在强制主机模式）、GCCFG.VBDEN=bit21 / PWRDWN=bit16（是否又在检测 VBUS）、
	// GOTGINT.SEDET=bit2 / SRSSCHG=bit8（会话结束/SRP 状态变化是否发生过）
	XART1.OutFormat("[USBH] GUSBCFG=0x%08X GCCFG=0x%08X GOTGINT=0x%08X GCCFG_CHK=0x%08X\r\n",
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x00C)),
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x038)),
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x004)),
		(unsigned)(*(volatile uint32*)(_OTG2_FS_ADDR + 0x000)));
	// FSLSPCS=HCFG bit1:0（PHY 时钟选择）、FRIVL=HFIR bit15:0（帧间隔）；
	// 连采三次 HFNUM，中间各忙等约 3ms：FRNUM 若三次相同，说明帧计数器没有在走
	{
		const uint32 h1 = (uint32)HCD2.HostReg(OTGHostReg::HFNUM);
		for (volatile uint32 w = 0; w < 200000; w++) {}
		const uint32 h2 = (uint32)HCD2.HostReg(OTGHostReg::HFNUM);
		for (volatile uint32 w = 0; w < 200000; w++) {}
		const uint32 h3 = (uint32)HCD2.HostReg(OTGHostReg::HFNUM);
		XART1.OutFormat("[USBH] HCFG=0x%08X FSLSPCS=%u HFIR=0x%08X\r\n",
			(unsigned)HCD2.HostReg(OTGHostReg::HCFG),
			(unsigned)((uint32)HCD2.HostReg(OTGHostReg::HCFG) & USB_OTG_HCFG_FSLSPCS_Msk),
			(unsigned)HCD2.HostReg(OTGHostReg::HFIR));
		XART1.OutFormat("[USBH] HFNUM x3: 0x%08X 0x%08X 0x%08X (FRNUM must advance between samples)\r\n",
			(unsigned)h1, (unsigned)h2, (unsigned)h3);
	}
#endif
	// 阶段0/3/4 的绑定、坐标与按键证据：库只在建出 HIDMouseDriver 时才订阅观察者
	XART1.OutFormat("[HID] mouse reports=%u pos=(%d,%d) last: btn=0x%02X x=%d y=%d clicks=L%u R%u M%u\r\n",
		(unsigned)mouse_report_cnt, (int)mouse_x, (int)mouse_y,
		(unsigned)mouse_last[0],
		(int)(int8)mouse_last[1], (int)(int8)mouse_last[2],
		(unsigned)mouse_click_cnt[0], (unsigned)mouse_click_cnt[1],
		(unsigned)mouse_click_cnt[2]);
	PrintDeviceTree();
}

// 枚举完成后打印设备信息，以及每个端点的类驱动绑定情况
static void PrintDeviceInfo(OTGHostDevice& dev) {
	XART1.OutFormat("[USBH] VID=0x%04X PID=0x%04X class=0x%02X sub=0x%02X proto=0x%02X\r\n",
		(unsigned)dev.VendorID(), (unsigned)dev.ProductID(),
		(unsigned)dev.DeviceClass(), (unsigned)dev.DeviceSubClass(), (unsigned)dev.DeviceProtocol());
	const char* str = dev.ManufacturerString();
	if (str) XART1.OutFormat("[USBH] Manufacturer: %s\r\n", str);
	str = dev.ProductString();
	if (str) XART1.OutFormat("[USBH] Product: %s\r\n", str);
	str = dev.SerialString();
	if (str) XART1.OutFormat("[USBH] Serial: %s\r\n", str);
	const int eps = dev.NumEndpointConfigs();
	XART1.OutFormat("[USBH] endpoint configs=%d\r\n", eps);
	int bound = 0;
	for (int i = 0; i < eps; i++) {
		const EndpointConfig& ep = dev.EndpointConfigs()[i];
		ClassDriver* d = dev.ClassDriverOf(ep.ep_id.Number());
		if (d) bound++;
		XART1.OutFormat("[HID] ep[%d] id=%d dir=%s type=%d mps=%u interval=%u driver=%s\r\n",
			i, (int)ep.ep_id.Number(), ep.ep_id.IsIn() ? "in" : "out",
			(int)ep.ep_type, (unsigned)ep.max_packet_size, (unsigned)ep.interval,
			d ? "bound" : "none");
	}
	XART1.OutFormat("[USBH] Enumeration OK\r\n");
	XART1.OutFormat("[HID] class driver bound: %d\r\n", bound);
}

// ==================== 阶段5：LTDC 屏（测试图案） ====================
// 8 条竖色条 + 白边框，用于检查分辨率、色序和行宽
static void lcd_test_pattern() {
	static const uint16 bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
	uint16* fb = (uint16*)SDRAM_BANK1_BASE;
	for (stduint y = 0; y < LCD_H; y++) {
		for (stduint x = 0; x < LCD_W; x++) {
			const bool edge = (x < 4 || y < 4 || x >= LCD_W - 4 || y >= LCD_H - 4);
			fb[y * LCD_W + x] = edge ? 0xFFFF : bars[(x * 8) / LCD_W];
		}
	}
	ltdc_text(10, 10, "82-USBM-Mouse + LTDC 800x480");
	ltdc_text(10, 30, "cursor comes in stage 6");
	// 本 demo 只开 I-cache（L1C.enAbleICacheAll），CPU 写直接落 SDRAM，所以不用清 D-cache
	// 若以后开了 D-cache，这里必须改成 SCB_CleanDCache_by_Addr(帧缓冲, 大小) 再让 LTDC 显示
}

int main() {
	// Mempool：框架的 new/malloc 靠它
	{
		extern uni::Mempool mempool;
		mempool.enable_auto_expand = false;
		mempool.Append(Slice{ (stduint)(pureptr_t)_heap_pool, byteof(_heap_pool) });
	}

	L1C.enAbleICacheAll();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro("Clock init failed");
	// SysTick 必须放到最高抢占优先级。SysTick::enClock 给的是最低优先级（uwTickPrio=15），
	// 而 USB ISR 是 1；USB ISR 里有 FlushTxFifo（最多 20 万次寄存器轮询）和
	// OnDeviceAddressChanged（忙等 3 个 USB 帧）这种长时间忙等，会把 SysTick 压住，
	// 于是 SysTick::getTick() 落后真实时间、主循环的 1ms 调度变稀、中断端点轮询跟着变慢。
	// 必须在 setPriorityGroup 之后设，否则分组变化会把原来的优先级值重新解释。
	NVIC.setPriority(IRQ_SysTick, 0);

	GPIOA.enClock(true);
	GPIOB.enClock(true);
	GPIOC.enClock(true);
	GPIOG.enClock(true);
	GPIOH.enClock(true);

	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;

	// ---- 按键：EXTI 中断模式，优先级 3（低于 USB 的 1） ----
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// Up    (PA0, 高有效)
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true); // Left  (PC13)
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true); // Down  (PH2)
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true); // Right (PH3)
	KEYU.setMode(GPIORupt::Posedge);
	KEYL.setMode(GPIORupt::Negedge);
	KEYD.setMode(GPIORupt::Negedge);
	KEYR.setMode(GPIORupt::Negedge);
	KEYU.setInterrupt(key_isr_up);
	KEYL.setInterrupt(key_isr_lf);
	KEYD.setInterrupt(key_isr_dn);
	KEYR.setInterrupt(key_isr_rt);
	KEYU.setInterruptPriority(3, 0);
	KEYL.setInterruptPriority(3, 0);
	KEYD.setInterruptPriority(3, 0);
	KEYR.setInterruptPriority(3, 0);
	KEYU.enInterrupt();
	KEYL.enInterrupt();
	KEYD.enInterrupt();
	KEYR.enInterrupt();

	XART1.setMode(115200);
	SysDelay_ms(200);

	// ---- 阶段5：先把屏点亮，USB 那条链路完全不动 ----
	sdram_init();
	ltdc_init();
	// 第 1 层是 RGB565（没有像素 alpha），显式用"常量 alpha"混合，免得被算成全透明
	LTDC[1][LTDCLayerReg::CACR] = 0xFF;// CONSTA 全不透明
	LTDC[1][LTDCLayerReg::BFCR] = (0b100 << 8) | 0b101;// BF1=CA, BF2=1-CA
	LTDC.Reload(LTDCReload::Immediate);
	lcd_test_pattern();
	cursor_build();
	cursor_layer_init();
	XART1.OutFormat("[LCD] LTDC OK, PixClk=%dHz\r\n", (int)LTDC.getFrequency());

	XART1.OutFormat("\r\n");
	XART1.OutFormat("====================================================\r\n");
	XART1.OutFormat("  UNISYM STM32H743 82-USBM-Mouse\r\n");
	XART1.OutFormat("  USB Host (OTG2_FS) bring-up + enumeration\r\n");
	XART1.OutFormat("====================================================\r\n");

	// ==================== 主机口供电 ====================
	XART1.OutFormat("Initializing USB Host...\r\n");
#if USBH_PWR_MODE == 1
	pcf_iic = new (_pcf_wire_buf) PcfIIC(GPIOH[5], GPIOH[4]);// SDA=PH5, SCL=PH4
	pcf_iic->func_delay = iic_delay;
#endif
	UsbHostDriverVBUS(true);
	SysDelay_ms(500);// 等鼠标供电稳定

	// ==================== USB 时钟与引脚 ====================
	// 0) VDD33USB 检测器：等稳压就绪（HSI48 作 USB 时钟的前提）
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// PWR_CR3.USB33DEN
	while (!(*(volatile stduint*)0x5802480CU & 0x04000000U));// 等 USB33RDY

	// 1) 48MHz 内核时钟：HSI48
	RCC[uni::RCCReg::CR].setof(12, true);          // HSI48ON
	while (!RCC[uni::RCCReg::CR].bitof(13));       // 等 HSI48RDY
	uni::RCC.setPeriphClock(uni::PeriphClock::USB, uni::ClockSource::HSI48);// D2CCIP2R.USBSEL

	// 2) OTG2_FS 引脚 PA11/PA12 = AF10
	GPIOA[11].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOA[12].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);

	// ==================== HCD2：主机模式 ====================
	HCD2.base = _OTG2_FS_ADDR;
	HCD2.speed = 3;              // 控制器级 FULL，内嵌 FS PHY 48MHz
	HCD2.host_channels = 8;
	HCD2.phy_itface = 2;         // 内嵌 PHY
	HCD2.dma_enable = false;
	HCD2.use_external_vbus = false;
	if (!HCD2.setMode()) erro("HCD Init Error");

	HCD2.ConnectHandler = on_hcd_connect;
	HCD2.DisconnectHandler = on_hcd_disconnect;
	HCD2.NotifyURBChangeHandler = on_hcd_urb;
	// 键鼠驱动建立时库会自动订阅这两个默认观察者，所以要在枚举之前挂上
	HIDMouseDriver::default_observer = on_mouse_report;
	HIDKeyboardDriver::default_observer = on_key_push;
	HCD2.setInterrupt(on_usb_fs_interrupt);
	HCD2.setInterruptPriority(1, 0);
	HCD2.enInterrupt();

	HCD2.Start();// 内含 HPRT0.PPWR 驱动 VBUS
	// SOF 中断保留：卡住时只有它能让 ISR 定期进来查看通道挂起位

	XART1.OutFormat("[USBH] Host controller started (HPRT0=0x%08X)\r\n", (unsigned)_USBH_HPRT0);
	XART1.OutFormat("[USBH] Plug a USB mouse into the HOST port.\r\n");

	uint32 tick = 0;
	uint32 hb_ms = 0;
	uint32 seen_conn = 0, seen_disc = 0;
	uint32 seen_up = 0, seen_dn = 0, seen_lf = 0, seen_rt = 0;
	uint32 ms_up = 0, ms_dn = 0;
	uint32 enum_deadline = 0;
	bool enum_reported = false;
	uint32 next_poll_ms = 0;
	uint32 cur_seen = 0;
	int32 seen_x = 0, seen_y = 0;
	byte seen_btn = 0;
	uint32 ms_mouse = 0;
	uint32 seen_kbev = 0;
	// 端口死亡探测（帧号停走 = 核不再发 SOF = 根口已被关闭）与就地自救状态
	uint32 frnum_seen = (uint32)HCD2.getCurrentFrame();
	uint32 port_dead_ms = 0;
	uint32 port_recover_cnt = 0;
	uint32 port_recover_at = 0;
	byte port_recover_stage = 0;

	// ==================== 主循环 ====================
	while (true) {
		ev_loop_cnt++;
		uint32 now_ms = (uint32)SysTick::getTick();

		// ---- 总线上每个设备每毫秒跑一次（根设备 + Hub 下游设备）----
		// 关键：Hub 类驱动的端口策略（给下游口上电、复位端口）就是靠 ProcessDelayed()
		// 推进的；原来只调 ProcessDeferredInterrupts() 的话 Hub 永远不给下游口上电。
		if ((int32)(now_ms - next_poll_ms) >= 0) {
			next_poll_ms = now_ms + 1;
			OTGHostDevice::TickAll();
			// 帧号正常每毫秒推进一格；它停走就说明核不再发 SOF，即根口已被关闭
			const uint32 frnum_now = (uint32)HCD2.getCurrentFrame();
			if (frnum_now != frnum_seen) {
				frnum_seen = frnum_now;
				port_dead_ms = 0;
			}
			else if (port_dead_ms < 1000) port_dead_ms++;
		}

		// ---- 端口被判死（HUB 的 LS 转发把根口搞掉）之后就自救：断 VBUS 清 HUB 状态，重来一遍 ----
		if (!port_recover_stage && port_dead_ms >= 200 && usb_otg_host && UsbHostPortConnected()
			&& port_recover_cnt < 3) {
			port_recover_cnt++;
			port_recover_stage = 1;
			port_recover_at = now_ms + 400;
			UsbHostDriverVBUS(false);
			UsbHostDropBridge();
			enum_reported = false;
			XART1.OutFormat("[USBH] PORT DEAD: frame counter stopped, HUB power-cycle (%u/3)\r\n",
				(unsigned)port_recover_cnt);
		}
		if (port_recover_stage && (int32)(now_ms - port_recover_at) >= 0) {
			if (port_recover_stage == 1) {
				UsbHostDriverVBUS(true);
				port_recover_stage = 2;
				port_recover_at = now_ms + 600;
			}
			else {
				port_recover_stage = 0;
				port_dead_ms = 0;
				frnum_seen = (uint32)HCD2.getCurrentFrame();
				// babble 之后核不会自己再发 token：软复位 + 重新初始化，再走一次接入流程
				if (!HCD2.setMode()) XART1.OutFormat("[USBH] PORT RECOVER: HCD re-init failed\r\n");
				HCD2.setInterrupt(on_usb_fs_interrupt);
				HCD2.enInterrupt();
				HCD2.Start();
				XART1.OutFormat("[USBH] PORT RECOVER: core re-initialized (%u/3)\r\n",
					(unsigned)port_recover_cnt);
				if (UsbHostPortConnected()) {
					seen_conn = ev_conn_cnt;
					enum_reported = false;
					UsbHostStartEnumeration(enum_deadline);
				}
			}
		}

		// ---- 设备表：库在中断里新建/删除桥，这里跟着报接入与枚举结果 ----
		ScanChildDevices(now_ms);

		// ---- 键盘按键：事件在中断里记录，这里打印 ----
		PrintKeyEvents(seen_kbev);

		// ---- 阶段6：光标（位移在中断里入队，这里出队并按屏坐标夹取） ----
		while (cur_seen != cur_dq_cnt) {
			const uint32 i = cur_seen++ & 31;
			cursor_move(cur_dq_x[i], cur_dq_y[i]);
		}

		// ---- 阶段3：鼠标回显 事件在中断里记录，这里打印 ----
#if _MOUSE_LOG
		{
			int32 x = mouse_x, y = mouse_y;
			byte btn = mouse_btn;
			const bool moved = (x != seen_x || y != seen_y);
			const bool btn_changed = (btn != seen_btn);
			if (btn_changed || (moved && (int32)(now_ms - ms_mouse) >= 200)) {
				ms_mouse = now_ms;
				seen_x = x;
				seen_y = y;
				seen_btn = btn;
				XART1.OutFormat("[MOUSE] x=%d y=%d btn=0x%02X dx=%d dy=%d\r\n",
					(int)x, (int)y, (unsigned)btn,
					(int)(int8)mouse_last[1], (int)(int8)mouse_last[2]);
			}
		}
#endif

		// ---- 拔出 ----
		if (ev_disc_cnt != seen_disc) {
			seen_disc = ev_disc_cnt;
			if (usb_otg_host) {
				UsbHostDropBridge();
				enum_reported = false;
				XART1.OutFormat("[USBH] Device detached (event #%u)\r\n", (unsigned)seen_disc);
			}
		}

		// ---- 插入：端口复位 -> 建传输桥 -> 开始枚举 ----
		// ConnectHandler 在 PCDET / PENCHNG 上各回调一次，所以以端口状态为准
		if (ev_conn_cnt != seen_conn) {
			seen_conn = ev_conn_cnt;
			if (!usb_otg_host && UsbHostPortConnected()) {
				XART1.OutFormat("[USBH] Device attached (event #%u)\r\n", (unsigned)seen_conn);
				enum_reported = false;
				UsbHostStartEnumeration(enum_deadline);
			}
		}

		// ---- 枚举结果 ----
		if (usb_otg_host && !enum_reported && usb_otg_host->IsEnumerated()) {
			enum_reported = true;
			PrintDeviceInfo(*usb_otg_host);
#if _USBH_DUMP
			hid_stage1_ms = now_ms + 500;// 给驱动一点时间跑完 SET_PROTOCOL 与首次提交
			Stage1AskConfiguration(*usb_otg_host);
#endif
		}

		// ---- 阶段1：HID 握手 + 中断端点首次提交 ----
#if _USBH_DUMP
		if (usb_otg_host && enum_reported && !hid_stage1_reported && hid_stage1_ms &&
			(int32)(now_ms - hid_stage1_ms) >= 0) {
			hid_stage1_reported = true;
			PrintHidStage1();
		}
#endif

		// ---- 第一次出现非 Done 完成时，立刻把现场打出来 ----
		if (!urb_trace_reported && ev_urb_bad_cnt) {
			urb_trace_reported = true;
			PrintHostStatus();
		}

		// ---- 枚举超时 ----
		if (usb_otg_host && !enum_reported && (int32)(now_ms - enum_deadline) >= 0) {
			enum_reported = true;
			XART1.OutFormat("[USBH] Enumeration did NOT finish in 3s:\r\n");
			PrintHostStatus();
		}

		// ---- 心跳：证明主循环没被卡住（默认关闭，刷屏时开 _USBH_HEARTBEAT 才有） ----
#if _USBH_HEARTBEAT
		if ((now_ms - hb_ms) >= 2000) {
			hb_ms = now_ms;
			XART1.OutFormat("[USBH] heartbeat: bridge=%u enumerated=%u irq=%u urb=%u conn=%u disc=%u\r\n",
				(unsigned)(usb_otg_host ? 1 : 0),
				(unsigned)((usb_otg_host && usb_otg_host->IsEnumerated()) ? 1 : 0),
				(unsigned)ev_irq_cnt, (unsigned)ev_urb_cnt,
				(unsigned)ev_conn_cnt, (unsigned)ev_disc_cnt);
		}
#else
		(void)hb_ms;
#endif

		// ---- 按键（EXTI 事件 + 消抖） ----
		if (seen_up != key_ev_up) {// Up：重新复位端口并重新枚举
			seen_up = key_ev_up;
			if (now_ms - ms_up > 200) {
				ms_up = now_ms;
				XART1.OutFormat("[USBH] Re-enumerate (port reset by key)...\r\n");
				enum_reported = false;
				if (!UsbHostStartEnumeration(enum_deadline)) {
					enum_reported = true;
					XART1.OutFormat("[USBH] No device on the host port.\r\n");
				}
			}
		}
		if (seen_dn != key_ev_dn) {// Down：状态快照
			seen_dn = key_ev_dn;
			// 这份 dump 在 115200 下要 ~150ms 才打完，节流必须远大于它，否则一次按压就会
			// 让主循环每轮都耗在串口上，TickAll() 被饿死、Hub 端口扫描和 USB 栈全部停摆
			if (now_ms - ms_dn > 1000) {
				ms_dn = now_ms;
				PrintHostStatus();
			}
		}
		if (seen_lf != key_ev_lf) seen_lf = key_ev_lf;// Left：保留
		if (seen_rt != key_ev_rt) seen_rt = key_ev_rt;// Right：保留

		// ---- 心跳灯 ----
		tick++;
		if (tick >= 250000) {
			tick = 0;
			LEDB.Toggle();
		}
	}
}

OstreamTrait* con0_out = 0;
