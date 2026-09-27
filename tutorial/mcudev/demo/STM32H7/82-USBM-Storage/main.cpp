// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 82-USBM-Storage：USB 主机（Host）读 U 盘
 *
 * 阶段一（本文件当前范围）：主机控制器 bring-up + 设备枚举 + 信息打印
 *   1) OTG2_FS 切到主机模式（PA11/PA12，与从机 demo 同一实例、同一对引脚，照实验59）；
 *   2) 主机口 5V 供电：见 USBH_PWR_MODE 宏（默认 = 实验59 的 PCF8574 P3 位写）；
 *   3) 插入设备 → 端口复位 → 建传输桥(OTGHostDevice) → 枚举 → 打印 VID/PID/类/字符串；
 *   4) 拔出 → 报断开并丢弃传输桥，等下一次插入。
 *
 * 未做（后续阶段）：MSC 类驱动、BOT/SCSI 命令、FAT 挂载与文件读写。
 *
 * 键位（Up/Down/Left/Right = PA0/PH2/PC13/PH3，EXTI 中断）：
 *   Up    → 重新复位端口并重新枚举
 *   Down  → 打印主机状态快照（端口寄存器 / 通道寄存器 / 事件计数）
 *   Left / Right → 保留（暂无动作）
 *
 * 说明：本框架的主机栈目前不发 SET_ADDRESS，设备一直按地址 0 访问；
 *       单设备（一个 U 盘）场景可用，接 Hub / 多设备需要后续补。
 */

#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/IIC>
#include <cpp/Device/USB/HCD.hpp>
#include <cpp/Device/USB/OTGHostDevice.hpp>
#include <cpp/Device/USB/USBHost-MSC.hpp>
#include <c/format/filesys/FAT.h>
#include <c/data.h>
#include <c/mempool.h>
#include <c/consio.h>
#include <c/ustring.h>
#include <c/system/debug.h>


#define USBH_WRITE_TEST 0
#define USBH_FAT_WRITE_TEST 1

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

// ustring 的 String 依赖这三个接口（本 demo 不用 String，但按其余 demo 的惯例补齐）
char* StrHeap(const char* valit_str) { return (char*)valit_str; }
char* StrHeapAppendChars(char* dest, char chr, size_t n) { return dest + n + chr; }
char* salc(size_t size) { return 0; }

// ---- 按键中断：ISR 只计数，动作留给主循环（不阻塞 USB） ----
static volatile uint32 key_ev_up = 0, key_ev_dn = 0, key_ev_lf = 0, key_ev_rt = 0;
void key_isr_up() { key_ev_up++; }
void key_isr_dn() { key_ev_dn++; }
void key_isr_lf() { key_ev_lf++; }
void key_isr_rt() { key_ev_rt++; }

// HPRT0：主机端口寄存器（OTG 基址 + USB_OTG_HOST_PORT_BASE = 0x440）
#define _USBH_HPRT0 (*(volatile uint32*)(_OTG2_FS_ADDR + USB_OTG_HOST_PORT_BASE))
// 主机通道 n 的寄存器（HCCHAR +0x00 / HCINT +0x08）
#define _USBH_HCREG(n, off) (*(volatile uint32*)(_OTG2_FS_ADDR + USB_OTG_HOST_CHANNEL_BASE + (n) * USB_OTG_HOST_CHANNEL_SIZE + (off)))

// ==================== 主机口 5V 供电 ====================
// 1 = PCF8574 位写（照实验59：HAL_HCD_MspInit 里 PCF8574_WriteBit(USB_PWR_IO,1)，即 P3）
// 2 = GPIO 直驱（PG13，与 81-USBS-* 两个从机 demo 的做法一致）
// 3 = 常供（板上 5V 一直有，固件不动任何东西）
#define USBH_PWR_MODE 1

// 枚举期寄存器跟踪（1 = 打印通道/全局寄存器现场，定位完可以关掉）
#define _USBH_ENUM_TRACE 0

#if USBH_PWR_MODE == 1
#define USBH_PCF_ADDR7   0x20// 7 位器件地址（实验59 的 PCF8574_ADDR 0x40 是左移后的写地址）
#define USBH_PCF_RDADDR  0x41// 0x20 << 1 | 1，进入接收模式
#define USBH_USB_PWR_BIT 3   // P3 = USB 电源（P0 蜂鸣器 / P2 DCMI / P6 RS485 / P7 以太网复位）

static void iic_delay() { for (volatile int i = 0; i < 500; i++) {} }

// PCF8574 挂在与 ES8388/24C02 同一条软 I2C 上（SDA=PH5, SCL=PH4）。
// 强制 push_pull：让 Send 写 SDA / ReadByte 读 SDA 靠临时切输入模式完成
// （与 20-LTDC-DMA2D-Touch 的 TouchIIC 同一处理）。
class PcfIIC : public uni::IIC_SOFT {
public:
	// IIC_SOFT 自己声明了 Send(byte*, stduint, bool)，会把基类的 Send(byte, bool)
	// 整个名字遮住；这里把它拉回来，才能按“写一个字节”调用。
	using uni::IIC_t::Send;

	PcfIIC(uni::GPIN& sda, uni::GPIN& scl) : uni::IIC_SOFT(sda, scl, false) {
		push_pull = true;
		setMode();
	}
};

static byte _pcf_wire_buf[byteof(PcfIIC)];
static PcfIIC* pcf_iic = nullptr;

// 读 8 位 IO（照 PCF8574_ReadOneByte）。IIC_SOFT::SendStart 只会发写地址，
// 所以这里用 addr==0 的裸 START，再手发读地址。
static bool PcfReadByte(byte& out) {
	pcf_iic->SendStart(0);
	pcf_iic->Send(USBH_PCF_RDADDR, true);
	if (pcf_iic->getError() != ERR_IIC_NONE) return false;
	out = pcf_iic->ReadByte(true, false);// 只读 1 字节 -> 末尾回 NACK
	pcf_iic->SendStop();
	return true;
}

// 写 8 位 IO（照 PCF8574_WriteOneByte）
static bool PcfWriteByte(byte val) {
	pcf_iic->SendStart(USBH_PCF_ADDR7);// 框架内部左移并补写位
	pcf_iic->Send(val, true);
	pcf_iic->SendStop();
	SysDelay_ms(10);// 照参考驱动的写周期等待
	return (pcf_iic->getError() == ERR_IIC_NONE);
}

// 照 PCF8574_WriteBit：先读回再只改一位，免得踩到蜂鸣器/DCMI/RS485/以太网复位那几个位
static bool PcfWriteBit(byte bit_index, bool val) {
	byte cur = 0xFF;// 读失败时按 PCF8574 上电默认（全高）继续
	bool read_ok = PcfReadByte(cur);
	if (val) cur |= (byte)(1u << bit_index);
	else cur &= (byte)~(1u << bit_index);
	bool write_ok = PcfWriteByte(cur);
	return read_ok && write_ok;
}
#endif

// AKA USBH_LL_DriverVBUS（实验59 在 USE_USB_FS 下这里是空实现，
// 真正上电在 HAL_HCD_MspInit 里做；供电方式由 USBH_PWR_MODE 选）
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
static OTGHostDevice* usb_otg_host = nullptr;// 传输桥（插上设备时才构造）
alignas(8) static byte _host_bridge_buf[byteof(OTGHostDevice)];// 含引用/指针成员，按 8 字节对齐

static volatile uint32 ev_conn_cnt = 0;
static volatile uint32 ev_disc_cnt = 0;
static volatile uint32 ev_sof_cnt = 0;
static volatile uint32 ev_urb_cnt = 0;
static volatile uint32 ev_irq_cnt = 0;
// ISR 出口处见过的 GINTSTS 位或起来：框架没清掉的位会一直出现，据此能看出是谁在反复打断
static volatile uint32 ev_gintsts_seen = 0;

// 主机端口的连接状态：HPRT0.PCSTS
static bool UsbHostPortConnected() {
	return (_USBH_HPRT0 & (1u << USB_OTG_HPRT_PCSTS_Pos)) != 0;
}

void on_hcd_connect() { ev_conn_cnt++; }
void on_hcd_disconnect() { ev_disc_cnt++; }
void on_hcd_sof() { ev_sof_cnt++; }

// 最近 8 次 ch0 的完成记录（一次控制传输 = SETUP/DATA/STATUS 三条，留 8 条才看得到中间那段）。
// 用途：枚举卡住时判断数据到底有没有进缓冲区，以及是哪一段的问题。
//   count = xfer_count（本段实际收/发字节）；buf = xfer_buff（RX 处理器每收一包会把它前进 bcnt）
//   tgl   = toggle_in（RX 处理器收到包并重新激活通道时会翻转它）；chs = hc[].state
static volatile uint32 ev_urb_state_hist[8] = {};
static volatile uint32 ev_urb_count_hist[8] = {};
static volatile uint32 ev_urb_buff_hist[8] = {};
static volatile uint32 ev_urb_tgl_hist[8] = {};
static volatile uint32 ev_urb_chs_hist[8] = {};
static volatile byte ev_urb_head_hist[8][8] = {};

// RX FIFO 观察：ISR 出口处若 RXFLVL 还挂着，就用只读的 GRXSTSR 偷看 FIFO 顶部条目。
//   PKTSTS: 2=收到 IN 数据包, 3=IN 传输完成, 5=翻转错, 7=通道已停
//   若 rx_data 计数为 0、rx_bcnt 也为 0，说明设备根本没回过数据（响应了 0 长度包）。
static volatile uint32 ev_rx_left_cnt = 0;   // 出口时 RXFLVL 仍悬挂的次数
static volatile uint32 ev_rx_data_cnt = 0;   // 见到 PKTSTS=2（IN 数据）的次数
static volatile uint32 ev_rx_bcnt_max = 0;   // 见到的 IN 数据包最大字节数
static volatile uint32 ev_rx_last_pktsts = 0xFFFFFFFF;

// URB 完成回调（ISR 上下文）：HCD 是先更新 hc[].urb_state 再回调，所以这里能直接查状态
void on_hcd_urb(void* ch_num, ...) {
	byte ch = (byte)(stduint)ch_num;
	URBState st = HCD2.getURBState(ch);
	if (ch == 0) {
		uint32 i = (uint32)ev_urb_cnt & 7;
		ev_urb_state_hist[i] = (uint32)st;
		ev_urb_count_hist[i] = (uint32)HCD2.getXferCount(ch);
		ev_urb_buff_hist[i] = (uint32)(stduint)HCD2.hc[0].xfer_buff;
		ev_urb_tgl_hist[i] = (uint32)HCD2.hc[0].toggle_in;
		ev_urb_chs_hist[i] = (uint32)HCD2.hc[0].state;
		if (usb_otg_host) {
			byte* b = usb_otg_host->Buffer();
			for (int k = 0; k < 8; k++) ev_urb_head_hist[i][k] = b ? b[k] : (byte)0xFF;
		}
	}
	ev_urb_cnt++;
	// 记录完再交给桥，免得它提交下一段时把 xfer_count 清掉
	if (usb_otg_host) usb_otg_host->OnChannelURBCompleted(ch, st);
}

void on_usb_fs_interrupt() {
	ev_irq_cnt++;
	uint32 gs = (uint32)HCD2.GlobalReg(OTGGlobalReg::GINTSTS);
	ev_gintsts_seen |= gs;
	if (gs & _IMM1S(USB_OTG_GINTSTS_RXFLVL_Pos)) {
		ev_rx_left_cnt++;
		uint32 v = (uint32)HCD2.GlobalReg(OTGGlobalReg::GRXSTSR);// 只读，不弹 FIFO
		uint32 pktsts = (v & USB_OTG_GRXSTSP_PKTSTS) >> USB_OTG_GRXSTSP_PKTSTS_Pos;
		uint32 bcnt = (v & USB_OTG_GRXSTSP_BCNT) >> USB_OTG_GRXSTSP_BCNT_Pos;
		ev_rx_last_pktsts = pktsts;
		if (pktsts == 2) {// IN data packet received
			ev_rx_data_cnt++;
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
	HCD2.ResetPort();// AKA USB_ResetPort：内部含 100ms 的 PRST 拉高
	SysDelay_ms(20);// 复位放开后留一点稳定时间
	byte spd = (byte)HCD2.getCurrentSpeed();// HPRT0.PSPD: 0=HS, 1=FS, 2=LS
	UsbHostDropBridge();
	usb_otg_host = new (_host_bridge_buf) OTGHostDevice(HCD2, 0, spd);
	enum_deadline = (uint32)SysTick::getTick() + 3000;
#if _USBH_ENUM_TRACE
	XART1.OutFormat("[USBH] CH0 after init: HCCHAR=0x%08X HCINT=0x%08X HCINTMSK=0x%08X\r\n",
		(unsigned)_USBH_HCREG(0, 0x00), (unsigned)_USBH_HCREG(0, 0x08),
		(unsigned)_USBH_HCREG(0, 0x0C));
#endif
	Error err = usb_otg_host->StartInitialize();
#if _USBH_ENUM_TRACE
	// Name()/Line() 是产出这个 Error 的源码位置：能直接看出 ControlIn 有没有真的走到 SubmitRequest
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

// 主机状态快照（Down 键 / 枚举超时时用）
static void PrintHostStatus() {
	uint32 hprt0 = _USBH_HPRT0;
	XART1.OutFormat("[USBH] HPRT0=0x%08X PCSTS=%u PENA=%u PPWR=%u PRST=%u PLSTS=%u PSPD=%u\r\n",
		(unsigned)hprt0,
		(unsigned)((hprt0 >> USB_OTG_HPRT_PCSTS_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PENA_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PPWR_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PRST_Pos) & 1),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PLSTS_Pos) & 3),
		(unsigned)((hprt0 >> USB_OTG_HPRT_PSPD_Pos) & 3));
	XART1.OutFormat("[USBH] CH0 HCCHAR=0x%08X HCINT=0x%08X urb=%u chstate=%u\r\n",
		(unsigned)_USBH_HCREG(0, 0x00), (unsigned)_USBH_HCREG(0, 0x08),
		(unsigned)HCD2.getURBState(0), (unsigned)HCD2.getHostChannelState(0));
	XART1.OutFormat("[USBH] ev: IRQ=%u Conn=%u Disc=%u SOF=%u URB=%u | bridge=%u enumerated=%u\r\n",
		(unsigned)ev_irq_cnt, (unsigned)ev_conn_cnt, (unsigned)ev_disc_cnt,
		(unsigned)ev_sof_cnt, (unsigned)ev_urb_cnt,
		(unsigned)(usb_otg_host ? 1 : 0),
		(unsigned)((usb_otg_host && usb_otg_host->IsEnumerated()) ? 1 : 0));
	// 每次控制段完成的现场（按 ch0 顺序）：
	//   SETUP   段 count=0（OUT，没有接收）
	//   DATA IN 段 count=描述符长度、head 应以 12 01 开头（0x12=18，0x01=设备描述符）
	//   STATUS  段 count=0
	uint32 n = (ev_urb_cnt < 8) ? (uint32)ev_urb_cnt : 8;
	for (uint32 i = 0; i < n; i++) {
		XART1.OutFormat("[USBH] urb[%u] st=%u chs=%u count=%u tgl=%u buf=%08X head=%02X %02X %02X %02X\r\n",
			(unsigned)i, (unsigned)ev_urb_state_hist[i], (unsigned)ev_urb_chs_hist[i],
			(unsigned)ev_urb_count_hist[i], (unsigned)ev_urb_tgl_hist[i],
			(unsigned)ev_urb_buff_hist[i],
			(unsigned)ev_urb_head_hist[i][0], (unsigned)ev_urb_head_hist[i][1],
			(unsigned)ev_urb_head_hist[i][2], (unsigned)ev_urb_head_hist[i][3]);
	}
	XART1.OutFormat("[USBH] rx: left=%u data=%u bcnt_max=%u last_pktsts=%u (2=IN data 3=IN done 7=halted)\r\n",
		(unsigned)ev_rx_left_cnt, (unsigned)ev_rx_data_cnt,
		(unsigned)ev_rx_bcnt_max, (unsigned)ev_rx_last_pktsts);
	uint32 gs = (uint32)HCD2.GlobalReg(OTGGlobalReg::GINTSTS);
	XART1.OutFormat("[USBH] GINTSTS=0x%08X (CMOD=%u SOF=%u RXFLVL=%u HPRTINT=%u HCINT=%u DISCINT=%u WKUP=%u) seen=0x%08X\r\n",
		(unsigned)gs, (unsigned)(gs & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_SOF_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_RXFLVL_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_HPRTINT_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_HCINT_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_DISCINT_Pos) & 1),
		(unsigned)((gs >> USB_OTG_GINTSTS_WKUINT_Pos) & 1),
		(unsigned)ev_gintsts_seen);
	XART1.OutFormat("[USBH] GINTMSK=0x%08X GAHBCFG=0x%08X GUSBCFG=0x%08X HAINT=0x%08X HAINTMSK=0x%08X\r\n",
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GINTMSK),
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GAHBCFG),
		(unsigned)HCD2.GlobalReg(OTGGlobalReg::GUSBCFG),
		(unsigned)HCD2.HostReg(OTGHostReg::HAINT),
		(unsigned)HCD2.HostReg(OTGHostReg::HAINTMSK));
	XART1.OutFormat("[USBH] HCFG=0x%08X HFIR=%u | CH0 HCINTMSK=0x%08X HCTSIZ=0x%08X mp=%u\r\n",
		(unsigned)HCD2.HostReg(OTGHostReg::HCFG), (unsigned)HCD2.HostReg(OTGHostReg::HFIR),
		(unsigned)_USBH_HCREG(0, 0x0C), (unsigned)_USBH_HCREG(0, 0x10),
		(unsigned)HCD2.hc[0].max_packet);// mp = EP0 实际包长（引导后应为设备的 bMaxPacketSize0）
}

// ==================== 阶段二：MSC（U 盘） ====================
// 类驱动由枚举时的 NewClassDriver 创建（interface class 0x08/0x06/0x50），
// 它在端点配置完成后自己跑 GET_MAX_LUN → INQUIRY → READ CAPACITY(10)。
static USBHost_MSC* usb_msc_host = nullptr;
static bool msc_reported = false;
static bool msc_read_started = false;
static bool msc_read_reported = false;
static bool msc_bound_reported = false;
static bool msc_stuck_reported = false;
static uint32 msc_bound_ms = 0;
alignas(4) static byte _msc_block[512];

static void PrintMscInfo(USBHost_MSC& msc) {
	XART1.OutFormat("[MSC] vendor=\"%s\" product=\"%s\" rev=\"%s\" maxLUN=%u\r\n",
		msc.Vendor(), msc.Product(), msc.Revision(), (unsigned)msc.MaxLun());
	const uint32 blocks = msc.BlockCount();
	const uint32 bsize = msc.BlockSize();
	XART1.OutFormat("[MSC] %u blocks x %u B = %u MB\r\n",
		(unsigned)blocks, (unsigned)bsize,
		(unsigned)(((uint64)blocks * bsize) >> 20));// /1MB
}

// ==================== 阶段三：把 U 盘当块设备挂 FAT ====================
// MSC 事务是异步的（提交后靠 URB 中断推进），而 FAT 层要求阻塞式返回，
// 所以适配器里提交之后自旋等待（照实验59 的 USBH_MSC_RdWrProcess busy-wait）。
static bool UsbMscWait(uint32 timeout_ms) {
	const uint32 t0 = (uint32)SysTick::getTick();
	while (usb_msc_host && usb_msc_host->IsBusy()) {
		if ((uint32)SysTick::getTick() - t0 > timeout_ms) {
			usb_msc_host->AbortCommand();
			return false;
		}
	}
	return usb_msc_host && usb_msc_host->LastResult() == USBHost_MSC::Result::kOk;
}

// AKA 一颗 512 字节块的 U 盘：StorageTrait 适配层（照 SDCard1 / SpiDisk 的写法）
class UsbMscDisk_t : public uni::StorageTrait {
public:
	// 单次 bulk 传输的上限：HCD 的 PKTCNT 最多 256 包（256*64=16KB）且提交长度是
	// uint16，取 16 扇区（8KB）留足余量，超过就分块。
	static const stduint kMaxChunk = 16;

	explicit UsbMscDisk_t(USBHost_MSC& msc, stduint unit = 512) : msc_(msc) {
		Block_Size = unit;
		readable = true;
		writable = true;
		Block_buffer = cache_;
	}

	using BlockTrait::Read;
	using BlockTrait::Write;

	bool Read(stduint BlockIden, void* Dest, stduint Times = 1) override {
		if (!msc_.IsReady() || !Dest || Times == 0) return false;
		if (BlockIden + Times > getUnits()) return false;// AKA SpiDisk_t: bounds first
		byte* dst = static_cast<byte*>(Dest);
		stduint lba = BlockIden;
		stduint left = Times;
		while (left) {
			const uint16 chunk = (left > kMaxChunk) ? (uint16)kMaxChunk : (uint16)left;
			if (msc_.ReadBlocks((uint32)lba, chunk, dst)) return false;
			if (!UsbMscWait(5000)) return false;
			lba += chunk;
			dst += (stduint)chunk * Block_Size;
			left -= chunk;
		}
		return true;
	}

	bool Write(stduint BlockIden, const void* Sors, stduint Times = 1) override {
		if (!msc_.IsReady() || !Sors || Times == 0) return false;
		if (BlockIden + Times > getUnits()) return false;// AKA SpiDisk_t: bounds first
		const byte* src = static_cast<const byte*>(Sors);
		stduint lba = BlockIden;
		stduint left = Times;
		while (left) {
			const uint16 chunk = (left > kMaxChunk) ? (uint16)kMaxChunk : (uint16)left;
			if (msc_.WriteBlocks((uint32)lba, chunk, src)) return false;
			if (!UsbMscWait(5000)) return false;
			lba += chunk;
			src += (stduint)chunk * Block_Size;
			left -= chunk;
		}
		return true;
	}

	stduint getUnits() override { return msc_.BlockCount(); }

	// 分区视图：U 盘是 MBR 分区盘（LBA0 是分区表，不是 FAT 引导扇区），
	// 照框架 StorageTrait 的设计给出切片：dev 0 = 整盘，dev 1..4 = 主分区。
	uni::PartitionSlice getSlice(stduint dev) override {
		uni::PartitionSlice res{};
		if (dev == 0) {
			res.address = 0;
			res.length = getUnits();
			res.sys_id = 0;
			return res;
		}
		if (!mbr_loaded_) {
			mbr_loaded_ = true;
			if (Read(0, mbr_sector_, 1)) {
				const PartitionTableX86* pt = reinterpret_cast<const PartitionTableX86*>(
					mbr_sector_ + MBR_PARTITION_TABLE_OFFSET);
				for (int i = 0; i < 4; i++) {
					parts_[i].address = pt[i].lba_start;
					parts_[i].length = pt[i].lba_count;
					parts_[i].sys_id = pt[i].type;
				}
			}
		}
		if (dev >= 1 && dev <= 4) res = parts_[dev - 1];
		return res;
	}

	// 字节访问：带一个块的缓存（Block_buffer 指向它）
	int operator[](uint64 bytid) override {
		const stduint block = (stduint)(bytid / Block_Size);
		const stduint offset = (stduint)(bytid % Block_Size);
		if (block >= getUnits()) return -1;
		if (!cache_valid_ || cached_block_ != block) {
			if (!Read(block, cache_, 1)) return -1;
			cached_block_ = block;
			cache_valid_ = true;
		}
		return cache_[offset];
	}

private:
	USBHost_MSC& msc_;
	byte cache_[512] = {};
	stduint cached_block_ = 0;
	bool cache_valid_ = false;
	byte mbr_sector_[512] = {};
	uni::PartitionSlice parts_[4] = {};
	bool mbr_loaded_ = false;
};

static UsbMscDisk_t* usb_disk = nullptr;
static uni::DiscPartition* usb_part = nullptr;
static FilesysFAT* usb_fs = nullptr;
static bool fat_mounted_reported = false;
static stduint fat_entries = 0;
static char fat_first_file[64] = {};
// 阶段四：热插拔 —— 三个对象放在静态存储里（不走堆，重插不会累积泄漏），
// 拔出时按依赖逆序析构：FAT → 分区视图 → 块设备（它们都引用桥里的 MSC 驱动）。
alignas(8) static byte _disk_store[sizeof(UsbMscDisk_t)];
alignas(8) static byte _part_store[sizeof(uni::DiscPartition)];
alignas(8) static byte _fs_store[sizeof(FilesysFAT)];

alignas(4) static byte _fat_sector_buf[512];
alignas(4) static byte _fat_table_buf[512];
alignas(4) static byte _fat_read_buf[32];

// 列目录回调：FAT 按 (is_dir, name) 两参调用（照 92-AudioPlay 的 on_wav_seg）
static void OnFatEntry(void* is_dir, ...) {
	va_list ap;
	va_start(ap, is_dir);
	const char* nm = va_arg(ap, const char*);
	va_end(ap);
	if (!nm) return;
	XART1.OutFormat("[FAT] %s%s\r\n", is_dir ? "<DIR> " : "", nm);
	if (!is_dir && fat_first_file[0] == 0) {
		// 记住第一个文件（带前导 '/'），供随后读一个文件用
		const stduint len = StrLength(nm);
		if (len && len < sizeof(fat_first_file) - 2) {
			fat_first_file[0] = '/';
			for (stduint k = 0; k < len; k++) fat_first_file[k + 1] = nm[k];
			fat_first_file[len + 1] = 0;
		}
	}
	fat_entries++;
}

// 根目录列举：照 92-AudioPlay —— search("/") 拿根目录句柄，再 enumer 逐项回调
static void FatListRoot(FilesysFAT& fs) {
	FAT_FileHandle handle{};
	FilesysSearchArgs args{};
	args.handle_buffer = &handle;
	fat_entries = 0;
	if (!fs.search("/", &args)) {
		XART1.OutFormat("[FAT] open root failed\r\n");
		return;
	}
	fs.enumer(&handle, OnFatEntry);
	XART1.OutFormat("[FAT] root entries=%u\r\n", (unsigned)fat_entries);
}

// 阶段四：写路径验证（宏开关，默认关）





static void FatWriteMscState(const char* tag) {
	if (!usb_msc_host) {
		XART1.OutFormat("[FAT] %s: msc gone\r\n", tag);
		return;
	}
	XART1.OutFormat("[FAT] %s: msc cmds=%u done=%u halt=%u result=%s stage=%s busy=%u\r\n",
		tag,
		(unsigned)usb_msc_host->CommandCount(), (unsigned)usb_msc_host->CompletionCount(),
		(unsigned)usb_msc_host->HaltRecoveries(),
		usb_msc_host->ResultName(), usb_msc_host->StageName(),
		(unsigned)(usb_msc_host->IsBusy() ? 1 : 0));
}

static void FatWriteTest(FilesysFAT& fs) {
	fs.allow_allocate = true;// create/writfl 需要分配簇
	fs.remove("/UNISYM.TXT");
	FatWriteMscState("before create");
	if (!fs.create("/UNISYM.TXT", 0, nullptr)) {
		XART1.OutFormat("[FAT] create FAIL: error=%u\r\n", (unsigned)fs.error_number);
		FatWriteMscState("after create");
		return;
	}
	FatWriteMscState("after create");
	FAT_FileHandle handle{};
	FilesysSearchArgs args{};
	args.handle_buffer = &handle;
	void* fh = fs.search("/UNISYM.TXT", &args);
	if (!fh) {
		XART1.OutFormat("[FAT] search new file FAIL\r\n");
		return;
	}
	static const char msg[] = "Hello, unisym FAT!";
	const stduint wlen = fs.writfl(fh, Slice{ 0, sizeof(msg) - 1 }, (const byte*)msg);
	FatWriteMscState("after writfl");
	byte rd[32] = {};
	const stduint rlen = fs.readfl(fh, Slice{ 0, sizeof(msg) - 1 }, rd);
	rd[(rlen < sizeof(rd)) ? rlen : (sizeof(rd) - 1)] = 0;
	XART1.OutFormat("[FAT] write test: writfl=%u readfl=%u -> \"%s\"\r\n",
		(unsigned)wlen, (unsigned)rlen, (const char*)rd);
}

static void MscWriteTest(UsbMscDisk_t& disk, uni::DiscPartition& part) {
	const uni::PartitionSlice ps = part.getSlice();
	const stduint part_end = ps.address + ps.length;
	const stduint dev_blocks = usb_msc_host ? usb_msc_host->BlockCount() : 0;
	XART1.OutFormat("[WR] partition ends at lba=%u, device has %u blocks\r\n",
		(unsigned)part_end, (unsigned)dev_blocks);
	if (part_end + 1 >= dev_blocks) {
		XART1.OutFormat("[WR] no slack sector outside the partition, skipped\r\n");
		return;
	}
	const stduint lba = part_end;// 分区之后第一个扇区（设备空闲余量）
	static byte backup[512];
	static byte pattern[512];
	static byte rd[512];
	if (!disk.Read(lba, backup, 1)) {
		XART1.OutFormat("[WR] backup read FAIL (lba=%u)\r\n", (unsigned)lba);
		return;
	}
	for (stduint i = 0; i < sizeof(pattern); i++) pattern[i] = (byte)(i ^ 0x5A);
	if (!disk.Write(lba, pattern, 1)) {
		XART1.OutFormat("[WR] WRITE(10) FAIL (lba=%u)\r\n", (unsigned)lba);
		return;
	}
	if (!disk.Read(lba, rd, 1)) {
		XART1.OutFormat("[WR] readback FAIL (lba=%u)\r\n", (unsigned)lba);
		return;
	}
	bool ok = true;
	for (stduint i = 0; i < sizeof(pattern); i++) {
		if (rd[i] != pattern[i]) { ok = false; break; }
	}
	XART1.OutFormat("[WR] write+readback at lba=%u : %s (head=%02X %02X %02X %02X)\r\n",
		(unsigned)lba, ok ? "OK" : "MISMATCH",
		(unsigned)rd[0], (unsigned)rd[1], (unsigned)rd[2], (unsigned)rd[3]);
	// 恢复原内容（空闲扇区也还原，保持盘面原样）
	if (disk.Write(lba, backup, 1)) XART1.OutFormat("[WR] original sector restored\r\n");
	else XART1.OutFormat("[WR] restore FAIL\r\n");
}

// 读第一个文件的开头（只读验证 FAT→块→USB 的读链路）
static void FatReadFirstFile(FilesysFAT& fs) {
	if (fat_first_file[0] == 0) {
		XART1.OutFormat("[FAT] no file to read\r\n");
		return;
	}
	FAT_FileHandle handle{};
	FilesysSearchArgs args{};
	args.handle_buffer = &handle;
	void* fh = fs.search(fat_first_file, &args);
	if (!fh) {
		XART1.OutFormat("[FAT] search %s failed\r\n", fat_first_file);
		return;
	}
	const stduint n = fs.readfl(fh, Slice{ 0, sizeof(_fat_read_buf) }, _fat_read_buf);
	XART1.OutFormat("[FAT] read %s -> %u B: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
		fat_first_file, (unsigned)n,
		(unsigned)_fat_read_buf[0], (unsigned)_fat_read_buf[1],
		(unsigned)_fat_read_buf[2], (unsigned)_fat_read_buf[3],
		(unsigned)_fat_read_buf[4], (unsigned)_fat_read_buf[5],
		(unsigned)_fat_read_buf[6], (unsigned)_fat_read_buf[7]);
}

// 枚举完成后打印设备信息
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
	XART1.OutFormat("[USBH] endpoint configs=%d (0 = 还没有类驱动接管)\r\n",
		(int)dev.NumEndpointConfigs());
	XART1.OutFormat("[USBH] Enumeration OK\r\n");
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
	GPIOG.enClock(true);
	GPIOH.enClock(true);

	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;

	// ---- 按键：EXTI 中断模式（优先级 3 = 低于 USB 的 1） ----
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
	XART1.OutFormat("  UNISYM STM32H743 82-USBM-Storage \r\n");
	XART1.OutFormat("  USB Host (OTG2_FS) bring-up + enumeration\r\n");
	XART1.OutFormat("====================================================\r\n");

	// ==================== 主机口供电 ====================
	XART1.OutFormat("Initializing USB Host...\r\n");
#if USBH_PWR_MODE == 1
	pcf_iic = new (_pcf_wire_buf) PcfIIC(GPIOH[5], GPIOH[4]);// SDA=PH5, SCL=PH4
	pcf_iic->func_delay = iic_delay;
#endif
	UsbHostDriverVBUS(true);
	SysDelay_ms(500);// 照实验59 USBH_LL_DriverVBUS 里的 delay_ms(500)，等 U 盘上电稳定

	// ==================== USB 时钟与引脚（照实验59 HAL_HCD_MspInit） ====================
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

	// ==================== HCD2：主机模式 ====================
	HCD2.base = _OTG2_FS_ADDR;
	HCD2.speed = 3;              // 控制器级：HCD_SPEED_FULL，内嵌 FS PHY 48MHz
	HCD2.host_channels = 8;
	HCD2.phy_itface = 2;         // 内嵌 PHY
	HCD2.dma_enable = false;
	HCD2.use_external_vbus = false;
	if (!HCD2.setMode()) erro("HCD Init Error");// 内含 OTG 时钟使能 + 强制主机模式

	HCD2.ConnectHandler = on_hcd_connect;
	HCD2.DisconnectHandler = on_hcd_disconnect;
	HCD2.SOFHandler = on_hcd_sof;
	HCD2.NotifyURBChangeHandler = on_hcd_urb;
	HCD2.setInterrupt(on_usb_fs_interrupt);
	HCD2.setInterruptPriority(1, 0);
	HCD2.enInterrupt();

	HCD2.Start();// AKA HAL_HCD_Start：内含 HPRT0.PPWR 驱动 VBUS

	XART1.OutFormat("[USBH] Host controller started (HPRT0=0x%08X)\r\n", (unsigned)_USBH_HPRT0);
	XART1.OutFormat("[USBH] Plug a USB device (U-disk) into the HOST port.\r\n");

	uint32 tick = 0;
	uint32 seen_conn = 0, seen_disc = 0;
	uint32 seen_up = 0, seen_dn = 0, seen_lf = 0, seen_rt = 0;
	uint32 ms_up = 0, ms_dn = 0;
	uint32 enum_deadline = 0;
	bool enum_reported = false;

	// ==================== 主循环 ====================
	while (true) {
		uint32 now_ms = (uint32)SysTick::getTick();

		// ---- 拔出：先处理，保证后面的插入判断看到的是最新状态 ----
		if (ev_disc_cnt != seen_disc) {
			seen_disc = ev_disc_cnt;
			if (usb_otg_host) {
				// 阶段四：先按依赖逆序拆掉 FAT 层（它们都引用桥里的 MSC 驱动），
				// 再丢弃传输桥——顺序反了就是 use-after-free。
				if (usb_fs) { usb_fs->~FilesysFAT(); usb_fs = nullptr; }
				if (usb_part) { usb_part->~DiscPartition(); usb_part = nullptr; }
				if (usb_disk) { usb_disk->~UsbMscDisk_t(); usb_disk = nullptr; }
				usb_msc_host = nullptr;// 类驱动随桥一起销毁（~USBHostDevice 里 delete）
				UsbHostDropBridge();
				msc_reported = false;
				msc_read_started = false;
				msc_read_reported = false;
				msc_bound_reported = false;
				msc_stuck_reported = false;
				fat_mounted_reported = false;
				fat_first_file[0] = 0;
				enum_reported = false;
				XART1.OutFormat("[USBH] Device detached (event #%u)\r\n", (unsigned)seen_disc);
			}
		}

		// ---- 插入：端口复位 -> 建传输桥 -> 开始枚举 ----
		// ConnectHandler 在 PCDET / PENCHNG 上各回调一次，所以这里以“桥是否已建 + HPRT0.PCSTS”
		// 为准，避免把同一次插入的第二次回调当成新的插入。
		if (ev_conn_cnt != seen_conn) {
			seen_conn = ev_conn_cnt;
			if (!usb_otg_host && UsbHostPortConnected()) {
				XART1.OutFormat("[USBH] Device attached (event #%u)\r\n", (unsigned)seen_conn);
				enum_reported = false;
				UsbHostStartEnumeration(enum_deadline);// 端口刚查过，这里必定成功
			}
		}

		// ---- 枚举结果 ----
		if (usb_otg_host && !enum_reported && usb_otg_host->IsEnumerated()) {
			enum_reported = true;
			PrintDeviceInfo(*usb_otg_host);
		}

		// ---- 阶段二：MSC 类驱动就绪后打印 INQUIRY / 容量，并读一次 LBA0 ----
		if (usb_otg_host && usb_otg_host->IsEnumerated()) {
			if (!usb_msc_host) {// 从端点槽位反查 NewClassDriver 建出来的类驱动
				for (int i = 0; i < usb_otg_host->NumEndpointConfigs(); i++) {
					ClassDriver* d = usb_otg_host->ClassDriverOf(
						usb_otg_host->EndpointConfigs()[i].ep_id.Number());
					if (d) {
						usb_msc_host = static_cast<USBHost_MSC*>(d);
						break;
					}
				}
				if (usb_msc_host) msc_bound_ms = now_ms;
			}
			if (usb_msc_host && !msc_bound_reported) {
				msc_bound_reported = true;
				XART1.OutFormat("[MSC] driver bound: phase=%s stage=%s cmds=%u done=%u\r\n",
					usb_msc_host->PhaseName(), usb_msc_host->StageName(),
					(unsigned)usb_msc_host->CommandCount(),
					(unsigned)usb_msc_host->CompletionCount());
			}
			if (usb_msc_host && !msc_reported && usb_msc_host->BringUpDone()) {
				msc_reported = true;
				if (usb_msc_host->IsReady()) PrintMscInfo(*usb_msc_host);
				else XART1.OutFormat("[MSC] bring-up failed: %s (phase=%s stage=%s cmds=%u done=%u)\r\n",
					usb_msc_host->ResultName(), usb_msc_host->PhaseName(),
					usb_msc_host->StageName(), (unsigned)usb_msc_host->CommandCount(),
					(unsigned)usb_msc_host->CompletionCount());
			}
			// 卡住检测：2 秒还没走完 bring-up，把停在哪一段打出来（只打一次）
			if (usb_msc_host && !msc_reported && !msc_stuck_reported &&
				(now_ms - msc_bound_ms) > 2000) {
				msc_stuck_reported = true;
				XART1.OutFormat("[MSC] stuck 2s: phase=%s stage=%s busy=%u cmds=%u done=%u\r\n",
					usb_msc_host->PhaseName(), usb_msc_host->StageName(),
					(unsigned)(usb_msc_host->IsBusy() ? 1 : 0),
					(unsigned)usb_msc_host->CommandCount(),
					(unsigned)usb_msc_host->CompletionCount());
			}
			if (usb_msc_host && usb_msc_host->IsReady() && !msc_read_started) {
				msc_read_started = true;
				Error err = usb_msc_host->ReadBlocks(0, 1, _msc_block);
				if (err) XART1.OutFormat("[MSC] READ(10) submit failed: %s\r\n", err.Name());
				else XART1.OutFormat("[MSC] READ(10) LBA0 ...\r\n");
			}
			if (usb_msc_host && msc_read_started && !msc_read_reported && !usb_msc_host->IsBusy()) {
				msc_read_reported = true;
				if (usb_msc_host->LastResult() == USBHost_MSC::Result::kOk) {
					// 分区盘的第 0 扇区末尾应是 MBR 签名 55 AA
					const bool mbr = (_msc_block[510] == 0x55) && (_msc_block[511] == 0xAA);
					XART1.OutFormat("[MSC] LBA0 = %02X %02X %02X %02X ... %02X %02X | MBR 55AA %s\r\n",
						(unsigned)_msc_block[0], (unsigned)_msc_block[1],
						(unsigned)_msc_block[2], (unsigned)_msc_block[3],
						(unsigned)_msc_block[510], (unsigned)_msc_block[511],
						mbr ? "OK" : "not found (blank disk?)");
				}
				else XART1.OutFormat("[MSC] READ(10) failed: %s\r\n", usb_msc_host->ResultName());
			}

			// ---- 阶段三：U 盘作为块设备，挂 FAT 并列根目录 / 读一个文件（全程只读）----
			// 必须等 LBA0 那次读测试结束再挂载：MSC 一次只允许一条命令在飞，
			// 否则 loadfs 的读会被 kFull 顶回来。
			if (usb_msc_host && usb_msc_host->IsReady() && msc_read_reported &&
				!fat_mounted_reported) {
				fat_mounted_reported = true;
				usb_disk = new (_disk_store) UsbMscDisk_t(*usb_msc_host);
				XART1.OutFormat("[FAT] disk: %u blocks x %u B\r\n",
					(unsigned)usb_disk->getUnits(), (unsigned)usb_disk->Block_Size);
				// 打印分区表并选第一个有内容的主分区；没有分区表（超级软盘格式）就用整盘
				stduint use_dev = 0;
				for (stduint dev = 1; dev <= 4; dev++) {
					const uni::PartitionSlice ps = usb_disk->getSlice(dev);
					if (!ps.length) continue;
					XART1.OutFormat("[FAT] part%u: type=0x%02X lba=%u count=%u\r\n",
						(unsigned)dev, (unsigned)ps.sys_id,
						(unsigned)ps.address, (unsigned)ps.length);
					if (use_dev == 0) use_dev = dev;
				}
				XART1.OutFormat("[FAT] mounting %s\r\n",
					use_dev ? "partition 1" : "whole disk (no partition table)");
				usb_part = new (_part_store) uni::DiscPartition(*usb_disk, (int)use_dev);
				usb_fs = new (_fs_store) FilesysFAT(0, *usb_part, _fat_sector_buf, _fat_table_buf);
				if (!usb_fs->loadfs()) {
					XART1.OutFormat("[FAT] mount FAIL, error=%u\r\n", (unsigned)usb_fs->error_number);
				}
				else {
					XART1.OutFormat("[FAT] FAT%u mounted OK: total_sectors=%u spc=%u root_cluster=%u\r\n",
						(unsigned)usb_fs->fat_type, (unsigned)usb_fs->total_sectors,
						(unsigned)usb_fs->sectors_per_cluster, (unsigned)usb_fs->root_cluster);
					FatListRoot(*usb_fs);
					FatReadFirstFile(*usb_fs);
#if USBH_WRITE_TEST
					// 安全写测试：只写分区之外的空闲扇区，写完恢复原值，不碰文件系统
					MscWriteTest(*usb_disk, *usb_part);
#endif
#if USBH_FAT_WRITE_TEST
					FatWriteTest(*usb_fs);
#endif
				}
			}
		}
		// 枚举超时：把端口/通道现场打出来，首次把 H7 主机模式跑起来时很需要
		if (usb_otg_host && !enum_reported && (int32)(now_ms - enum_deadline) >= 0) {
			enum_reported = true;
			XART1.OutFormat("[USBH] Enumeration did NOT finish in 3s:\r\n");
			PrintHostStatus();
		}

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
			if (now_ms - ms_dn > 30) {
				ms_dn = now_ms;
				PrintHostStatus();
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
