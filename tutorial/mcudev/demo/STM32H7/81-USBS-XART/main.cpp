// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 81-USBS-XART：USB CDC 虚拟串口
 *
 * 已实现：
 *   1) 枚举与控制面：设备管理器出现 "USB 串行设备 (COMx)"（Win10/11 免 INF）；
 *   2) 类请求：SET_LINE_CODING / GET_LINE_CODING / SET_CONTROL_LINE_STATE；
 *   3) 接收（Bulk OUT）：PC 发来的字节入环形队列，主循环排空并打印；
 *   4) 回显（Bulk IN）：把收到的字节原样发回 PC，队列满则丢弃并计数（不阻塞）。
 *
 * 键位（Up/Down/Left/Right = PA0/PH2/PC13/PH3，EXTI 中断）：
 *   Up    → 软重连（模拟拔插，DCTL.SDIS 脉冲 500ms）
 *   Down  → 打印 CDC 状态快照
 *   Left / Right → 保留（暂无动作）
 */

#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/USB/USBPeri-Device.hpp>
#include <cpp/Device/USB/USBPeri-Class.hpp>
#include <cpp/Device/USB/USBPeri-CDC.hpp>
#include <c/driver/_predefine/predef.usbperi.hpp>
#include <c/mempool.h>
#include <c/consio.h>
#include <c/ustring.h>
#include <c/system/debug.h>
#include <new>

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

static byte _heap_pool[64 * 1024];

extern "C" void outtxt(const char* str, stduint len) { XART1.out(str, len); }

// ---- 按键中断：ISR 只计数，动作留给主循环（不阻塞 USB） ----
static volatile uint32 key_ev_up = 0, key_ev_dn = 0, key_ev_lf = 0, key_ev_rt = 0;
void key_isr_up() { key_ev_up++; }
void key_isr_dn() { key_ev_dn++; }
void key_isr_lf() { key_ev_lf++; }
void key_isr_rt() { key_ev_rt++; }

// ==================== USB 描述符 (CDC-ACM) ====================
// 设备级 bDeviceClass = 0x02 (CDC)：Win10/11 据此自动绑定 usbser.sys，免装 INF。
// VID/PID 用 ST 标准 VCP 0483:5740（与参考例程 实验58 的 usbd_desc.c 一致）。
static const byte _cdc_dev_desc[18] = {
	0x12,                       // bLength
	0x01,                       // bDescriptorType = Device
	0x00, 0x02,                 // bcdUSB 2.00
	0x02, 0x00, 0x00,           // bDeviceClass = CDC, SubClass, Protocol
	64,                         // bMaxPacketSize0
	0x83, 0x04,                 // idVendor  = 0x0483
	0x40, 0x57,                 // idProduct = 0x5740
	0x00, 0x02,                 // bcdDevice = 2.00
	0x01, 0x02, 0x03,           // iManufacturer / iProduct / iSerialNumber
	0x01                        // bNumConfigurations
};

static byte _str_desc[66];

static const byte* MakeString(const char* s) {
	const char* p = s;
	stduint n = 0;
	while (*p) { p++; n++; }
	byte idx = 0;
	_str_desc[idx++] = (byte)(n * 2 + 2);
	_str_desc[idx++] = 0x03;
	while (*s) {
		_str_desc[idx++] = (byte)*s++;
		_str_desc[idx++] = 0x00;
	}
	return _str_desc;
}

static const byte* CDC_DeviceDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	len = sizeof(_cdc_dev_desc);
	return _cdc_dev_desc;
}
static const byte* CDC_LangIDDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	static const byte lang[] = { 0x04, 0x03, 0x09, 0x04 };
	len = sizeof(lang);
	return lang;
}
static const byte* CDC_ManufacturerDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("STMicroelectronics");
	len = r[0];
	return r;
}
static const byte* CDC_ProductDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString(_IDN_BOARD);
	len = r[0];
	return r;
}
static const byte* CDC_SerialDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("00000000001A");
	len = r[0];
	return r;
}
static const byte* CDC_ConfigurationStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("CDC Config");
	len = r[0];
	return r;
}
static const byte* CDC_InterfaceStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("CDC Interface");
	len = r[0];
	return r;
}

// ==================== USB 事件转发 ====================
static PeripheralDevice* p_usb_core = nullptr;
static USBPeri_CDC* p_usb_cdc = nullptr;

static volatile uint32 ev_reset_cnt = 0;
static volatile uint32 ev_setup_cnt = 0;
static volatile uint32 ev_susp_cnt = 0;
static volatile uint32 ev_resume_cnt = 0;
static volatile uint32 ev_irq_cnt = 0;
struct LastUSBReq {
	uint8 req_type;
	uint8 req;
	uint16 val;
	uint16 idx;
	uint16 len;
	uint32 seq;
};
static volatile LastUSBReq g_last_req{};

// 控制面跟踪开关：默认关闭，日志保持干净；联调枚举时可改成 1。
#define _CDC_SETUP_TRACE 0

void on_setup() {
	if (p_usb_core) {
		p_usb_core->HandleSetup();
		ev_setup_cnt++;
		g_last_req.req_type = p_usb_core->request.request_type;
		g_last_req.req = p_usb_core->request.request;
		g_last_req.val = p_usb_core->request.value;
		g_last_req.idx = p_usb_core->request.index;
		g_last_req.len = p_usb_core->request.length;
		g_last_req.seq = ev_setup_cnt;
	}
}
void on_reset() {
	ev_reset_cnt++;
	if (p_usb_core) p_usb_core->HandleReset();
}
void on_suspend() {
	ev_susp_cnt++;
	if (p_usb_core) p_usb_core->HandleSuspend();
}
void on_resume() {
	ev_resume_cnt++;
	if (p_usb_core) p_usb_core->HandleResume();
}
void on_sof() { if (p_usb_core) p_usb_core->HandleSOF(); }
void on_connect() { if (p_usb_core) p_usb_core->HandleConnect(); }
void on_disconnect() { if (p_usb_core) p_usb_core->HandleDisconnect(); }
void on_dataout(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataOut((byte)(stduint)epnum); }
void on_datainn(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataInn((byte)(stduint)epnum); }
void on_usb_fs_interrupt() { ev_irq_cnt++; }

void erro(const char* str) {
	if (str) XART1.OutFormat("[Error] %s\r\n", str);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i{0}; i < 1000000; i++) {}
	}
}

// 硬件异常时打印现场，便于定位（框架的弱定义只调 erro() 不打印任何东西）
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

static void PrintCDCStatus() {
	XART1.OutFormat("[CDC] line=%u bps, %u bits, parity=%u, stop=%u | DTR=%d RTS=%d open=%d\r\n",
		(unsigned)p_usb_cdc->line_coding.dwDTERate,
		(unsigned)p_usb_cdc->line_coding.bDataBits,
		(unsigned)p_usb_cdc->line_coding.bParityType,
		(unsigned)p_usb_cdc->line_coding.bCharFormat,
		(int)((p_usb_cdc->line_state >> 0) & 1),
		(int)((p_usb_cdc->line_state >> 1) & 1),
		(int)(p_usb_cdc->isOpen() ? 1 : 0));
	XART1.OutFormat("[CDC] rx_packets=%u rx_bytes=%u ring=%u overflow=%u empty=%u\r\n",
		(unsigned)p_usb_cdc->rx_packets, (unsigned)p_usb_cdc->rx_bytes,
		(unsigned)p_usb_cdc->RxCount(), (unsigned)p_usb_cdc->rx_overflow,
		(unsigned)p_usb_cdc->rx_empty);
	XART1.OutFormat("[CDC] tx_bytes=%u tx_packets=%u tx_dropped=%u busy=%d free=%u\r\n",
		(unsigned)p_usb_cdc->tx_bytes, (unsigned)p_usb_cdc->tx_packets,
		(unsigned)p_usb_cdc->tx_dropped, (int)(p_usb_cdc->TxBusy() ? 1 : 0),
		(unsigned)p_usb_cdc->TxFree());
	XART1.OutFormat("[CDC] uptime=%us (long-run check: ovf/drop must stay 0)\r\n",
		(unsigned)(SysTick::getTick() / 1000));
}

int main() {
	// Mempool：_MEMMAN.cpp 的 new/malloc 靠它
	{
		extern uni::Mempool mempool;
		mempool.enable_auto_expand = false;
		mempool.Append(Slice{ (stduint)(pureptr_t)_heap_pool, byteof(_heap_pool) });
	}

	L1C.enAbleICacheAll();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro("Clock init failed");

	GPIOA.enClock(true);
	GPIOB.enClock(true);
	GPIOC.enClock(true);
	GPIOH.enClock(true);
	GPIOG.enClock(true);

	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;

	// ---- 按键：EXTI 中断模式（优先级 3 = 低于 USB 的 1），主循环只处理事件 ----
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

	XART1.OutFormat("\r\n");
	XART1.OutFormat("====================================================\r\n");
	XART1.OutFormat("  UNISYM STM32H743 81-USBS-XART \r\n");
	XART1.OutFormat("  CDC Virtual COM Port\r\n");
	XART1.OutFormat("====================================================\r\n");

	// ==================== USB OTG2_FS 设备栈 ====================
	XART1.OutFormat("Initializing USB CDC Device Stack...\r\n");

	// 0) VDD33USB 检测器：等稳压就绪（HSI48 作 USB 时钟的前提）
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// PWR_CR3.USB33DEN
	while (!(*(volatile stduint*)0x5802480CU & 0x04000000U));// wait USB33RDY

	// 1) 48MHz 内核时钟：HSI48
	RCC[uni::RCCReg::CR].setof(12, true);          // HSI48ON
	while (!RCC[uni::RCCReg::CR].bitof(13));       // wait HSI48RDY
	uni::RCC.setPeriphClock(uni::PeriphClock::USB, uni::ClockSource::HSI48);// D2CCIP2R.USBSEL

	// 2) OTG2_FS 引脚 PA11/PA12 = AF10
	GPIOA[11].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOA[12].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);

	// 3) PCD2 = USB2_OTG_FS（内嵌 FS 收发器）
	PCD2.base = _OTG2_FS_ADDR;
	PCD2.speed = 3;             // DEVSPD=3: FS, 48MHz 内嵌 PHY
	PCD2.ep0_mps = 64;
	PCD2.dev_endpoints = 8;
	PCD2.phy_itface = 2;        // 内嵌 PHY
	PCD2.dma_enable = false;
	PCD2.vbus_sensing_enable = false;// 板上无 VBUS 检测线
	PCD2.sof_enable = false;
	PCD2.lpm_enable = false;
	PCD2.battery_charging_enable = false;
	PCD2.use_dedicated_ep1 = false;
	if (!PCD2.setMode()) erro("PCD Init Error");

	// 4) FIFO：RX 128 words，TX0(EP0) 64 words，TX1(Bulk IN) 64 words
	PCD2.setRxFiFo(0x80);
	PCD2.setTxFiFo(0, 0x40);
	PCD2.setTxFiFo(1, 0x40);

	// 5) 设备栈 + 注册 CDC 类
	static PeripheralDescriptor desc;
	static PeripheralDevice usb_core;
	static USBPeri_CDC usb_cdc;
	p_usb_core = &usb_core;
	p_usb_cdc = &usb_cdc;

	desc.GetDeviceDescriptor = CDC_DeviceDescriptor;
	desc.GetLangIDStrDescriptor = CDC_LangIDDescriptor;
	desc.GetManufacturerStrDescriptor = CDC_ManufacturerDescriptor;
	desc.GetProductStrDescriptor = CDC_ProductDescriptor;
	desc.GetSerialStrDescriptor = CDC_SerialDescriptor;
	desc.GetConfigurationStrDescriptor = CDC_ConfigurationStrDescriptor;
	desc.GetInterfaceStrDescriptor = CDC_InterfaceStrDescriptor;

	usb_core.setMode(PCD2, desc);
	usb_core.RegisterClass(usb_cdc);

	// 6) 中断接线
	PCD2.SetupStageHandler = on_setup;
	PCD2.ResetHandler = on_reset;
	PCD2.SuspendHandler = on_suspend;
	PCD2.ResumeHandler = on_resume;
	PCD2.SOFHandler = on_sof;
	PCD2.ConnectHandler = on_connect;
	PCD2.DisconnectHandler = on_disconnect;
	PCD2.DataOutStageHandler = on_dataout;
	PCD2.DataInStageHandler = on_datainn;
	PCD2.setInterrupt(on_usb_fs_interrupt);
	PCD2.setInterruptPriority(1, 0);
	PCD2.enInterrupt();

	// 7) 启动（拉 D+、开全局中断）
	usb_core.Start();
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// AKA HAL_PWREx_EnableUSBVoltageDetector

	XART1.OutFormat("[USB] Started! DCTL=0x%08X (SDIS=%d), PWR_CR3=0x%08X (RDY=%d)\r\n",
		(unsigned)*(volatile uint32*)(_OTG2_FS_ADDR + 0x804),
		(int)((*(volatile uint32*)(_OTG2_FS_ADDR + 0x804) >> 1) & 1),
		(unsigned)*(volatile uint32*)0x5802480C,
		(int)((*(volatile uint32*)0x5802480C >> 26) & 1));
	XART1.OutFormat("[USB] Ready! Plug the USB cable into the PC.\r\n");
	XART1.OutFormat("[CDC] Ready: enumeration + control plane + bulk OUT receive + echo.\r\n");

	uint32 tick = 0;

	// ==================== 主循环 ====================
	while (true) {
		uint32 now_ms = SysTick::getTick();// 全循环共用的时间基准（按键消抖 / 统计）

		// ---- CDC 控制面事件（中断里只置标志，这里打印） ----
		if (usb_cdc.line_coding_updated) {
			usb_cdc.line_coding_updated = false;
			XART1.OutFormat("[CDC] Host SET_LINE_CODING: %u bps, %u data bits, parity=%u, stop=%u\r\n",
				(unsigned)usb_cdc.line_coding.dwDTERate,
				(unsigned)usb_cdc.line_coding.bDataBits,
				(unsigned)usb_cdc.line_coding.bParityType,
				(unsigned)usb_cdc.line_coding.bCharFormat);
		}
		if (usb_cdc.line_state_updated) {
			usb_cdc.line_state_updated = false;
			XART1.OutFormat("[CDC] Host SET_CONTROL_LINE_STATE: DTR=%d RTS=%d -> port %s\r\n",
				(int)((usb_cdc.line_state >> 0) & 1),
				(int)((usb_cdc.line_state >> 1) & 1),
				usb_cdc.isOpen() ? "OPENED" : "closed");
		}

		// ---- 热拔插 / 重新枚举：总线复位时类已复位收发队列与 FIFO ----
		static uint32 last_reset = 0;
		if (ev_reset_cnt != last_reset) {
			last_reset = ev_reset_cnt;
			XART1.OutFormat("[USB] Bus reset #%u -> re-enumerating (RX/TX queues reset)\r\n",
				(unsigned)last_reset);
		}

		// ---- 接收（Bulk OUT） ----
		// 中断只入队，这里排空并打印：一次最多 32 字节；全部可打印就原样显示，
		// 含不可打印字符时同时给出十六进制列。
		if (usb_cdc.RxCount()) {
			byte buf[32];
			uint32 n = usb_cdc.Rx(buf, sizeof(buf));
			char txt[33];
			char hex[32 * 3 + 1];
			uint32 tp = 0, hp = 0;
			bool ascii_ok = true;
			for (uint32 i = 0; i < n; i++) {
				byte c = buf[i];
				if (c >= 0x20 && c <= 0x7E) txt[tp++] = (char)c;
				else ascii_ok = false;
				hex[hp++] = "0123456789ABCDEF"[(c >> 4) & 0xF];
				hex[hp++] = "0123456789ABCDEF"[c & 0xF];
				hex[hp++] = ' ';
			}
			txt[tp] = 0;
			hex[hp] = 0;
			if (ascii_ok) XART1.OutFormat("[CDC] RX %uB: %s\r\n", (unsigned)n, txt);
			else XART1.OutFormat("[CDC] RX %uB hex: %s| %s\r\n", (unsigned)n, hex, txt);
			// 原样回显给 PC（Bulk IN）：队列满则丢弃并计 tx_dropped，绝不阻塞主循环。
			usb_cdc.Transmit(buf, (uint16)n);
		}

		// ---- 数据流统计（有收发或出现问题时才打印一行，空闲时安静） ----
		static uint32 rx_stats_ms = 0, last_rx_pk = 0, last_rx_by = 0, last_rx_ovf = 0;
		static uint32 last_tx_by = 0, last_tx_drop = 0;
		if (now_ms - rx_stats_ms >= 1000) {
			uint32 pk = usb_cdc.rx_packets, by = usb_cdc.rx_bytes, ovf = usb_cdc.rx_overflow;
			uint32 txb = usb_cdc.tx_bytes, txd = usb_cdc.tx_dropped;
			rx_stats_ms = now_ms;
			if ((pk != last_rx_pk) || (ovf != last_rx_ovf) || (txd != last_tx_drop)) {
				XART1.OutFormat("[CDC] 1s: rx pkt=+%u byte=+%u ring=%u ovf=+%u | tx byte=+%u drop=+%u\r\n",
					(unsigned)(pk - last_rx_pk), (unsigned)(by - last_rx_by),
					(unsigned)usb_cdc.RxCount(), (unsigned)(ovf - last_rx_ovf),
					(unsigned)(txb - last_tx_by), (unsigned)(txd - last_tx_drop));
			}
			last_rx_pk = pk;
			last_rx_by = by;
			last_rx_ovf = ovf;
			last_tx_by = txb;
			last_tx_drop = txd;
		}

#if _CDC_SETUP_TRACE
		// 枚举跟踪（联调用）：打印每次 SETUP
		static uint32 seen_setup = 0;
		if (g_last_req.seq != seen_setup) {
			seen_setup = g_last_req.seq;
			XART1.OutFormat("[Setup] #%u Type=0x%02X Req=0x%02X Val=0x%04X Idx=0x%04X Len=%u\r\n",
				(unsigned)seen_setup, (unsigned)g_last_req.req_type, (unsigned)g_last_req.req,
				(unsigned)g_last_req.val, (unsigned)g_last_req.idx, (unsigned)g_last_req.len);
		}
#endif

		// ---- 按键事件（EXTI，30ms 消抖，无阻塞等待） ----
		static uint32 seen_up = 0, seen_dn = 0, seen_lf = 0, seen_rt = 0;
		static uint32 ms_up = 0, ms_dn = 0, ms_lf = 0, ms_rt = 0;

		if (seen_up != key_ev_up) {// Up：软重连（模拟拔插）
			seen_up = key_ev_up;
			if (now_ms - ms_up > 200) {
				ms_up = now_ms;
				XART1.OutFormat("[USB] Pulsing Soft Reconnect (500ms DCTL.SDIS=1)...\r\n");
				PCD2.Disconnect();
				SysDelay_ms(500);
				PCD2.Connect();
				XART1.OutFormat("[USB] Reconnected!\r\n");
			}
		}
		if (seen_dn != key_ev_dn) {// Down：状态快照
			seen_dn = key_ev_dn;
			if (now_ms - ms_dn > 30) {
				ms_dn = now_ms;
				PrintCDCStatus();
				XART1.OutFormat("[USB] ev: IRQ=%u Reset=%u Setup=%u Susp=%u Resume=%u\r\n",
					(unsigned)ev_irq_cnt, (unsigned)ev_reset_cnt, (unsigned)ev_setup_cnt,
					(unsigned)ev_susp_cnt, (unsigned)ev_resume_cnt);
			}
		}
		if (seen_lf != key_ev_lf) seen_lf = key_ev_lf;// Left ：保留（暂无动作）
		if (seen_rt != key_ev_rt) seen_rt = key_ev_rt;// Right：保留（暂无动作）

		// ---- 心跳 ----
		tick++;
		if (tick >= 250000) {
			tick = 0;
			LEDB.Toggle();
		}
	}
}
