// ASCII CPP-ISO11 TAB4 CRLF
/*
 * 81-USBS-AudioCard (Stage 2): USB Audio Class 1.0 Enumeration & Control Plane Validation
 */

#define _DEBUG
#include <cpp/MCU/ST/STM32H7>
#include <cpp/Device/SAI>
#include <cpp/Device/DMA>
#include <cpp/Device/USB/USBPeri-Device.hpp>
#include <cpp/Device/USB/USBPeri-Class.hpp>
#include <c/driver/_predefine/predef.usbperi.hpp>
#include <c/mempool.h>
#include <c/consio.h>
#include <c/ustring.h>
#include <c/system/debug.h>
#include <new>

extern "C" char _IDN_BOARD[16] {"STM32H743IIT6"};

using namespace uni;
using namespace uni::device::SpaceUSB;

// Onboard LEDs and Buttons
GPIN& LEDB = GPIOB[ 0];// DS0
GPIN& LEDR = GPIOB[ 1];// DS1
GPIN& KEYU = GPIOA[ 0];// WK_UP (PA0)
GPIN& KEYL = GPIOC[13];// KEY2  (PC13) -> Volume -
GPIN& KEYD = GPIOH[ 2];// KEY1  (PH2)  -> Pause / Resume
GPIN& KEYR = GPIOH[ 3];// KEY0  (PH3)  -> Volume +

static byte _heap_pool[64 * 1024];

char* StrHeap(const char* valit_str) { return (char*)valit_str; }
char* StrHeapAppendChars(char* dest, char chr, size_t n) { return dest + n + chr; }
char* salc(size_t size) { return 0; }
extern "C" void outtxt(const char* str, stduint len) { XART1.out(str, len); }
int outsfmt(const char* fmt, ...) { outtxt(fmt, StrLength(fmt)); return 0; }
void printlogx(loglevel_t level, const char* fmt, para_list paras) {
	(void)level; (void)paras; outtxt(fmt, StrLength(fmt));
}

// Audio DMA buffers in D2 domain SRAM1/2 (accessible by DMA1)
#define AUDIO_DMA_BUF_SIZE   4096
#define AUDIO_RING_SLOTS     2
static byte* ring_buf[AUDIO_RING_SLOTS] = {
	(byte*)(0x30000000 + 0x0000),// slot 0: DMA M0 buffer
	(byte*)(0x30000000 + 0x1000),// slot 1: DMA M1 buffer
};

// Include AudioBridge before WM8978 / ES8388 codec drivers
#include <cpp/System/Audiosys/AudioBridge.hpp>
#include "../../../device/audio/wm8978.hpp"
#include "../../../device/audio/es8388.hpp"

// Software I2C delay for 480MHz clock
static void iic_delay() {
	for (volatile int i = 0; i < 500; i++) {}
}

// Software I2C driver for audio codec (PH4: SCL, PH5: SDA)
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

// Volume & Mute State
static uint8 current_volume = 70;
static bool  is_muted = false;
static bool  is_streaming = false;

// ---- Key interrupts (EXTI): the ISRs must stay tiny — they only bump a counter and
// let the main loop do the I2C/UART work. Nothing here may block or print. ----
static volatile uint32 key_ev_up = 0, key_ev_dn = 0, key_ev_lf = 0, key_ev_rt = 0;
void key_isr_up() { key_ev_up++; }// PA0  Up    : diagnostic dump + soft reconnect
void key_isr_dn() { key_ev_dn++; }// PH2  Down  : pause / play
void key_isr_lf() { key_ev_lf++; }// PC13 Left  : volume -
void key_isr_rt() { key_ev_rt++; }// PH3  Right : volume +

// ==================== Stage 3: USB PCM ring (EP1 isoc OUT -> SAI1/DMA1) ====================
// Producer: the USB interrupt (the class's out() pushes every received isochronous
// packet). Consumer: the SAI/DMA refill callback below.
// Deliberately single-producer/single-consumer: only the ISR moves pcm_wr and only
// the main loop moves pcm_rd, so no locking is needed. The ring itself is touched by
// the CPU only (the DMA reads the bridge's own buffers), so no cache upkeep either.
#define PCM_RING_SIZE 16384              // ~85 ms @ 48kHz stereo 16-bit
#define PCM_BYTES_PER_MS 192             // 48000 Hz * 2ch * 2B / 1000
static byte pcm_ring[PCM_RING_SIZE];
static volatile uint32 pcm_wr = 0, pcm_rd = 0;
static volatile uint32 pcm_packets = 0, pcm_bytes = 0;
static volatile uint32 pcm_overflows = 0, pcm_underruns = 0;
static volatile uint32 pcm_slips_skip = 0, pcm_slips_pad = 0;

// ---- Stage 4 watermark servo ----
// The host's 1 ms SOF clock and our SAI/PLL2 clock are independent, so the ring level
// random-walks (measured: ~500 B/s at 192000 B/s, i.e. ~0.27 %). With no correction it
// ends up either empty (dropouts, silence inserted) or full (whole 1 ms packets lost).
// So: outside the watermark band, slip a few SAMPLES per refill — 4 stereo frames
// per DMA buffer is a ~0.39 % max correction, i.e. enough to beat the drift, and the
// slip itself (83 us every ~21 ms) is far below audibility.
#define PCM_SLIP_BYTES  16               // 4 stereo frames = 83 us
#define PCM_LEVEL_MIN   1024             // ~5 ms: below this, hold a sliver of silence
#define PCM_LEVEL_MAX   8192             // ~43 ms: above this, skip a sliver

static uint32 PcmRingLevel() { return pcm_wr - pcm_rd; }
static void PcmRingReset() { pcm_rd = pcm_wr; }
static void PcmRingPush(const byte* src, uint32 n) {
	if (!n || n > PCM_RING_SIZE) return;
	if (n > (PCM_RING_SIZE - (pcm_wr - pcm_rd))) {// hard full: drop this packet, count it
		pcm_overflows++;
		return;
	}
	for (uint32 i = 0; i < n; i++) pcm_ring[(pcm_wr + i) % PCM_RING_SIZE] = src[i];
	pcm_wr += n;
	pcm_packets++;
	pcm_bytes += n;
}
static uint32 PcmRingPop(byte* dst, uint32 len) {
	uint32 avail = pcm_wr - pcm_rd;
	uint32 take = (avail < len) ? avail : len;
	for (uint32 i = 0; i < take; i++) dst[i] = pcm_ring[(pcm_rd + i) % PCM_RING_SIZE];
	pcm_rd += take;
	return take;
}

// SAI/DMA refill: never returns 0 — the bridge reads 0 as "stream over". A short ring
// is padded with silence and counted as an underrun; the servo above keeps that rare.
static uint32 pcm_refill_usb(void* ctx, byte* dst, uint32 len) {
	(void)ctx;
	uint32 avail = pcm_wr - pcm_rd;
	uint32 pad = 0;
	if (avail > PCM_LEVEL_MAX) {// producer ahead: skip a sliver of the backlog
		pcm_rd += PCM_SLIP_BYTES;
		pcm_slips_skip++;
	}
	else if (avail < PCM_LEVEL_MIN && avail < len) {// consumer ahead: hold a sliver
		pad = PCM_SLIP_BYTES;
		pcm_slips_pad++;
	}
	uint32 got = PcmRingPop(dst + pad, len - pad);
	if (pad) for (uint32 i = 0; i < pad; i++) dst[i] = 0;
	if (got < (len - pad)) {
		for (uint32 i = pad + got; i < len; i++) dst[i] = 0;
		pcm_underruns++;
	}
	return len;
}

static void set_hardware_volume(uint8 vol) {
	if (vol > 100) vol = 100;
	current_volume = vol;
	uint8 codec_reg = 0;
	if (active_codec == AudioCodecType::WM8978 && wm8978) {
		uint8 hp_vol = (uint8)(vol * 63 / 100);
		uint8 spk_vol = (uint8)(vol * 63 / 100);
		wm8978->SetHPVol(is_muted ? 0 : hp_vol, is_muted ? 0 : hp_vol);
		wm8978->SetSPKVol(is_muted ? 0 : spk_vol);
		codec_reg = is_muted ? 0 : hp_vol;
	}
	else if (active_codec == AudioCodecType::ES8388 && es8388) {
		uint8 vol_val = (uint8)(vol * 33 / 100);
		es8388->SetHPVol(is_muted ? 0 : vol_val);
		es8388->SetSPKVol(is_muted ? 0 : vol_val);
		codec_reg = is_muted ? 0 : vol_val;
	}
	// EVID: show the codec register actually written (ES8388 HP/SPK volume 0x2E..0x31)
	XART1.OutFormat("[Volume] Current Volume: %u%%%s (codec vol reg=0x%02X)\r\n",
		(unsigned)current_volume, is_muted ? " (MUTED)" : "", (unsigned)codec_reg);
}

// ==================== USB Descriptors (UAC 1.0) ====================

#define _USB_AUDIO_CONFIG_DESC_SIZE 109

static const byte _audio_dev_desc[18] = {
	0x12,                       // bLength
	0x01,                       // bDescriptorType = Device
	0x00, 0x02,                 // bcdUSB 2.00
	0x00, 0x00, 0x00,           // bDeviceClass, SubClass, Protocol (at Interface)
	64,                         // bMaxPacketSize0
	0x83, 0x04,                 // idVendor = 0x0483 (STMicroelectronics)
	0x30, 0x57,                 // idProduct = 0x5730 (Audio Card)
	0x00, 0x02,                 // bcdDevice = 2.00
	0x01, 0x02, 0x03,           // iManufacturer, iProduct, iSerialNumber
	0x01                        // bNumConfigurations
};

static const byte _audio_cfg_desc[_USB_AUDIO_CONFIG_DESC_SIZE] = {
	// Configuration Descriptor Header (9 bytes)
	0x09, 0x02, _USB_AUDIO_CONFIG_DESC_SIZE, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32,

	// Interface 0: AudioControl Standard Interface (9 bytes)
	0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,

	// AudioControl Class-Specific Header (9 bytes)
	0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, 0x01,

	// Input Terminal (ID=1, USB Streaming, 2 channels) (12 bytes)
	// bNrChannels = 0x02 (must match the stereo Format Type I descriptor below),
	// wChannelConfig = 0x0003 (Left + Right)
	0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,

	// Feature Unit (Unit ID=2, Source=1, Mute + Volume) (9 bytes)
	// bmaControls(0) = 0x03: bit0 = Mute, bit1 = Volume. With only bit0 Windows shows
	// no volume control for this unit, so dragging the slider never issues
	// SET_CUR(VOLUME) and GET_MIN/GET_MAX/GET_RES are never queried.
	0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00,

	// Output Terminal (ID=3, Speaker, Source=2) (9 bytes)
	0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x00,

	// Interface 1: AudioStreaming (Alt 0: Zero Bandwidth) (9 bytes)
	0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

	// Interface 1: AudioStreaming (Alt 1: Operational) (9 bytes)
	0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

	// AudioStreaming Class-Specific General (7 bytes)
	0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,

	// Format Type I (PCM 16-bit Stereo 48kHz) (11 bytes)
	0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 16, 0x01, 0x80, 0xBB, 0x00,

	// Standard AS Isochronous OUT Endpoint (EP1 OUT, MPS=192) (9 bytes)
	0x09, 0x05, 0x01, 0x01, 0xC0, 0x00, 0x01, 0x00, 0x00,

	// Class-Specific AS Isochronous Endpoint (7 bytes)
	0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00
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

static const byte* Audio_DeviceDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	len = sizeof(_audio_dev_desc);
	return _audio_dev_desc;
}
static const byte* Audio_LangIDDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	static const byte lang[] = { 0x04, 0x03, 0x09, 0x04 };
	len = sizeof(lang);
	return lang;
}
static const byte* Audio_ManufacturerDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("STMicroelectronics");
	len = r[0];
	return r;
}
static const byte* Audio_ProductDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString(_IDN_BOARD);// "STM32 Audio Card in FS Mode"
	len = r[0];
	return r;
}
static const byte* Audio_SerialDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("00000000001A");
	len = r[0];
	return r;
}
static const byte* Audio_ConfigurationStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("AUDIO Config");
	len = r[0];
	return r;
}
static const byte* Audio_InterfaceStrDescriptor(PeripheralSpeed speed, uint16& len) {
	(void)speed;
	const byte* r = MakeString("AUDIO Interface");
	len = r[0];
	return r;
}

// ==================== USB Audio Class Driver ====================

#define AUDIO_REQ_GET_CUR   0x81
#define AUDIO_REQ_SET_CUR   0x01
#define AUDIO_REQ_GET_MIN   0x82
#define AUDIO_REQ_GET_MAX   0x83
#define AUDIO_REQ_GET_RES   0x84

#define AUDIO_CONTROL_MUTE_CS   0x01
#define AUDIO_CONTROL_VOLUME_CS 0x02

// ---- UAC1 volume units ----
// The Feature Unit's Volume values (GET_CUR/GET_MIN/GET_MAX/GET_RES and SET_CUR) are
// SIGNED 16-bit in 1/256 dB: 0x0000 = 0 dB (loudest), negative = attenuation.
// We expose the codec's own span, -33 dB .. 0 dB with a 1 dB step. A raw 0..100
// scale is NOT usable here: the host reads it as a 0.39 dB span, judges the control
// useless and silently keeps the volume in software (no SET_CUR ever reaches us).
#define AUDIO_VOL_MIN_DB256  (-33 * 256)   // 0xDF00 = -33.00 dB
#define AUDIO_VOL_MAX_DB256  (0)           // 0x0000 =   0.00 dB
#define AUDIO_VOL_RES_DB256  (256)         // 0x0100 =   1.00 dB

// percent (0..100, our internal scale) -> 1/256 dB code
static uint16 VolPercentToDb256(uint8 percent) {
	if (percent > 100) percent = 100;
	int32 db256 = ((int32)percent * (-AUDIO_VOL_MIN_DB256) / 100) + AUDIO_VOL_MIN_DB256;
	// Snap onto the advertised resolution grid (GET_RES): the host expects every
	// reported/programmed value to be MIN + N*RES, otherwise it may distrust the node.
	int32 steps = (db256 - AUDIO_VOL_MIN_DB256) / AUDIO_VOL_RES_DB256;
	db256 = AUDIO_VOL_MIN_DB256 + steps * AUDIO_VOL_RES_DB256;
	return (uint16)(int16)db256;
}

// 1/256 dB code -> percent (0..100)
static uint8 VolDb256ToPercent(uint16 db256) {
	int32 db = (int16)db256;
	int32 span = -AUDIO_VOL_MIN_DB256;
	int32 att = (-db) * 100 / span;
	if (att < 0) att = 0;
	if (att > 100) att = 100;
	return (uint8)(100 - att);
}

class USBPeri_Audio : public ClassPeripheral {
public:
	byte alt_setting = 0;
	byte req_cmd = 0;
	byte req_unit = 0;
	byte req_cs = 0;
	byte req_buf[8]{};
	// Isochronous OUT (EP1) landing buffer: the host streams audio here as soon as
	// alt 1 is selected. It MUST be armed with ReceiveEndpoint(), otherwise a packet
	// arrives with no destination and the RxFIFO handler has nowhere to copy it.
	static const uint16 ISO_OUT_SIZE = 192;
	byte iso_out_buf[ISO_OUT_SIZE]{};
	uint32 iso_out_packets = 0;

	bool armISOOut() {
		if (!Parent()) return false;
		Parent()->ReceiveEndpoint(0x01, iso_out_buf, ISO_OUT_SIZE);
		return true;
	}

	bool setMode(byte cfgidx) override {
		(void)cfgidx;
		alt_setting = 0;
		if (Parent()) {
			Parent()->OpenEndpoint(0x01, 1, ISO_OUT_SIZE);
			armISOOut();
		}
		XART1.OutFormat("[USB] Audio Class Configured (Cfg %u)\r\n", (unsigned)cfgidx);
		return true;
	}

	bool canMode(byte cfgidx) override {
		(void)cfgidx;
		alt_setting = 0;
		// Bus reset / cable unplug / SET_CONFIGURATION(0): drop whatever was queued and
		// close the stream, so a re-plug starts from a clean ring (hot-plug recovery).
		is_streaming = false;
		PcmRingReset();
		if (Parent()) Parent()->CloseEndpoint(0x01);
		XART1.OutFormat("[USB] Audio Class De-configured\r\n");
		return true;
	}

	// EP1 (isoc OUT) packet completed: hand the samples to the PCM ring, then re-arm.
	// getRxCount() must be read BEFORE re-arming (arming resets the counter).
	bool out(byte epnum) override {
		if (epnum == 1) {
			uint16 n = Parent() ? Parent()->getRxCount(0x01) : 0;
			// Only queue audio while the stream is actually open (Alt 1 and not paused
			// by the board key): otherwise the ring fills up while nothing consumes it
			// and every packet is thrown away as an "overflow".
			if (n && is_streaming) PcmRingPush(iso_out_buf, n);
			iso_out_packets++;
			armISOOut();
		}
		return true;
	}

	bool Setup() override {
		if (!Parent()) return false;
		SetupPacket& req = Parent()->request;
		byte rtype = req.request_type;

		// 1) Standard Interface Requests (SET_INTERFACE / GET_INTERFACE / GET_DESCRIPTOR)
		if ((rtype & _USB_REQ_TYPE_MASK) == _USB_REQ_TYPE_STANDARD) {
			if (req.request == _USB_REQ_GET_DESCRIPTOR) {
				if ((req.value >> 8) == 0x21) {
					uint16 dlen = req.length > 9 ? 9 : req.length;
					Parent()->SendControl(_audio_cfg_desc + 18, dlen);
					return true;
				}
			}
			else if (req.request == _USB_REQ_SET_INTERFACE) {
				alt_setting = (byte)req.value;
				if (alt_setting == 1) {
					is_streaming = true;
					XART1.OutFormat("[USB] Audio Stream Start (Alt 1, 48000Hz Stereo 16-bit)\r\n");
				}
				else {
					is_streaming = false;
					XART1.OutFormat("[USB] Audio Stream Stop (Alt 0)\r\n");
				}
				return true;
			}
			else if (req.request == _USB_REQ_GET_INTERFACE) {
				Parent()->SendControl(&alt_setting, 1);
				return true;
			}
		}

		// 2) Class-Specific Requests (GET_CUR, SET_CUR, GET_MIN, GET_MAX, GET_RES)
		if ((rtype & _USB_REQ_TYPE_MASK) == _USB_REQ_TYPE_CLASS) {
			req_cmd = req.request;
			req_unit = (byte)(req.index >> 8);
			req_cs = (byte)(req.value >> 8);
			// EVID: log every UAC class request so the control plane is auditable —
			// cmd: 0x81=CUR 0x82=MIN 0x83=MAX 0x84=RES 0x01=SET_CUR;
			// cs : 1=Mute 2=Volume; ch : 0=master (wValue low byte)
			XART1.OutFormat("[USB] ClassReq cmd=0x%02X cs=%u unit=%u ch=%u itf=%u len=%u\r\n",
				(unsigned)req_cmd, (unsigned)req_cs, (unsigned)req_unit,
				(unsigned)(req.value & 0xFF), (unsigned)(req.index & 0xFF), (unsigned)req.length);

			if (req_cmd == AUDIO_REQ_GET_CUR) {
				if (req_cs == AUDIO_CONTROL_MUTE_CS) {
					byte mute_val = is_muted ? 1 : 0;
					Parent()->SendControl(&mute_val, 1);
					return true;
				}
				else if (req_cs == AUDIO_CONTROL_VOLUME_CS) {
					uint16 vol_val = VolPercentToDb256(current_volume);
					XART1.OutFormat("[USB] Get Volume: %u%% (%d dB x256=0x%04X)\r\n",
						(unsigned)current_volume, (int)((int16)vol_val) / 256, (unsigned)vol_val);
					Parent()->SendControl((byte*)&vol_val, 2);
					return true;
				}
			}
			else if (req_cmd == AUDIO_REQ_GET_MIN) {
				uint16 min_vol = (uint16)(int16)AUDIO_VOL_MIN_DB256;
				XART1.OutFormat("[USB] Get Volume Range: min=%d dB\r\n", AUDIO_VOL_MIN_DB256 / 256);
				Parent()->SendControl((byte*)&min_vol, 2);
				return true;
			}
			else if (req_cmd == AUDIO_REQ_GET_MAX) {
				uint16 max_vol = (uint16)(int16)AUDIO_VOL_MAX_DB256;
				XART1.OutFormat("[USB] Get Volume Range: max=%d dB\r\n", AUDIO_VOL_MAX_DB256 / 256);
				Parent()->SendControl((byte*)&max_vol, 2);
				return true;
			}
			else if (req_cmd == AUDIO_REQ_GET_RES) {
				uint16 res_vol = AUDIO_VOL_RES_DB256;
				XART1.OutFormat("[USB] Get Volume Range: res=%d dB\r\n", AUDIO_VOL_RES_DB256 / 256);
				Parent()->SendControl((byte*)&res_vol, 2);
				return true;
			}
			else if (req_cmd == AUDIO_REQ_SET_CUR) {
				Parent()->ReceiveControl(req_buf, req.length > sizeof(req_buf) ? sizeof(req_buf) : req.length);
				return true;
			}
		}

		Parent()->StallControl();
		return false;
	}

	bool EP0RxReady() override {
		if (req_cmd == AUDIO_REQ_SET_CUR) {
			if (req_cs == AUDIO_CONTROL_MUTE_CS) {
				is_muted = (req_buf[0] != 0);
				XART1.OutFormat("[USB] Set Mute: %s\r\n", is_muted ? "MUTED" : "UNMUTED");
				set_hardware_volume(current_volume);
			}
			else if (req_cs == AUDIO_CONTROL_VOLUME_CS) {
				// UAC1: signed 1/256 dB. Convert to our 0..100 % scale before use.
				uint16 db256 = req_buf[0] | ((uint16)req_buf[1] << 8);
				uint8 host_vol = VolDb256ToPercent(db256);
				XART1.OutFormat("[USB] Set Volume: %u%% (%d dB)\r\n",
					(unsigned)host_vol, (int)((int16)db256) / 256);
				set_hardware_volume(host_vol);
			}
			req_cmd = 0;
		}
		return true;
	}

	const byte* getConfigurationDescriptor(uint16& len) override {
		len = sizeof(_audio_cfg_desc);
		return _audio_cfg_desc;
	}
	const byte* getOtherSpeedConfigurationDescriptor(uint16& len) override {
		len = sizeof(_audio_cfg_desc);
		return _audio_cfg_desc;
	}
	const byte* getDeviceQualifierDescriptor(uint16& len) override {
		static const byte dev_qualifier[10] = {
			10, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 64, 0x01, 0x00
		};
		len = sizeof(dev_qualifier);
		return dev_qualifier;
	}
};

// ==================== USB Interrupt & PCD Forwarding ====================

static PeripheralDevice* p_usb_core = nullptr;
static USBPeri_Audio* p_usb_audio = nullptr;

static volatile uint32 ev_reset_cnt = 0;
static volatile uint32 ev_setup_cnt = 0;
static volatile uint32 ev_susp_cnt = 0;
static volatile uint32 ev_resume_cnt = 0;
static volatile uint32 ev_conn_cnt = 0;
static volatile uint32 ev_disconn_cnt = 0;
struct LastUSBReq {
	uint8 req_type;
	uint8 req;
	uint16 val;
	uint16 idx;
	uint16 len;
	uint32 seq;
};
static volatile LastUSBReq g_last_req{};

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
void on_connect() {
	ev_conn_cnt++;
	if (p_usb_core) p_usb_core->HandleConnect();
}
void on_disconnect() {
	ev_disconn_cnt++;
	if (p_usb_core) p_usb_core->HandleDisconnect();
}
void on_dataout(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataOut((byte)(stduint)epnum); }
void on_datainn(void* epnum, ...) { if (p_usb_core) p_usb_core->HandleDataInn((byte)(stduint)epnum); }

static volatile uint32 ev_irq_cnt = 0;
void on_usb_fs_interrupt() {
	ev_irq_cnt++;
}

// ---- [EVID] USB bring-up evidence dump (COMMENTED OUT — bring-up forensics, kept for
// later re-use: it prints the registers that decide whether the OTG core can hear the
// bus, which is how the RCC_AHB1ENR bit28 / USB2 clock clobber was found) ----
#if 0
static void DumpUSBDiag(const char* tag) {
	uint32 otg = _OTG2_FS_ADDR;
	uint32 cr = *(volatile uint32*)0x58024400;// RCC_CR
	uint32 ccip2 = *(volatile uint32*)0x58024454;// RCC_D2CCIP2R (USBSEL[21:20])
	uint32 ahb1 = *(volatile uint32*)0x580244D8;// RCC_AHB1ENR
	uint32 gccfg = *(volatile uint32*)(otg + 0x038);
	uint32 dctl = *(volatile uint32*)(otg + 0x804);
	uint32 dsts = *(volatile uint32*)(otg + 0x808);
	uint32 gsts = *(volatile uint32*)(otg + 0x014);
	XART1.OutFormat("[EVID %s] RCC_CR=0x%08X HSI48ON=%d RDY=%d D2CCIP2R=0x%08X USBSEL=%d AHB1ENR=0x%08X DMA1=%d MUX1=%d USB2FS=%d ULPI2bit28=%d\r\n",
		tag, (unsigned)cr, (int)((cr >> 12) & 1), (int)((cr >> 13) & 1),
		(unsigned)ccip2, (int)((ccip2 >> 20) & 3), (unsigned)ahb1,
		(int)(ahb1 & 1), (int)((ahb1 >> 2) & 1), (int)((ahb1 >> 27) & 1), (int)((ahb1 >> 28) & 1));
	XART1.OutFormat("[EVID %s] GCCFG=0x%08X PWRDWN=%d DCTL=0x%08X SDIS=%d DSTS=0x%08X SUSP=%d SPD=%d GINTSTS=0x%08X RST=%d ENUM=%d RXFLVL=%d SOF=%d\r\n",
		tag, (unsigned)gccfg, (int)((gccfg >> 16) & 1), (unsigned)dctl, (int)((dctl >> 1) & 1),
		(unsigned)dsts, (int)(dsts & 1), (int)((dsts >> 1) & 3), (unsigned)gsts,
		(int)((gsts >> 12) & 1), (int)((gsts >> 13) & 1), (int)((gsts >> 4) & 1), (int)((gsts >> 3) & 1));
	XART1.OutFormat("[EVID %s] IRQ=%u Reset=%u Setup=%u Susp=%u Resume=%u\r\n",
		tag, (unsigned)ev_irq_cnt, (unsigned)ev_reset_cnt, (unsigned)ev_setup_cnt,
		(unsigned)ev_susp_cnt, (unsigned)ev_resume_cnt);
}
#else
static void DumpUSBDiag(const char* tag) { (void)tag; }// keep the call sites compiling
#endif

void erro(const char* str) {
	if (str) XART1.OutFormat("[Error] %s\r\n", str);
	LEDR.setMode(GPIOMode::OUT);
	while (true) {
		LEDR.Toggle();
		for (volatile unsigned i{0}; i < 1000000; i++) {}
	}
}

// ---- [EVID] fault dumper: the framework's weak HardFault_Handler only calls erro(),
// which prints nothing — print the stacked PC/LR plus the fault status registers so a
// crash points at its instruction (AKA SCB->CFSR/HFSR/MMFAR/BFAR). ----
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

int main() {
	// Initialize mempool first for _MEMMAN.cpp backing new/malloc
	{
		extern uni::Mempool mempool;
		mempool.enable_auto_expand = false;
		mempool.Append(Slice{ (stduint)(pureptr_t)_heap_pool, byteof(_heap_pool) });
	}

	// I-cache only: the SAI/DMA read the ring buffers (D2 SRAM, cacheable by default)
	// and the DMA does not snoop the D-cache — with the D-cache on, the audio the DMA
	// fetches would be stale. Same choice as 92-AudioPlay.
	L1C.enAbleICacheAll();
	NVIC.setPriorityGroup(2);
	if (!RCC.setClock(SysclkSource::HSE)) erro("Clock init failed");

	GPIOA.enClock(true);
	GPIOB.enClock(true);
	GPIOC.enClock(true);
	GPIOH.enClock(true);
	GPIOE.enClock(true);
	GPIOG.enClock(true);

	LEDB.setMode(GPIOMode::OUT) = true;
	LEDR.setMode(GPIOMode::OUT) = true;

	// ---- Keys: interrupt mode (EXTI) ----
	// The reference example's KEY0/KEY1/KEY2/WK_UP are this board's Right/Down/Left/Up:
	//   Up    = PA0  (active high, pull-down)   Down  = PH2  (active low, pull-up)
	//   Left  = PC13 (active low)               Right = PH3  (active low)
	// Polled keys used to cost the audio path dearly: "While (KEY == 0);" blocked the
	// main loop (and therefore ServicePlayback()) for as long as the key was held, which
	// is far longer than the 21 ms DMA half-buffer — that was the click while adjusting
	// the volume. The ISRs now only count events; the loop does the slow work.
	KEYU.setMode(GPIOMode::IN_Pull).setPull(false);// Up    (PA0)
	KEYL.setMode(GPIOMode::IN_Pull).setPull(true); // Left  (PC13) volume -
	KEYD.setMode(GPIOMode::IN_Pull).setPull(true); // Down  (PH2)  pause / play
	KEYR.setMode(GPIOMode::IN_Pull).setPull(true); // Right (PH3)  volume +
	// Press edge only; priority 3 = below USB (1) and the audio DMA (0), so a key can
	// never delay enumeration or the DMA refill.
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
	XART1.OutFormat("  UNISYM STM32H743 81-USBS-AudioCard \r\n");
	XART1.OutFormat("====================================================\r\n");

	// ==================== Step 1: Initialize Audio Codec ====================
	XART1.OutFormat("Initializing Audio Codec...\r\n");

	audio_iic = new (audio_wire_buf) AudioIIC(GPIOH[5], GPIOH[4]);
	audio_iic->func_delay = iic_delay;

	es8388 = new (es8388_buf) ES8388_t(*audio_iic);
	byte es_r0 = es8388->Read(0);
	bool es_detected = (audio_iic->getError() == ERR_IIC_NONE);

	if (es_detected) {
		active_codec = AudioCodecType::ES8388;
		bool ok = es8388->Initialize();
		es8388->SetADDA(true, false);
		es8388->SetOutput(true, true);
		XART1.OutFormat("Audio Codec Detected: ES8388 (R0=0x%02X, Init=%s)\r\n",
			(unsigned)es_r0, ok ? "OK" : "FAIL");
	}
	else {
		wm8978 = new (wm8978_buf) WM8978_t(*audio_iic);
		bool wm_ok = wm8978->Initialize();
		if (wm_ok) {
			active_codec = AudioCodecType::WM8978;
			wm8978->SetADDA(true, false);
			wm8978->SetInput(false, false, false);
			wm8978->SetOutput(true, false);
			XART1.OutFormat("Audio Codec Detected: WM8978 (Init=OK)\r\n");
		}
		else {
			active_codec = AudioCodecType::None;
			XART1.OutFormat("Audio Codec Detection FAILED!\r\n");
		}
	}

	set_hardware_volume(current_volume);

	// ==================== Step 2: Initialize USB OTG2_FS Device (Disconnected) ====================
	XART1.OutFormat("Initializing USB OTG2_FS Device Stack...\r\n");

	// 0) Enable VDD33USB detector and wait for regulator ready
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// PWR_CR3.USB33DEN
	while (!(*(volatile stduint*)0x5802480CU & 0x04000000U));// wait PWR_CR3.USB33RDY (bit 26)

	// 1) Enable HSI48 for USB 48MHz Kernel Clock
	RCC[uni::RCCReg::CR].setof(12, true);          // HSI48ON
	while (!RCC[uni::RCCReg::CR].bitof(13));       // wait HSI48RDY
	uni::RCC.setPeriphClock(uni::PeriphClock::USB, uni::ClockSource::HSI48);// D2CCIP2R.USBSEL

	// 2) OTG2_FS pins: PA11/PA12 AF10
	GPIOA[11].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);
	GPIOA[12].setMode(GPIOMode::OUT_AF_PushPull, GPIOSpeed::Veryhigh)._set_alternate(10);

	// 3) USB port power switch (PG13 high)
	GPIOG[13].setMode(GPIOMode::OUT) = true;

	// 4) init PCD2 (device controller, USB2_OTG_FS on the Openedv board)
	PCD2.base = _OTG2_FS_ADDR;
	PCD2.speed = 3;             // USB_OTG_SPEED_FULL: DEVSPD=3 (FS 48MHz embedded)
	PCD2.ep0_mps = 64;
	PCD2.dev_endpoints = 8;
	PCD2.phy_itface = 2;        // PCD_PHY_EMBEDDED (internal FS transceiver)
	PCD2.dma_enable = false;
	PCD2.vbus_sensing_enable = false;
	PCD2.sof_enable = false;
	PCD2.lpm_enable = false;
	PCD2.battery_charging_enable = false;
	PCD2.use_dedicated_ep1 = false;

	if (!PCD2.setMode()) {
		XART1.OutFormat("[USB] PCD init failed!\r\n");
		erro("PCD Init Error");
	}

	// 5) FIFO sizes
	PCD2.setRxFiFo(0x80);
	PCD2.setTxFiFo(0, 0x40);
	PCD2.setTxFiFo(1, 0x40);

	// 6) Setup USB Device & Register UAC 1.0 Class
	static PeripheralDescriptor desc;
	static PeripheralDevice usb_core;
	static USBPeri_Audio usb_audio;
	p_usb_core = &usb_core;
	p_usb_audio = &usb_audio;

	desc.GetDeviceDescriptor = Audio_DeviceDescriptor;
	desc.GetLangIDStrDescriptor = Audio_LangIDDescriptor;
	desc.GetManufacturerStrDescriptor = Audio_ManufacturerDescriptor;
	desc.GetProductStrDescriptor = Audio_ProductDescriptor;
	desc.GetSerialStrDescriptor = Audio_SerialDescriptor;
	desc.GetConfigurationStrDescriptor = Audio_ConfigurationStrDescriptor;
	desc.GetInterfaceStrDescriptor = Audio_InterfaceStrDescriptor;

	usb_core.setMode(PCD2, desc);
	usb_core.RegisterClass(usb_audio);

	// 7) Wire PCD interrupt handlers
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

	// 8) Start USB core
	usb_core.Start();

	// Enable USB 3.3V voltage detector
	*(volatile stduint*)0x5802480CU |= 0x01000000U;// PWR_CR3.USB33DEN

	XART1.OutFormat("[USB] Started! DCTL=0x%08X (SDIS bit1=%d), PWR_CR3=0x%08X (RDY=%d)\r\n",
		(unsigned)*(volatile uint32*)(_OTG2_FS_ADDR + 0x804),
		(int)((*(volatile uint32*)(_OTG2_FS_ADDR + 0x804) >> 1) & 1),
		(unsigned)*(volatile uint32*)0x5802480C,
		(int)((*(volatile uint32*)0x5802480C >> 26) & 1));
	XART1.OutFormat("[USB] Ready! Connect USB cable to PC.\r\n");

	// ==================== Step 3: Initialize Audio Bridge (SAI1 + DMA) ====================
	XART1.OutFormat("Initializing Audio Bridge...\r\n");
	uni::AudioDeviceInterface* audio_chip = nullptr;
	if (active_codec == AudioCodecType::ES8388) audio_chip = es8388;
	else if (active_codec == AudioCodecType::WM8978) audio_chip = wm8978;

	uni::AudioBridge bridge(ring_buf, 2, AUDIO_DMA_BUF_SIZE, audio_chip);

	uni::AudioFormat audio_fmt;
	audio_fmt.channels = 2;
	audio_fmt.sample_rate = 48000;
	audio_fmt.sample_format = uni::AudioSampleFormat::S16LE;

	// Stage 3: the SAI/DMA are NOT started at boot any more — the host's
	// SET_INTERFACE(Alt 1) starts the stream and Alt 0 pauses it (see the main loop),
	// so nothing plays before the PC actually streams audio. StartStream() is what
	// programs PLL2/SAI1/DMA1 and prefills the double buffer from the PCM ring.
	DumpUSBDiag("bridgeReady");
	XART1.OutFormat("[Audio] Bridge ready (SAI1+DMA1 idle): waiting for host Alt 1\r\n");

	uint32 tick = 0;

	// Main loop
	while (true) {
		bridge.ServicePlayback();

		// ---- Stage 3: stream lifecycle follows the host's SET_INTERFACE(Alt) ----
		static bool stream_go = false;
		if (is_streaming != stream_go) {
			stream_go = is_streaming;
			PcmRingReset();// never replay stale samples after an Alt switch
			if (stream_go) {
				if (!bridge.isRunning()) {
					if (!bridge.StartStream(audio_fmt, pcm_refill_usb, nullptr))
						XART1.OutFormat("[Audio] StartStream FAILED\r\n");
				}
				bridge.ResumeStream();
				XART1.OutFormat("[Audio] USB PCM -> SAI1 running (48000Hz/16bit/2ch)\r\n");
			}
			else {
				bridge.PauseStream();// silence; SAI/DMA keep clocking (the host toggles Alt often)
				XART1.OutFormat("[Audio] Playback paused (Alt 0), ring cleared\r\n");
			}
		}

		// ---- Data-flow watchdog ----
		// Quiet by design: a second that streamed normally prints NOTHING. A line is
		// emitted only when something actually went wrong — lost packets (ovf), inserted
		// silence (unf), or a saturated watermark servo (>=30 slips/s out of 47 max).
		static uint32 stats_ms = 0;
		static uint32 last_pk = 0, last_by = 0, last_ovf = 0, last_unf = 0, last_sk = 0, last_pd = 0;
		uint32 now_ms = SysTick::getTick();
		if (now_ms - stats_ms >= 1000) {
			uint32 dt = now_ms - stats_ms;
			stats_ms = now_ms;
			uint32 pk = pcm_packets, by = pcm_bytes;
			uint32 ovf = pcm_overflows, unf = pcm_underruns;
			uint32 sk = pcm_slips_skip, pd = pcm_slips_pad;
			uint32 slip = (sk - last_sk) + (pd - last_pd);
			if ((ovf != last_ovf) || (unf != last_unf) || (slip >= 30)) {
				XART1.OutFormat("[Stream] %ums: pkt=+%u/s byte=+%u/s ring=%uB ovf=+%u unf=+%u slip=-%u/+%u played=%u run=%u\r\n",
					(unsigned)dt,
					(unsigned)((uint64)(pk - last_pk) * 1000 / dt),
					(unsigned)((uint64)(by - last_by) * 1000 / dt),
					(unsigned)PcmRingLevel(), (unsigned)(ovf - last_ovf), (unsigned)(unf - last_unf),
					(unsigned)(sk - last_sk), (unsigned)(pd - last_pd),
					(unsigned)bridge.getPlayedBytes(), (unsigned)(bridge.isRunning() ? 1 : 0));
			}
			last_pk = pk;
			last_by = by;
			last_ovf = ovf;
			last_unf = unf;
			last_sk = sk;
			last_pd = pd;
		}

		// ---- Key events (EXTI-driven; debounced here, no blocking wait) ----
		static uint32 seen_up = 0, seen_dn = 0, seen_lf = 0, seen_rt = 0;
		static uint32 ms_up = 0, ms_dn = 0, ms_lf = 0, ms_rt = 0;
		if (seen_rt != key_ev_rt) {// Right (PH3): volume +
			seen_rt = key_ev_rt;
			if (now_ms - ms_rt > 30) {
				ms_rt = now_ms;
				if (current_volume <= 95) set_hardware_volume(current_volume + 5);
				else set_hardware_volume(100);
			}
		}
		if (seen_lf != key_ev_lf) {// Left (PC13): volume -
			seen_lf = key_ev_lf;
			if (now_ms - ms_lf > 30) {
				ms_lf = now_ms;
				if (current_volume >= 5) set_hardware_volume(current_volume - 5);
				else set_hardware_volume(0);
			}
		}
		if (seen_dn != key_ev_dn) {// Down (PH2): pause / play
			seen_dn = key_ev_dn;
			if (now_ms - ms_dn > 30) {
				ms_dn = now_ms;
				is_streaming = !is_streaming;
				XART1.OutFormat("[Audio] Manual %s\r\n", is_streaming ? "PLAY" : "PAUSE");
			}
		}

		static uint32 last_seen_reset = 0;
		static uint32 last_seen_setup = 0;
		if (ev_reset_cnt != last_seen_reset) {
			last_seen_reset = ev_reset_cnt;
			XART1.OutFormat("[Live USB] Bus Reset Detected (Total %u)\r\n", (unsigned)ev_reset_cnt);
		}
		if (g_last_req.seq != last_seen_setup) {
			last_seen_setup = g_last_req.seq;
			XART1.OutFormat("[Live USB] SETUP #%u: Type=0x%02X, Req=0x%02X, Val=0x%04X, Idx=0x%04X, Len=%u\r\n",
				(unsigned)last_seen_setup,
				(unsigned)g_last_req.req_type,
				(unsigned)g_last_req.req,
				(unsigned)g_last_req.val,
				(unsigned)g_last_req.idx,
				(unsigned)g_last_req.len);
		}

		// Up key (PA0): soft reconnect pulse (simulates unplug/replug)
		if (seen_up != key_ev_up) {
			seen_up = key_ev_up;
			if (now_ms - ms_up > 200) {
				ms_up = now_ms;
#if 0
				// [Diag]/[EVID] forensics — commented out after bring-up, kept for later:
				uint32 dctl = *(volatile uint32*)(_OTG2_FS_ADDR + 0x804);
				uint32 dsts = *(volatile uint32*)(_OTG2_FS_ADDR + 0x808);
				uint32 gintsts = *(volatile uint32*)(_OTG2_FS_ADDR + 0x014);
				uint32 gintmsk = *(volatile uint32*)(_OTG2_FS_ADDR + 0x018);
				uint32 gahbcfg = *(volatile uint32*)(_OTG2_FS_ADDR + 0x008);
				uint32 pwr_cr3 = *(volatile uint32*)0x5802480C;
				XART1.OutFormat("[Diag] DCTL=0x%08X (SDIS=%d), DSTS=0x%08X (Susp=%d, EnumSpd=%d)\r\n",
					dctl, (int)((dctl >> 1) & 1), dsts, (int)(dsts & 1), (int)((dsts >> 1) & 3));
				XART1.OutFormat("[Diag] GINTSTS=0x%08X, GINTMSK=0x%08X, GAHBCFG=0x%08X, PWR_CR3=0x%08X\r\n",
					gintsts, gintmsk, gahbcfg, pwr_cr3);
				uint32 gpioa_moder = *(volatile uint32*)0x58020000;
				uint32 gpioa_afrh = *(volatile uint32*)0x58020024;
				uint32 gpioa_pupdr = *(volatile uint32*)0x5802000C;
				uint32 gpiog_odr = *(volatile uint32*)0x58021814;
				XART1.OutFormat("[Diag GPIO] MODER=0x%08X, AFRH=0x%08X, PUPDR=0x%08X, GPIOG_ODR=0x%08X\r\n",
					gpioa_moder, gpioa_afrh, gpioa_pupdr, gpiog_odr);
				XART1.OutFormat("[Diag NVIC] ISER3=0x%08X, ISPR3=0x%08X, IRQ_Fired=%u\r\n",
					*(volatile uint32*)0xE000E10C, *(volatile uint32*)0xE000E20C, (unsigned)ev_irq_cnt);
				DumpUSBDiag("key");
				XART1.OutFormat("[Diag] Events: Reset=%u, Setup=%u (LastReq=Type 0x%02X Req 0x%02X Val 0x%04X)\r\n",
					(unsigned)ev_reset_cnt, (unsigned)ev_setup_cnt,
					(unsigned)g_last_req.req_type, (unsigned)g_last_req.req, (unsigned)g_last_req.val);
#endif

				// Force a 500ms soft-disconnect pulse on the Up key to simulate unplug and replug
				XART1.OutFormat("[USB] Pulsing Soft Reconnect (500ms DCTL.SDIS=1)...\r\n");
				PCD2.Disconnect();
				SysDelay_ms(500);
				PCD2.Connect();
				XART1.OutFormat("[USB] Reconnected!\r\n");
			}
		}

		// Heartbeat blink every 500ms
		tick++;
		if (tick >= 250000) {
			tick = 0;
			LEDB.Toggle();
		}
	}
}
