// UTF-8 C++(ARMCLANG) TAB4 CRLF
// ADC3 通道18 = 内部温度传感器(VSENSE)，结果经 XART1 输出

// 预期现象，XART1 115200 8N1（板载 USB 转串口，PA9/PA10）：
//   - 上电打印一次：欢迎行、从系统存储区读出的两个 VSENSE 校准字、以及 Calibrate() 返回的校准因子。
//   - 之后每 500ms 一行 "ADC RAW: <16位原始值>, TEMPERATURE: <xx.xx> C"，同时 LEDB(PB0) 以相同节奏翻转。
//   - 这个值是 MCU 的结温，不是环境温度：芯片自身发热，通常会比室温高几度，而且这个传感器本身精度
//     就只有几度。让它跑重活、或给芯片加热/吹冷，读数会明显跟着动——跟着它变化是判断"真的读到了
//     传感器"的最快办法。
//   - 失败路径：TSEN 通道配不上、或 Calibrate() 超时，则 LEDR(PB1) 快速闪烁死循环，不再有输出。
//   - 若两个校准字相等，温度字段退化为 "n/a"，只报原始值。

#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;

// 非广告：以下为正点原子(Openedv)开发板参数
GPIN& LEDB = GPIOB[ 0];
GPIN& LEDR = GPIOB[ 1];
GPIN& KEYU = GPIOA[ 0];// 上
GPIN& KEYL = GPIOC[13];// 左
GPIN& KEYD = GPIOH[ 2];// 下
GPIN& KEYR = GPIOH[ 3];// 右

char* StrHeap(const char* valit_str){return (char*)valit_str;}
char* StrHeapAppendChars(char* dest, char chr, size_t n){return dest + n + chr;}
char* salc(size_t size){return 0;}
void outtxt(const char* str, stduint len) {XART1.out(str, len);}

// VSENSE 出厂校准值：仅在 VDDA = 3.3V 且 16 位分辨率下成立。
// 手头没有任何头文件定义这两个地址，出处是参考例程的驱动。
static const pureptr_t ADDR_TS_CAL1 = (pureptr_t)0x1FF1E820;// 30 摄氏度
static const pureptr_t ADDR_TS_CAL2 = (pureptr_t)0x1FF1E840;// 110 摄氏度

int main() {
	L1C.enAble();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro();
	
	LEDB.setMode(GPIOMode::OUT) = !false;
	LEDR.setMode(GPIOMode::OUT) = !false;
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);
	KEYL.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYD.setMode(GPIOMode::IN_Pull).setPull( true);
	KEYR.setMode(GPIOMode::IN_Pull).setPull( true);
	
	XART1.setMode(115200);
	XART1.OutFormat("Hello, %s on %s\r\n", "Example", "ADC3 INP18 MCU Temperature", _IDN_BOARD);
	
	const uint16 ts_cal1 = *(volatile uint16*)ADDR_TS_CAL1;
	const uint16 ts_cal2 = *(volatile uint16*)ADDR_TS_CAL2;
	XART1.OutFormat("TS_CAL1: %u (30C), TS_CAL2: %u (110C)\r\n",
		(unsigned)ts_cal1, (unsigned)ts_cal2);
	// 两个校准字相等会让下面的除法除零，此时干脆不做换算。
	const bool cal_ok = ts_cal2 != ts_cal1;
	if (!cal_ok) XART1.OutFormat("WARN: TS_CAL1 == TS_CAL2, scaling disabled\r\n");
	
	// ---- ADC3：内部温度传感器（VSENSE，通道18）----
	// 用 16 位：TS_CAL1/TS_CAL2 是按 16 位分辨率给出的；1 次规则转换，软件触发。
	ADC3.setMode(ADCRes::B16, 1);
	// 内部通路由 ADC3_CCR.TSEN 接通；setInnerChannel() 会打开它，而且与 setChannelNum()
	// 不同，它能报出实例选错或 ADC 已使能的情况。
	// 采样时间 810.5 周期，与参考驱动对该通道的取值一致。
	if (!ADC3.setInnerChannel(ADCInner::TSEN, 0, ADCSample::CYCL_810_5)) erro();
	// 必须写在 setMode() 之后：setMode() 自己会用 presc=0 先开一次时钟。
	// per_ck / 4 = 16MHz，与参考驱动选的 ADCCLK 相同。
	ADC3.enClock(true, 2);
	// 这里不需要 enDeepPowerDown()/enVoltageRegulator()：Calibrate() 会自己做这部分上电。
	// 它返回校准因子；若等待超时则返回 (uint32)~_IMM0。
	const uint32 calf = ADC3.Calibrate();
	if (calf == (uint32)~_IMM0) erro();
	XART1.OutFormat("CALFACT: %u\r\n", (unsigned)calf);
	
	uint32 raw = 0;
	while (1) {
		// 10 次采样、间隔 5ms、取平均，与参考例程的 Get_Adc_Average() 一致。
		uint32 sum = 0, cnt = 0;
		for0(i, 10) {
			ADC3.Start();
			if (!ADC3.Poll(0xFFFF)) break;// 只对真正转换成功的那几次取平均
			sum += (uint32)ADC3.inn();
			cnt++;
			SysDelay_ms(5);
		}
		if (cnt) raw = sum / cnt;
		
		// T = (110 - 30) / (TS_CAL2 - TS_CAL1) * (raw - TS_CAL1) + 30，单位是百分之一摄氏度。
		// 全程整数运算：OutFormat() 不解析精度字段。
		// (raw - ts_cal1) 先转成有符号再算：低于 30 摄氏度时它是负数。
		if (cal_ok) {
			sint32 t100 = ((sint32)raw - (sint32)ts_cal1) * 8000
				/ ((sint32)ts_cal2 - (sint32)ts_cal1) + 3000;
			uint32 a100 = t100 < 0 ? (uint32)(-t100) : (uint32)t100;
			XART1.OutFormat("ADC RAW: %u, TEMPERATURE: ", (unsigned)raw);
			if (t100 < 0) XART1.out("-", 1);// 负号单独打，-0.xx 才不会被吃掉符号
			XART1.OutFormat("%u.%02u C\r\n", (unsigned)(a100 / 100), (unsigned)(a100 % 100));
		}
		else XART1.OutFormat("ADC RAW: %u, TEMPERATURE: n/a\r\n", (unsigned)raw);
		LEDB.Toggle();
		SysDelay_ms(500);
	}
}

void erro(const char* str) {
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for(volatile unsigned i{0}; i < 1000000; i++){}
	}
}
