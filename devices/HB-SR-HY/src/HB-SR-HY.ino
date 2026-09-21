//- -----------------------------------------------------------------------------------------------------------------------
// HB-SR-HY v21 idle-aware
// Stateful BidCoS protocol proxy for an original HM-CC-TC <-> original HM-CC-VD pair.
//
// Logical links:
//   Link A: original HM-CC-TC <-> HY acting as VD
//   Link B: HY acting as TC     <-> original HM-CC-VD
//
// Runtime principles:
//   * Link A and Link B are deliberately NOT packet-forwarded 1:1.
//   * Link A gets an immediate synthetic ACK_STATUS built from the cached real VD state.
//   * Link B has its own message counter and its own 120.00 ... 183.75 s schedule.
//   * Link B sends exactly one A2 58 in the calculated slot and never does an immediate retry.
//   * Link-B ACK_STATUS is received asynchronously by one shared CC1101 dispatcher.
//   * A central Link-B scheduler arbitrates collisions between virtual HYs. A losing
//     slot is skipped (never delayed), its counter/time line advances without drift,
//     and collision debt gives that HY priority until it has again completed one
//     successful Link-B exchange with a matching VD ACK_STATUS.
//   * Unsolicited CCU actuator-status ACK/retry is asynchronous; no blocking
//     AskSin++ waitAck() may consume TC/VD frames behind the shared dispatcher.
//     This is required when multiple logical HY devices share one physical radio.
//   * CC1101 RX bandwidth is widened from the AskSin++ default (~101.6 kHz) to ~203.1 kHz.
//   * Carrier frequency / FSCTRL0 are left unchanged.
//   * Only normal valve positions are hydraulically scaled. Special commands are conservative/transparent.
//
// Existing EEPROM compatibility:
//   Register 0x02 is intentionally still named "factor" and retains the old semantics:
//       factor = percentage passed to the real VD
//       factor 100 -> reduction x = 0 %
//       factor  80 -> reduction x = 20 %
//   This avoids turning an already stored default factor=100 into a 100 % reduction after a firmware update.
//
// Important scope limitations matching the still-open points in the protocol notes:
//   * AES/security handshakes for the custom 0x58/0x02 runtime path are not implemented here.
//   * The exact original VD RX-window width is still an empirical point; cyclic Link-B timing uses no fixed offset.
//   * A real power cycle still starts a fresh Link-B phase. A soft-watchdog recovery, however, preserves
//     counter/phase/target/cache in .noinit RAM so the real VD stays synchronized.
//   * HY does not invent a VD error bit. If no valid VD traffic is seen for ten minutes, HY instead
//     stops answering Link A so the original TC detects the missing valve exactly as with a lost VD.
//   Pairing/configuration remains handled by the AskSin++ base implementation.
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

// Pairing troubleshooting: disable AES to avoid key/index mismatch during teach-in.
// #define USE_AES
// #define HM_DEF_KEY 0x5f,0x8f,0xe3,0x01,0xaf,0x69,0x38,0xcf,0x1f,0xc1,0xca,0x6c,0x3d,0xf0,0x4b,0x01
// #define HM_DEF_KEY_INDEX 2

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Switch.h>

// Flash-saving long-test build:
// compile out the very verbose AskSin++ diagnostics below. Critical HY events
// are logged explicitly through Serial with short messages.
#ifdef DPRINT
#undef DPRINT
#endif
#ifdef DPRINTLN
#undef DPRINTLN
#endif
#ifdef DHEX
#undef DHEX
#endif
#ifdef DINIT
#undef DINIT
#endif
#define DPRINT(...)   do {} while (0)
#define DPRINTLN(...) do {} while (0)
#define DHEX(...)     do {} while (0)
#define DINIT(...)    do {} while (0)


#define LED_PIN 4
#define CONFIG_BUTTON_PIN 8

#define PEERS_PER_CHANNEL 4
#define NUM_CHANNELS 2

using namespace as;

// Capture reset cause before Arduino/AskSin initialization and disable any
// watchdog inherited from a previous hardware reset as early as possible.
//
// The classic Pro Mini bootloader can get stuck in an endless reset loop after
// a soft watchdog reset. Therefore HY uses watchdog interrupt mode only.
// On timeout the ISR records the current radio stage and jumps directly to the
// application reset vector, bypassing the bootloader.
uint8_t hyResetCause __attribute__((section(".noinit")));

volatile uint8_t  hyRadioStage __attribute__((section(".noinit")));
volatile uint8_t  hyRecoveredRadioStage __attribute__((section(".noinit")));
volatile uint16_t hySoftWatchdogMagic __attribute__((section(".noinit")));

static const uint16_t HY_SOFT_WDT_MAGIC = 0x4859; // 'HY'

// Runtime state retained only across our direct soft-watchdog application restart.
// It is intentionally NOT used after a true power cycle/reset.
static const uint16_t HY_RECOVERY_STATE_MAGIC = 0x4852; // 'HR'
static const uint8_t HY_RECOVERY_DEVICE_COUNT = 2;

struct HyRecoveryState {
  uint16_t magic;
  uint8_t flags;             // bit0 schedule, bit1 target, bit2 lastNormal,
                             // bit3 cache, bits4..5 HyMode
  uint8_t hyTcCounter;
  uint8_t currentTcCommand;
  uint8_t currentTcTargetRaw;
  uint8_t desiredVdTargetRaw;
  uint8_t lastSentVdTargetRaw;
  uint8_t collisionDebt;
  uint8_t lastVdPositionRaw;
  uint8_t lastVdStatus;
  uint32_t nextVdBaseMs;
};

HyRecoveryState hyRecoveryStates[HY_RECOVERY_DEVICE_COUNT]
  __attribute__((section(".noinit")));
volatile uint32_t hyWatchdogOldMillis __attribute__((section(".noinit")));

// Low-level SPI faults are latched without printing inside tight radio loops.
// The CC1101 recovery layer logs one compact diagnostic instead.
volatile uint8_t hySpiFaultCode = 0;

void hyCaptureResetCause(void)
  __attribute__((naked))
  __attribute__((section(".init3")));

void hyCaptureResetCause(void) {
  hyResetCause = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

// Watchdog timeout recovery without a hardware reset/bootloader pass.
// The C runtime entered at address 0 reinitializes stack/BSS before setup().
ISR(WDT_vect) {
  hyRecoveredRadioStage = hyRadioStage;
  hyWatchdogOldMillis = millis();
  hySoftWatchdogMagic = HY_SOFT_WDT_MAGIC;
  asm volatile("jmp 0");
}

static void enableHySoftWatchdog() {
  cli();
  wdt_reset();
  MCUSR &= ~_BV(WDRF);

  // Timed watchdog configuration sequence.
  WDTCSR = _BV(WDCE) | _BV(WDE);

  // 8 s, interrupt only (WDIE=1, WDE=0). 8 s is deliberately generous so
  // normal AskSin++ retries/pairing cannot trigger a false recovery.
  WDTCSR = _BV(WDIE) | _BV(WDP3) | _BV(WDP0);
  sei();
}


// Channel assignment retained from the original sketch:
//   CH1 = original thermostat side
//   CH2 = original valve side
static const uint8_t THERM_CHANNEL = 1;
static const uint8_t VALVE_CHANNEL = 2;

// BidCoS protocol constants used by HM-CC-TC / HM-CC-VD.
static const uint8_t TYPE_RESPONSE      = 0x02;
static const uint8_t TYPE_VALVE_RUNTIME = 0x10;
static const uint8_t TYPE_CLIMATE_EVENT = 0x58;
static const uint8_t TYPE_WEATHER_EVENT = 0x70;

static const uint8_t RESPONSE_ACK_STATUS = 0x01;
static const uint8_t VD_CHANNEL          = 0x01;

static const uint8_t CMD_REFRESH     = 0x00;
static const uint8_t CMD_VENT_OPEN   = 0x01; // likely VENT_OPEN; handled transparently regardless
static const uint8_t CMD_VENT_CLOSED = 0x02;
static const uint8_t CMD_NEW_TARGET  = 0x03;
static const uint8_t CMD_DECALC      = 0x04;

// CTRL A2 = RPTEN | BIDI | WKMEUP. Deliberately NO BURST.
static const uint8_t CTRL_TC_TO_VD = 0xA2;

// Measured with the real HM-CC-VD: ACK_STATUS arrived about 130 ms after the
// Link-B transmission. Keep a 300 ms asynchronous receive window as margin.
// Expiry NEVER causes an immediate retransmission.
static const uint16_t VD_ACK_TIMEOUT_MS = 300;

// The CC1101 can be unreliable when switching RX->TX immediately after a
// received packet. Older diagnostic builds accidentally added a little delay
// through serial output before the VD runtime ACK. Make that timing explicit.
static const uint16_t VD_RUNTIME_ACK_DELAY_MS = 15;

// Never allow an AVR SPI access to block forever waiting for CC1101 MISO.
// A radio fault may lose one telegram, but it must not freeze the whole HY.
static const uint16_t CC1101_MISO_TIMEOUT_US = 2000;
static const uint16_t AVR_SPI_TRANSFER_TIMEOUT_US = 2000;

// A VD is considered unreachable only after five minutes without any valid
// telegram from its configured peer. Link B keeps running for reacquisition.
static const uint32_t VD_COMM_LOST_MS = 10UL * 60UL * 1000UL;

// CC1101 must normally return to RX within a few milliseconds after TX. Do not
// let a bad state build up until the 8 s application watchdog: actively recover
// the radio after a short bounded wait.
// Allow up to ~80 ms for the complete on-air packet plus the normal
// TX->RX transition. The loop exits immediately when MARCSTATE_RX is seen,
// so this does not add latency to normal short packets. 20 ms was too tight
// for longer BidCoS frames (e.g. DeviceInfo/pairing traffic).
static const uint16_t CC1101_RX_WAIT_POLLS = 400;
static const uint16_t CC1101_RX_WAIT_STEP_US = 200;
static const uint8_t  CC1101_RX_RECOVERY_POLLS = 100;

// Unsolicited CCU status is not timing critical. Keep it away from freshly
// received TC/VD traffic and the CC1101 RX->TX corner case.
static const uint16_t CCU_STATUS_RADIO_QUIET_MS = 600;

// Link-B timebase calibration measured on this 8 MHz Pro Mini.
// A nominal 122500 ms interval took about 122918 ms in real time.
// Shorten millis()-based Link-B intervals by 0.9966.
static const uint16_t LINKB_TIME_SCALE_NUM = 9966;
static const uint16_t LINKB_TIME_SCALE_DEN = 10000;

static uint32_t calibrateLinkBInterval(uint32_t ms) {
  return (ms * (uint32_t)LINKB_TIME_SCALE_NUM)
       / (uint32_t)LINKB_TIME_SCALE_DEN;
}

// Link-A timing measured directly on CC1101 GDO0 against a real TC/VD pair.
// A 95 ms software delay places HY's ACK in the receive window in which the
// original HM-CC-VD answers. This timing is required by the real HM-CC-TC.
static const uint16_t LINK_A_RESPONSE_DELAY_MS = 95;

// The custom radio disables AskSin++'s global send delay for Link A/B.
// Keep the standard reply delay explicitly for pairing and CCU configuration.
static const uint16_t CONFIG_RESPONSE_DELAY_MS = 100;

// Shared multi-HY scheduling / pairing policy.
static const uint32_t PAIRING_SESSION_MS       = 10UL * 60UL * 1000UL;
static const uint32_t PAIR_REANNOUNCE_MS       = 2000UL;
static const uint32_t PAIR_ATTEMPT_TIMEOUT_MS  = 22000UL;
static const uint32_t PAIR_FINISH_QUIET_MS     = 500UL;

// Async CCU status delivery replaces AskSin++'s blocking 600 ms waitAck().
// We still retry, but the shared RX dispatcher keeps running while waiting.
static const uint16_t CCU_STATUS_ACK_TIMEOUT_MS = 650;

// While a Link-B transmission is waiting for its VD ACK, another virtual HY
// must not transmit to its VD. This is also the look-ahead collision window.
static const uint16_t LINKB_COLLISION_GUARD_MS = VD_ACK_TIMEOUT_MS;

// Spread initial Link-B message counters over the 8-bit counter space.
// For 20 HYs this yields 0,13,26,...,247 and avoids identical start phases.
static const uint8_t LINKB_COUNTER_STEP = 13;

// HY keeps its own two-channel device identity for the CCU. Link A still
// behaves protocol-wise like the valve side towards the real thermostat.
const struct DeviceInfo PROGMEM devinfo1 = {
  {0xfe,0x01,0x01},
  "SRUE000001",
  {0xfe,0x01},
  0x11,
  0xfe,
  {0x01,0x00}
};

const struct DeviceInfo PROGMEM devinfo2 = {
  {0xfe,0x01,0x02},
  "SRUE000002",
  {0xfe,0x01},
  0x11,
  0xfe,
  {0x01,0x00}
};

// Drop-in AVR SPI implementation with a bounded CC1101-ready wait.
// AskSin++ AvrSPI::waitMiso() waits forever. With one shared radio that would
// stop every virtual HY if the CC1101 ever fails to pull MISO low.
template <uint8_t CS,uint8_t MOSI,uint8_t MISO,uint8_t SCLK,
          class PINTYPE=ArduinoPins>
class HySafeAvrSPI {
private:
  bool waitMisoReady() {
    for (uint16_t us = 0; us < CC1101_MISO_TIMEOUT_US; ++us) {
      if (!PINTYPE::getState(MISO)) return true;
      _delay_us(1);
    }
    hySpiFaultCode = 1;
    return false;
  }

public:
  uint8_t send(uint8_t data) {
    SPDR = data;

    for (uint16_t us = 0; us < AVR_SPI_TRANSFER_TIMEOUT_US; ++us) {
      if (SPSR & _BV(SPIF)) {
        return SPDR;
      }
      _delay_us(1);
    }

    hySpiFaultCode = 2;

    // Do not let one failed byte transfer freeze all virtual HY devices.
    // Re-arm the AVR SPI master. The surrounding CC1101 operation will then
    // fail/timeout normally and a later transaction gets a clean SPI engine.
    SPCR = 0;
    SPSR = 0;
    SPCR = _BV(SPE) | _BV(MSTR);
    return 0xff;
  }

  void init() {
    PINTYPE::setOutput(CS);
    PINTYPE::setOutput(MOSI);
    PINTYPE::setInput(MISO);
    PINTYPE::setOutput(SCLK);
    SPCR = _BV(SPE) | _BV(MSTR);
    PINTYPE::setHigh(CS);
    PINTYPE::setHigh(SCLK);
    PINTYPE::setLow(MOSI);
  }

  void shutdown() {
    PINTYPE::setInput(CS);
    PINTYPE::setInput(MOSI);
    PINTYPE::setInput(SCLK);
  }

  void select()   { PINTYPE::setLow(CS); }
  void deselect() { PINTYPE::setHigh(CS); }

  // Compatibility with AskSin++ CC1101::reset(), which calls spi.waitMiso()
  // directly. Keep the original void signature, but use our bounded wait
  // internally so a stuck CC1101 cannot freeze the whole controller.
  void waitMiso() {
    waitMisoReady();
  }

  void ping() {
    select();
    waitMisoReady();
    deselect();
  }

  uint8_t strobe(uint8_t cmd) {
    select();
    if (!waitMisoReady()) {
      deselect();
      return 0xff;
    }
    uint8_t ret = send(cmd);
    deselect();
    return ret;
  }

  void readBurst(uint8_t* buf, uint8_t regAddr, uint8_t len) {
    select();
    if (!waitMisoReady()) {
      deselect();
      memset(buf, 0, len);
      return;
    }
    send(regAddr | READ_BURST);
    for (uint8_t i = 0; i < len; ++i) buf[i] = send(0x00);
    deselect();
  }

  void writeBurst(uint8_t regAddr, uint8_t* buf, uint8_t len) {
    select();
    if (!waitMisoReady()) {
      deselect();
      return;
    }
    send(regAddr | WRITE_BURST);
    for (uint8_t i = 0; i < len; ++i) send(buf[i]);
    deselect();
  }

  uint8_t readReg(uint8_t regAddr, uint8_t regType) {
    select();
    if (!waitMisoReady()) {
      deselect();
      return 0xff;
    }
    send(regAddr | regType);
    uint8_t val = send(0x00);
    deselect();
    return val;
  }

  void writeReg(uint8_t regAddr, uint8_t val) {
    select();
    if (!waitMisoReady()) {
      deselect();
      return;
    }
    send(regAddr);
    send(val);
    deselect();
  }
};

typedef HySafeAvrSPI<10,11,12,13> RadioSPI;

// Expose only CC1101 status information needed by the Link-B RX diagnostic.
// Normal AskSin++ RX/TX behavior remains unchanged.
template <class SPIType, uint8_t PWRPIN = 0xff>
class HyCC1101 : public CC1101<SPIType,PWRPIN> {
private:
  bool waitForRxShort(uint16_t polls, uint8_t& lastState) {
    for (uint16_t i = 0; i < polls; ++i) {
      lastState = this->spi.readReg(CC1101_MARCSTATE, CC1101_STATUS);
      if (lastState == MARCSTATE_RX) return true;
      if (hySpiFaultCode != 0) return false;
      _delay_us(CC1101_RX_WAIT_STEP_US);
    }
    return false;
  }

  bool recoverRxAfterTx(uint8_t firstState) {
    uint8_t fault = hySpiFaultCode;
    hySpiFaultCode = 0;

    Serial.print(F("R RX ")); Serial.print(firstState, HEX);
    Serial.print(' '); Serial.println(fault);

    hyRadioStage = 0x39;
    this->spi.strobe(CC1101_SIDLE);
    _delay_us(100);
    this->spi.strobe(CC1101_SFTX);
    this->spi.strobe(CC1101_SFRX);
    this->spi.strobe(CC1101_SRX);

    uint8_t state = 0xff;
    if (waitForRxShort(CC1101_RX_RECOVERY_POLLS, state)) {
      hySpiFaultCode = 0;
      Serial.println(F("R OK"));
      return false; // current TX is uncertain/lost
    }

    // Do not carry a broken physical radio through the main loop.
    // Re-enter the application immediately; setup() performs the full HAL/CC1101
    // initialization while the .noinit Link-B state survives.
    Serial.println(F("R FAIL"));
    cli();
    hyRecoveredRadioStage = 0x3A;
    hyWatchdogOldMillis = millis();
    hySoftWatchdogMagic = HY_SOFT_WDT_MAGIC;
    asm volatile("jmp 0");
    __builtin_unreachable();
  }

  bool waitRxWithRecovery() {
    uint8_t state = 0xff;

    // MCSM1=0x03 means:
    //   TXOFF_MODE=11 -> RX after TX
    //   RXOFF_MODE=00 -> IDLE after a received packet
    //
    // Therefore IDLE seen while waiting for post-TX RX is not necessarily a
    // radio fault: the CC1101 may already have received another packet and
    // deliberately returned to IDLE. Preserve RXFIFO and simply re-arm RX.
    for (uint16_t i = 0; i < CC1101_RX_WAIT_POLLS; ++i) {
      state = this->spi.readReg(CC1101_MARCSTATE, CC1101_STATUS);
      if (state == MARCSTATE_RX) {
        hySpiFaultCode = 0;
        return true;
      }
      if (hySpiFaultCode != 0) break;

      if (state == MARCSTATE_IDLE) {
        uint8_t rxBytes = this->spi.readReg(CC1101_RXBYTES, CC1101_STATUS);
        uint8_t mcsm1   = this->spi.readReg(CC1101_MCSM1, CC1101_CONFIG);

        // Do NOT flush RXFIFO here. If IDLE was caused by a just-received
        // packet, the shared dispatcher must still be able to read it.
        this->spi.strobe(CC1101_SRX);

        uint8_t after = 0xff;
        if (waitForRxShort(CC1101_RX_RECOVERY_POLLS, after)) {
          hySpiFaultCode = 0;
          Serial.print(F("R I "));
          Serial.print(rxBytes, HEX);
          Serial.print(' ');
          Serial.println(mcsm1, HEX);
          return true;
        }

        state = after;
        break;
      }

      _delay_us(CC1101_RX_WAIT_STEP_US);
    }

    return recoverRxAfterTx(state);
  }

public:
  void configureRxBandwidth() {
    this->spi.strobe(CC1101_SIDLE);
    _delay_ms(1);
    const uint8_t mdmcfg4 = this->spi.readReg(CC1101_MDMCFG4, CC1101_CONFIG);
    this->spi.writeReg(CC1101_MDMCFG4, (mdmcfg4 & 0x0f) | 0x80);
    this->spi.strobe(CC1101_SRX);
    hySpiFaultCode = 0;
  }

protected:
  uint8_t sndData(uint8_t *buf, uint8_t size, uint8_t burst) {
    hySpiFaultCode = 0;

    hyRadioStage = 0x31;
    this->spi.strobe(CC1101_SIDLE);
    hyRadioStage = 0x32;
    this->spi.strobe(CC1101_SFTX);
    hyRadioStage = 0x33;
    this->spi.strobe(CC1101_STX);

    hyRadioStage = 0x34;
    uint8_t i = 100;
    uint8_t state = 0xff;
    while (i-- != 0) {
      state = this->spi.readReg(CC1101_MARCSTATE, CC1101_STATUS);
      if (state == MARCSTATE_TX) break;
      if (hySpiFaultCode != 0) break;
      _delay_us(100);
    }

    if (state != MARCSTATE_TX) {
      Serial.print(F("R TX ")); Serial.println(state, HEX);
      hyRadioStage = 0x35;
      recoverRxAfterTx(state);
      hyRadioStage = 0;
      return false;
    }

    hyRadioStage = 0x36;
    _delay_ms(10);
    if (burst) _delay_ms(350);

    hyRadioStage = 0x37;
    this->spi.writeReg(CC1101_TXFIFO, size);
    this->spi.writeBurst(CC1101_TXFIFO | WRITE_BURST, buf, size);
    if (hySpiFaultCode != 0) {
      recoverRxAfterTx(0xff);
      hyRadioStage = 0;
      return false;
    }

    hyRadioStage = 0x38;
    bool result = waitRxWithRecovery();
    hyRadioStage = 0;
    return result;
  }
};

typedef HyCC1101<RadioSPI,0xff> HyRadioChip;

class HyRadio : public Radio<RadioSPI,2,0xff,0,HyRadioChip> {
public:
  typedef Radio<RadioSPI,2,0xff,0,HyRadioChip> BaseRadio;

  bool write(const Message& msg, uint8_t burst) {
    // 0x20 means the hang happened in the generic Radio wrapper before
    // HyCC1101::sndData() was reached (e.g. send-timeout/wakeup/IRQ handling).
    hyRadioStage = 0x20;
    bool result = BaseRadio::write(msg, burst);
    hyRadioStage = 0;
    return result;
  }
};

// SENDDELAY is set to 0 deliberately. The generic AskSin++ Radio defaults to a
// 100 ms minimum interval between transmissions. HY must be able to answer a
// TC immediately even if Link B happened to transmit shortly before.
typedef AskSin<StatusLed<LED_PIN>,BatterySensor,HyRadio> Hal;

static bool timeReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

static uint8_t clampU8(uint16_t value, uint8_t maximum) {
  return value > maximum ? maximum : (uint8_t)value;
}

// TYPE=0x58 target scale:
//   percent = raw / 2.56 = raw * 100 / 256
// Return percent * 100 to avoid floating point.
static uint16_t decodeTcTargetPercent100(uint8_t raw) {
  return (uint16_t)(((uint32_t)raw * 10000UL + 128UL) / 256UL);
}

static uint8_t encodeTcTargetPercent100(uint16_t percent100) {
  if (percent100 > 10000U) percent100 = 10000U;
  uint32_t raw = ((uint32_t)percent100 * 256UL + 5000UL) / 10000UL;
  return clampU8((uint16_t)raw, 0xff);
}

// ACK_STATUS position scale:
//   percent = raw / 2
// Therefore percent*100 = raw*50 exactly.
static uint16_t decodeVdPositionPercent100(uint8_t raw) {
  return (uint16_t)raw * 50U;
}

static uint8_t encodeVdPositionPercent100(uint16_t percent100) {
  if (percent100 > 10000U) percent100 = 10000U;
  // raw = percent*2 = percent100 / 50
  return clampU8((uint16_t)((percent100 + 25U) / 50U), 200U);
}

static bool isBroadcastHMID(const HMID& id) {
  return id.id0() == 0x00 && id.id1() == 0x00 && id.id2() == 0x00;
}

static void printHMIDValue(const HMID& id) {
  DHEX(id.id0()); DHEX(id.id1()); DHEX(id.id2());
}

static void dumpMsg(const __FlashStringHelper* tag, const Message& msg) {
  DPRINT(tag);
  DPRINT(F(" len=0x")); DHEX(msg.length());
  DPRINT(F(" cnt=0x")); DHEX(msg.count());
  DPRINT(F(" flags=0x")); DHEX(msg.flags());
  DPRINT(F(" type=0x")); DHEX(msg.type());
  DPRINT(F(" from="));
  printHMIDValue(msg.from());
  DPRINT(F(" to="));
  printHMIDValue(msg.to());
  DPRINT(F(" raw: "));
  for (uint8_t i = 0; i < msg.length(); i++) {
    uint8_t b = msg.buffer()[i];
    if (b < 0x10) DPRINT('0');
    DHEX(b);
    DPRINT(' ');
  }
  DPRINTLN(F(""));
}

DEFREGISTER(Reg0,DREG_INTKEY,DREG_LEDMODE,MASTERID_REGS,DREG_LOWBATLIMIT)
class SwList0 : public RegList0<Reg0> {
public:
  SwList0(uint16_t addr) : RegList0<Reg0>(addr) {}
  void defaults () {
    clear();
    lowBatLimit(22);
  }
};

// Keep the original two List1 registers and their storage layout unchanged.
DEFREGISTER(Reg1,0x01,0x02)
class SwList1 : public RegList1<Reg1> {
public:
  SwList1 (uint16_t addr) : RegList1<Reg1>(addr) {}

  bool enable () const { return this->readRegister(0x01,0); }
  bool enable (bool v) const { return this->writeRegister(0x01,v); }

  // Compatibility semantics:
  //   100 = pass 100 % = x 0 % reduction
  //    80 = pass  80 % = x 20 % reduction
  uint8_t factor () const { return this->readRegister(0x02,0); }
  bool factor (uint8_t v) const { return this->writeRegister(0x02,v); }

  void defaults () {
    clear();
    enable(true);
    factor(100);
  }
};

DEFREGISTER(Reg3)
class ProxyList3 : public RegList3<Reg3> {
public:
  ProxyList3 (uint16_t addr) : RegList3<Reg3>(addr) {}
  void defaults () { clear(); }

  bool single () const { return false; }
  bool odd () const { return false; }
  bool even () const { return false; }

  bool single (bool) { return true; }
  bool odd (bool) { return true; }
  bool even (bool) { return true; }
};

DEFREGISTER(Reg4)
class ProxyList4 : public RegList4<Reg4> {
public:
  ProxyList4 (uint16_t addr) : RegList4<Reg4>(addr) {}
  void defaults () { clear(); }
};

class ProxyChannel : public Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t runtimeStatus;
  uint8_t runtimeFlags;
  bool    runtimeValid;

public:
  typedef Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ProxyChannel () : BaseChannel(), runtimeStatus(0), runtimeFlags(0), runtimeValid(false) {}
  virtual ~ProxyChannel () {}

  // AskSin++ uses these values for CONFIG_STATUS_REQUEST replies. Our shared
  // multi-HY loop also uses them for asynchronous INFO_ACTUATOR_STATUS updates
  // to the paired master/CCU.
  uint8_t status () const { return runtimeStatus; }
  uint8_t flags  () const { return runtimeFlags; }

  void runtimeState(uint8_t stat, uint8_t flg) {
    bool changedState = (runtimeValid == false)
                     || (runtimeStatus != stat)
                     || (runtimeFlags != flg);
    runtimeStatus = stat;
    runtimeFlags  = flg;
    runtimeValid  = true;

    if (changedState) {
      // The shared multi-HY loop will turn this into an asynchronous
      // INFO_ACTUATOR_STATUS (TYPE=0x10, subtype 0x06) to MasterID.
      this->changed(true);
    }
  }

  bool hasRuntimeState() const { return runtimeValid; }

  void configChanged() {
    uint8_t factor = this->getList1().factor();
    if (factor > 100) factor = 100;

    DPRINT(F("ConfigChanged CH"));
    DPRINT(this->number());
    DPRINT(F(" enable="));
    DPRINT(this->getList1().enable());
    DPRINT(F(" factor="));
    DPRINT(factor);
    DPRINT(F(" reduction="));
    DPRINT(100U - factor);
    DPRINTLN(F("%"));
  }
};

enum HyMode : uint8_t {
  HY_NORMAL = 0,
  HY_FULL_OPEN,
  HY_VENT_CLOSED,
  HY_OTHER_SPECIAL
};

template <uint8_t ChannelCount>
class ProxyDevice : public MultiChannelDevice<Hal,ProxyChannel,ChannelCount,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ProxyChannel,ChannelCount,SwList0> DevType;

private:
  // Link A / requested state from the real TC.
  bool     haveTcTarget;
  uint8_t  currentTcCommand;
  uint8_t  currentTcTargetRaw;

  // Transformation state.
  HyMode   specialMode;
  uint8_t  desiredVdTargetRaw;
  uint8_t  lastEffectiveFactor;

  // Link B state - completely independent from the real TC counter.
  uint8_t  hyTcCounter;
  bool     vdScheduleStarted;
  uint32_t nextVdBaseMs;       // exact next Link-B TX slot
  bool     lastSentWasNormal;
  uint8_t  lastSentVdTargetRaw;

  bool     awaitingVdAck;
  uint8_t  awaitingVdCounter;
  uint32_t vdAckDeadlineMs;
  uint8_t  missCount;

  // Fairness state for the shared Link-B scheduler. A skipped slot increments
  // the debt. The debt remains until this HY has again completed a real Link-B
  // exchange and received the matching VD ACK_STATUS. Then the VD is known to
  // be back in sync and all accumulated collision debt is cleared.
  uint8_t  collisionDebt;

  // Index into the .noinit recovery snapshot array.
  uint8_t  recoveryIndex;

  // VD reachability watchdog. lastVdSeenMs doubles as the initial grace timer.
  bool     vdLost;

  // Non-blocking delivery of unsolicited INFO_ACTUATOR_STATUS to the CCU.
  // AskSin++ Device::send() normally waits synchronously up to 600 ms for the
  // ACK and consumes unrelated radio frames while doing so. With multiple
  // virtual devices that is unacceptable, so status ACK/retry is asynchronous.
  Message  pendingCcuStatusMsg;
  bool     ccuStatusPending;
  uint8_t  ccuStatusRetriesLeft;
  uint8_t  ccuStatusChannel;
  uint32_t ccuStatusAckDeadlineMs;

  // Cache of the most recent real VD ACK_STATUS.
  bool     vdCacheValid;
  uint8_t  lastVdPositionRaw;  // scale /2
  uint8_t  lastVdStatus;
  uint8_t  lastVdRssi;
  uint32_t lastVdSeenMs;

public:
  ProxyDevice(const DeviceInfo& i, uint16_t addr, uint8_t initialLinkBCounter, uint8_t recoverySlot)
    : DevType(i,addr),
      haveTcTarget(false),
      currentTcCommand(CMD_REFRESH),
      currentTcTargetRaw(0),
      specialMode(HY_NORMAL),
      desiredVdTargetRaw(0),
      lastEffectiveFactor(100),
      hyTcCounter(initialLinkBCounter),
      vdScheduleStarted(false),
      nextVdBaseMs(0),
      lastSentWasNormal(false),
      lastSentVdTargetRaw(0),
      awaitingVdAck(false),
      awaitingVdCounter(0),
      vdAckDeadlineMs(0),
      missCount(0),
      collisionDebt(0),
      recoveryIndex(recoverySlot),
      vdLost(false),
      ccuStatusPending(false),
      ccuStatusRetriesLeft(0),
      ccuStatusChannel(0),
      ccuStatusAckDeadlineMs(0),
      vdCacheValid(false),
      lastVdPositionRaw(0),
      lastVdStatus(0),
      lastVdRssi(0),
      lastVdSeenMs(0) {}

  virtual ~ProxyDevice() {}

  void saveRecoveryState() {
    if (recoveryIndex >= HY_RECOVERY_DEVICE_COUNT) return;
    HyRecoveryState& s = hyRecoveryStates[recoveryIndex];

    s.magic = 0; // two-phase commit
    uint8_t flags = 0;
    if (vdScheduleStarted) flags |= 0x01;
    if (haveTcTarget)      flags |= 0x02;
    if (lastSentWasNormal) flags |= 0x04;
    if (vdCacheValid)      flags |= 0x08;
    flags |= ((uint8_t)specialMode & 0x03) << 4;

    s.flags = flags;
    s.hyTcCounter = hyTcCounter;
    s.currentTcCommand = currentTcCommand;
    s.currentTcTargetRaw = currentTcTargetRaw;
    s.desiredVdTargetRaw = desiredVdTargetRaw;
    s.lastSentVdTargetRaw = lastSentVdTargetRaw;
    s.collisionDebt = collisionDebt;
    s.lastVdPositionRaw = lastVdPositionRaw;
    s.lastVdStatus = lastVdStatus;
    s.nextVdBaseMs = nextVdBaseMs;
    s.magic = HY_RECOVERY_STATE_MAGIC;
  }

  bool restoreRecoveryState(uint32_t oldMillis) {
    if (recoveryIndex >= HY_RECOVERY_DEVICE_COUNT) return false;
    HyRecoveryState& s = hyRecoveryStates[recoveryIndex];
    if (s.magic != HY_RECOVERY_STATE_MAGIC) return false;

    vdScheduleStarted = (s.flags & 0x01) != 0;
    haveTcTarget      = (s.flags & 0x02) != 0;
    lastSentWasNormal = (s.flags & 0x04) != 0;
    vdCacheValid      = (s.flags & 0x08) != 0;
    specialMode       = (HyMode)((s.flags >> 4) & 0x03);

    hyTcCounter = s.hyTcCounter;
    currentTcCommand = s.currentTcCommand;
    currentTcTargetRaw = s.currentTcTargetRaw;
    desiredVdTargetRaw = s.desiredVdTargetRaw;
    lastSentVdTargetRaw = s.lastSentVdTargetRaw;
    collisionDebt = s.collisionDebt;
    lastVdPositionRaw = s.lastVdPositionRaw;
    lastVdStatus = s.lastVdStatus;
    lastEffectiveFactor = effectiveFactor();

    awaitingVdAck = false;
    ccuStatusPending = false;
    ccuStatusRetriesLeft = 0;

    if (vdScheduleStarted) {
      const uint32_t now = millis();
      int32_t deadline = (int32_t)(s.nextVdBaseMs - oldMillis);
      HMID me;
      this->getDeviceID(me);
      while (deadline <= (int32_t)now) {
        deadline += (int32_t)calibrateLinkBInterval(linkBIntervalMs(me, hyTcCounter));
        ++hyTcCounter;
      }
      nextVdBaseMs = (uint32_t)deadline;
    }
    else {
      nextVdBaseMs = 0;
    }

    // VD reachability gets a fresh 10-minute grace period after application
    // recovery. Link-B synchronization itself is preserved above.
    lastVdSeenMs = millis();
    vdLost = false;

    if (vdCacheValid) updateLinkAChannelStateFromCache();

    Serial.print(F("W")); Serial.print(recoveryIndex + 1);
    Serial.print(' '); Serial.print(hyTcCounter, HEX);
    Serial.print(' '); Serial.println(vdScheduleStarted ? nextVdBaseMs : 0UL);

    saveRecoveryState();
    return true;
  }

  void noteValidVdRx() {
    lastVdSeenMs = millis();
    if (vdLost) {
      vdLost = false;
      Serial.print(F("V")); Serial.print(recoveryIndex + 1);
      Serial.println(F(" OK"));
    }
  }

  void serviceVdWatchdog(uint32_t now) {
    HMID valve;
    if (getFirstPeerOfChannel(VALVE_CHANNEL, valve) == false) return;

    if (lastVdSeenMs == 0) lastVdSeenMs = now;
    if (!vdLost && (uint32_t)(now - lastVdSeenMs) >= VD_COMM_LOST_MS) {
      vdLost = true;
      Serial.print(F("V")); Serial.print(recoveryIndex + 1);
      Serial.println(F(" LOST"));
    }
  }

  void configureRadio() {
    this->radio().configureRxBandwidth();
    DPRINTLN(F("CC1101 RX bandwidth: ~203.1 kHz (MDMCFG4 high nibble=0x8), FSCTRL0 unchanged"));
  }

  bool isAwaitingVdAck() const {
    return awaitingVdAck;
  }

  bool hasPendingCcuStatus() const {
    return ccuStatusPending;
  }

  bool linkBScheduleActive() const {
    return vdScheduleStarted;
  }

  bool isLinkBDue(uint32_t now) const {
    return vdScheduleStarted && timeReached(now, nextVdBaseMs);
  }

  uint32_t linkBSlotTime() const {
    return nextVdBaseMs;
  }

  uint8_t linkBCollisionDebt() const {
    return collisionDebt;
  }

  uint8_t linkBCounter() const {
    return hyTcCounter;
  }

  // True if this HY has a Link-B slot in [now, now+windowMs]. A slot already
  // due also counts. Used to keep low-priority CCU status traffic away from the
  // timing-sensitive VD window.
  bool linkBSlotWithin(uint32_t now, uint16_t windowMs) const {
    if (!vdScheduleStarted) return false;
    if (timeReached(now, nextVdBaseMs)) return true;
    return (uint32_t)(nextVdBaseMs - now) <= (uint32_t)windowMs;
  }

  bool handleAsyncCcuStatusAck(Message& msg) {
    if (!ccuStatusPending) return false;

    HMID master = this->getMasterID();
    HMID me;
    this->getDeviceID(me);

    if (msg.to() != me || msg.from() != master ||
        msg.count() != pendingCcuStatusMsg.count()) {
      return false;
    }

    if (msg.isAck()) {
      DPRINT(F("CCU STATUS ACK cnt=0x")); DHEX(msg.count());
      DPRINTLN(F(" received asynchronously"));
      ccuStatusPending = false;
      ccuStatusRetriesLeft = 0;
      return true;
    }

    if (msg.isNack()) {
      DPRINT(F("CCU STATUS NACK cnt=0x")); DHEX(msg.count());
      DPRINTLN(F(" - status will be generated again"));
      ccuStatusPending = false;
      ccuStatusRetriesLeft = 0;
      if (ccuStatusChannel >= 1 && ccuStatusChannel <= this->channels()) {
        this->channel(ccuStatusChannel).changed(true);
      }
      return true;
    }

    return false;
  }

  bool transmitPendingCcuStatus() {
    if (!ccuStatusPending || ccuStatusRetriesLeft == 0) return false;

    DPRINT(F("TX HY->CCU INFO_ACTUATOR_STATUS async BEGIN cnt=0x"));
    DHEX(pendingCcuStatusMsg.count());
    DPRINT(F(" ch=")); DPRINT(ccuStatusChannel);
    DPRINT(F(" triesLeft=")); DPRINTLN(ccuStatusRetriesLeft);

    bool sent = this->radio().write(pendingCcuStatusMsg,
                                    pendingCcuStatusMsg.burstRequired());

    DPRINT(F("TX HY->CCU INFO_ACTUATOR_STATUS async END sent=")); DPRINT(sent);
    DPRINT(F(" cnt=0x")); DHEX(pendingCcuStatusMsg.count());
    DPRINT(F(" ch=")); DPRINT(ccuStatusChannel);
    DPRINT(F(" triesLeftAfter=")); DPRINT(ccuStatusRetriesLeft - 1);
    DPRINTLN(F(""));

    --ccuStatusRetriesLeft;
    ccuStatusAckDeadlineMs = millis() + CCU_STATUS_ACK_TIMEOUT_MS;
    return sent;
  }

  // Called from the shared loop. No radio.read() happens here, so unrelated
  // TC/VD/other-HY frames remain available to the common dispatcher.
  void serviceAsyncCcuStatus(bool allowTx) {
    if (!ccuStatusPending) return;
    if (!timeReached(millis(), ccuStatusAckDeadlineMs)) return;
    if (!allowTx) return;

    if (ccuStatusRetriesLeft > 0) {
      DPRINT(F("CCU STATUS ACK timeout cnt=0x"));
      DHEX(pendingCcuStatusMsg.count());
      DPRINTLN(F(" -> async retry"));
      transmitPendingCcuStatus();
      return;
    }

    DPRINT(F("CCU STATUS delivery failed cnt=0x"));
    DHEX(pendingCcuStatusMsg.count());
    DPRINTLN(F(" - queue current channel state again"));
    ccuStatusPending = false;
    if (ccuStatusChannel >= 1 && ccuStatusChannel <= this->channels()) {
      this->channel(ccuStatusChannel).changed(true);
    }
  }

  // Queue at most one unsolicited status update per logical HY. A state change
  // that happens while this message is pending sets changed(true) again and is
  // sent after the pending transaction completes.
  bool serviceChangedChannelsAsync(bool allowTx) {
    if (ccuStatusPending || !allowTx) return false;

    HMID master = this->getMasterID();
    if (!master.valid()) {
      // Nothing to notify before pairing. Current state remains available via
      // CONFIG_STATUS_REQUEST once a master exists.
      for (uint8_t i = 1; i <= this->channels(); ++i) {
        if (this->channel(i).changed()) this->channel(i).changed(false);
      }
      return false;
    }

    for (uint8_t i = 1; i <= this->channels(); ++i) {
      ProxyChannel& ch = this->channel(i);
      if (!ch.changed()) continue;

      // AskSin++ may leave a channel marked changed during initialization or
      // configuration loading. Do not emit an unsolicited actuator status
      // until this HY has an actual runtime state derived from the real VD.
      // Otherwise we can start a radio TX immediately after boot with a
      // synthetic 0/0 status before the shared RX dispatcher ever gets a turn.
      if (!ch.hasRuntimeState()) {
        ch.changed(false);
        DPRINT(F("CCU STATUS: ignore startup changed flag CH"));
        DPRINTLN(i);
        continue;
      }

      const uint8_t count = this->nextcount();
      InfoActuatorStatusMsg& pm = pendingCcuStatusMsg.infoActuatorStatus();
      pm.init(count, ch, this->radio().rssi());
      ch.patchStatus(pendingCcuStatusMsg);
      pendingCcuStatusMsg.to(master);
      this->getDeviceID(pendingCcuStatusMsg.from());
      pendingCcuStatusMsg.setRpten();
      this->getHal().prepareSend(pendingCcuStatusMsg);

      ccuStatusPending = true;
      ccuStatusChannel = i;
      ccuStatusRetriesLeft = this->getList0().transmitDevTryMax();
      if (ccuStatusRetriesLeft == 0) ccuStatusRetriesLeft = 1;

      // This exact state is now represented by pendingCcuStatusMsg. If the
      // state changes again, runtimeState() will set changed(true) anew.
      ch.changed(false);
      transmitPendingCcuStatus();
      return true;
    }
    return false;
  }

  bool getFirstPeerOfChannel(uint8_t channelNo, HMID& peerid) {
    if (channelNo < 1 || channelNo > ChannelCount) return false;
    for (uint8_t i = 0; i < this->channel(channelNo).peers(); ++i) {
      Peer p = this->channel(channelNo).peerat(i);
      if (p.valid()) {
        peerid = p;
        return true;
      }
    }
    return false;
  }

  bool isFromChannelPeer(uint8_t channelNo, const HMID& sender) {
    if (channelNo < 1 || channelNo > ChannelCount) return false;
    uint8_t pidx = this->channel(channelNo).peerfor(sender);
    if (pidx >= this->channel(channelNo).peers()) return false;
    Peer p = this->channel(channelNo).peerat(pidx);
    return p.valid();
  }

  uint8_t effectiveFactor() {
    SwList1 l1 = this->channel(THERM_CHANNEL).getList1();
    if (l1.enable() == false) return 100;
    uint8_t factor = l1.factor();
    return factor > 100 ? 100 : factor;
  }

  uint8_t reductionPercent() {
    return (uint8_t)(100U - effectiveFactor());
  }

  // Explicit percent-domain transformation for TYPE=0x58 target values.
  uint8_t scaledTargetForVd(uint8_t tcRaw, uint8_t factor) const {
    uint16_t tcPercent100 = decodeTcTargetPercent100(tcRaw);
    uint16_t vdPercent100 = (uint16_t)(((uint32_t)tcPercent100 * factor + 50UL) / 100UL);
    return encodeTcTargetPercent100(vdPercent100);
  }

  // Inverse transform for the actual position returned in ACK_STATUS.
  uint8_t tcVisiblePositionRaw() {
    if (vdCacheValid == false) return 0;

    if (specialMode != HY_NORMAL) {
      return lastVdPositionRaw;
    }

    uint8_t factor = effectiveFactor();
    if (factor == 0) {
      // The inverse of a 0 factor is undefined. Keep the behavior specified in
      // the architecture: clamp the TC-visible value to 100 %.
      return 200;
    }

    uint16_t vdPercent100 = decodeVdPositionPercent100(lastVdPositionRaw);
    uint32_t tcPercent100 = ((uint32_t)vdPercent100 * 100UL + factor / 2U) / factor;
    if (tcPercent100 > 10000UL) tcPercent100 = 10000UL;
    return encodeVdPositionPercent100((uint16_t)tcPercent100);
  }

  // Mirror the cached real-VD state into the HY-VD channel (CH1).
  // This serves two purposes:
  //   1. CONFIG_STATUS_REQUEST from the CCU returns the real proxy state.
  //   2. a changed state triggers AskSin++ INFO_ACTUATOR_STATUS to MasterID.
  void updateLinkAChannelStateFromCache() {
    if (vdCacheValid == false) return;

    const uint8_t visiblePosition = tcVisiblePositionRaw();
    this->channel(THERM_CHANNEL).runtimeState(visiblePosition, lastVdStatus);

    DPRINT(F("  HY-VD shadow state posRaw=")); DPRINT(visiblePosition);
    DPRINT(F(" pos=")); DPRINT(decodeVdPositionPercent100(visiblePosition) / 100U);
    DPRINT(F("."));
    uint8_t frac = (uint8_t)(decodeVdPositionPercent100(visiblePosition) % 100U);
    if (frac < 10) DPRINT('0');
    DPRINT(frac);
    DPRINT(F("% status=0x")); DHEX(lastVdStatus);
    DPRINTLN(F(" -> CCU CH1"));
  }

  void updateDesiredFromTc(uint8_t command, uint8_t targetRaw) {
    currentTcCommand   = command;
    currentTcTargetRaw = targetRaw;

    switch (command) {
      case CMD_REFRESH:
      case CMD_NEW_TARGET:
        specialMode = HY_NORMAL;
        desiredVdTargetRaw = scaledTargetForVd(targetRaw, effectiveFactor());
        break;

      case CMD_VENT_OPEN:
        // 0x01 is still not conclusively proven to be VENT_OPEN. Treating it
        // as a transparent special mode is safe either way and also gives the
        // required FULL_OPEN behavior if the hypothesis is correct.
        specialMode = HY_FULL_OPEN;
        desiredVdTargetRaw = targetRaw;
        break;

      case CMD_VENT_CLOSED:
        specialMode = HY_VENT_CLOSED;
        desiredVdTargetRaw = 0x00;
        break;

      case CMD_DECALC:
      default:
        // Unknown/special commands must not be hydraulically transformed.
        specialMode = HY_OTHER_SPECIAL;
        desiredVdTargetRaw = targetRaw;
        break;
    }
  }

  // Build the left-side response with AskSin++'s native AckStatusMsg shape.
  // Normal operation reports the cached real-VD state:
  //   POSITION = inverse-scaled physical VD position for the TC
  //   STATUS   = unchanged physical VD status
  //   RSSI     = RSSI measured from the received TC command
  // The cache is deliberately asynchronous: Link A never waits for Link B.
  bool sendAckStatusToTc(const Message& request, uint8_t requestRssi) {
    if (vdLost) {
      DPRINT(F("[VD] HY")); DPRINT(recoveryIndex + 1);
      DPRINTLN(F(" TC ACK suppressed"));
      return false;
    }

    Message ack = request;
    HMID target = request.from();

    // init() fixes RESPONSE/ACK_STATUS and channel 1. Real HM-CC-VD replies
    // observed in our radio logs use CTRL=0x82. Set WKMEUP (0x02) explicitly;
    // send(to) adds RPTEN (0x80), yielding the observed CTRL=0x82.
    ack.ackStatus().init(this->channel(THERM_CHANNEL), 0x00);
    ack.flags(0x02);

    const uint8_t positionRaw = tcVisiblePositionRaw();
    const uint8_t statusByte  = vdCacheValid ? lastVdStatus : 0x00;
    // A real HM-CC-VD returns the RSSI of the TC command it just received.
    const uint8_t rssiByte    = requestRssi;

    ack.data()[0] = positionRaw;
    ack.data()[1] = statusByte;
    ack.data()[2] = rssiByte;

    bool ok = this->send(ack, target);

    // Logging only after the timing-sensitive response has been sent.
    dumpMsg(F("TX HY-VD->TC ACK_STATUS (cached real VD)"), ack);
    DPRINT(F("  LinkA mode=VD_CACHE_INVERSE_SCALED"));
    DPRINT(F(" requestedRaw=0x")); DHEX(request.subcommand());
    DPRINT(F(" positionRaw=0x")); DHEX(positionRaw);
    DPRINT(F(" status=0x")); DHEX(statusByte);
    DPRINT(F(" rssi=0x")); DHEX(rssiByte);
    DPRINT(F(" flags=0x")); DHEX(ack.flags());
    DPRINTLN(F(""));
    return ok;
  }

  // Formula reconstructed for HM-CC-TC/HM-CC-VD cyclic timing.
  // Returns 120000 ... 183750 ms in 250 ms steps.
  uint32_t linkBIntervalMs(const HMID& hyTcAddress, uint8_t counter) const {
    uint32_t address = ((uint32_t)hyTcAddress.id0() << 16)
                     | ((uint32_t)hyTcAddress.id1() << 8)
                     |  (uint32_t)hyTcAddress.id2();
    uint32_t seed = (address << 8) | counter;
    uint32_t result = (uint32_t)(seed * 1103515245UL + 12345UL);
    result >>= 16;
    return (480UL + (result & 0xffUL)) * 250UL;
  }

  void startLinkBScheduleIfNeeded() {
    if (vdScheduleStarted || haveTcTarget == false) return;

    HMID valve;
    if (getFirstPeerOfChannel(VALVE_CHANNEL, valve) == false) return;

    // The first frame is an acquisition attempt. Thereafter the complete Link-B
    // phase is deterministic. If the VD is not listening yet, normal missed
    // cycles continue until the VD's own resynchronisation window catches one.
    vdScheduleStarted = true;
    nextVdBaseMs = millis();
    saveRecoveryState();

    DPRINT(F("Link B schedule started: cnt=0x"));
    DHEX(hyTcCounter);
    DPRINTLN(F(" first slot=immediate"));
  }

  uint8_t commandForNextVdSlot() const {
    if (specialMode == HY_NORMAL) {
      bool changedForVd = (lastSentWasNormal == false)
                       || (desiredVdTargetRaw != lastSentVdTargetRaw);
      return changedForVd ? CMD_NEW_TARGET : CMD_REFRESH;
    }

    if (specialMode == HY_VENT_CLOSED) return CMD_VENT_CLOSED;

    // FULL_OPEN candidate, decalc and any unknown special command are kept
    // semantically transparent.
    return currentTcCommand;
  }

  uint8_t targetForNextVdSlot() const {
    if (specialMode == HY_VENT_CLOSED) return 0x00;
    return desiredVdTargetRaw;
  }

  void advanceLinkBSlotWithoutTx(const __FlashStringHelper* reason) {
    if (!vdScheduleStarted) return;

    HMID hyTcAddress;
    this->getDeviceID(hyTcAddress);

    const uint8_t counter = hyTcCounter;
    const uint32_t skippedSlotMs = nextVdBaseMs;
    const uint32_t nominalInterval = linkBIntervalMs(hyTcAddress, counter);
    const uint32_t interval = calibrateLinkBInterval(nominalInterval);

    // Critical: advance from the PLANNED slot, never from millis(). A skipped
    // collision must not shift the complete Link-B phase seen by the real VD.
    nextVdBaseMs = skippedSlotMs + interval;
    hyTcCounter = (uint8_t)(counter + 1U);
    if (collisionDebt != 0xff) ++collisionDebt;
    saveRecoveryState();

    DPRINT(F("LINKB SLOT SKIPPED HY="));
    printHMIDValue(hyTcAddress);
    DPRINT(F(" cnt=0x")); DHEX(counter);
    DPRINT(F(" debt=")); DPRINT(collisionDebt);
    DPRINT(F(" reason=")); DPRINT(reason);
    DPRINT(F(" nextCnt=0x")); DHEX(hyTcCounter);
    DPRINT(F(" nextIn="));
    uint32_t now = millis();
    DPRINT(timeReached(now, nextVdBaseMs) ? 0UL : (nextVdBaseMs - now));
    DPRINTLN(F("ms"));
  }

  bool sendLinkBSlot() {
    HMID valve;
    if (getFirstPeerOfChannel(VALVE_CHANNEL, valve) == false) {
      vdScheduleStarted = false;
      awaitingVdAck = false;
      return false;
    }

    HMID hyTcAddress;
    this->getDeviceID(hyTcAddress);

    const uint8_t counter = hyTcCounter;
    const uint8_t command = commandForNextVdSlot();
    const uint8_t target  = targetForNextVdSlot();
    const uint32_t plannedSlotMs = nextVdBaseMs;

    Message out;
    out.init(0x0b,
             counter,
             TYPE_CLIMATE_EVENT,
             CTRL_TC_TO_VD,
             command,
             target);
    out.from(hyTcAddress);
    out.to(valve);

    this->getHal().prepareSend(out);

    const uint32_t nominalInterval = linkBIntervalMs(hyTcAddress, counter);
    const uint32_t interval = calibrateLinkBInterval(nominalInterval);

    // Commit the NEXT deterministic slot before touching the radio. If TX later
    // wedges after the packet has already left the FIFO, a soft recovery must
    // continue with the next counter instead of repeating this one. If the TX
    // never made it on air, treating this slot as lost is still correct.
    nextVdBaseMs = plannedSlotMs + interval;
    hyTcCounter = (uint8_t)(counter + 1U);
    saveRecoveryState();

    // Exactly one physical attempt. No immediate retry: if this cycle is lost,
    // the following cycle remains on the deterministic Link-B time line.
    bool sent = this->radio().write(out, false);

    if (sent) {
      awaitingVdAck = true;
      awaitingVdCounter = counter;
      vdAckDeadlineMs = millis() + VD_ACK_TIMEOUT_MS;
      // Do not clear collisionDebt merely because the packet left the CC1101.
      // It is cleared only after the matching VD ACK_STATUS is received.
    }
    else {
      awaitingVdAck = false;
      if (missCount != 0xff) ++missCount;
    }

    if (specialMode == HY_NORMAL) {
      lastSentWasNormal = true;
      lastSentVdTargetRaw = target;
    }
    else {
      lastSentWasNormal = false;
    }
    saveRecoveryState();

    dumpMsg(F("TX HY-TC->VD CLIMATE_EVENT"), out);
    DPRINT(F("  LinkB sent=")); DPRINT(sent);
    DPRINT(F(" cnt=0x")); DHEX(counter);
    DPRINT(F(" cmd=0x")); DHEX(command);
    DPRINT(F(" targetRaw=0x")); DHEX(target);
    DPRINT(F(" nextCnt=0x")); DHEX(hyTcCounter);
    DPRINT(F(" nextNominal=")); DPRINT(nominalInterval);
    DPRINT(F("ms nextCalibrated=")); DPRINT(interval);
    DPRINT(F("ms debt=")); DPRINT(collisionDebt);
    DPRINT(F(" ackPending=")); DPRINT(sent ? 1 : 0);
    DPRINTLN(F(""));
    return sent;
  }

  bool handleTcClimate(Message& msg) {
    if (msg.type() != TYPE_CLIMATE_EVENT || msg.length() < 0x0b) return false;

    const uint32_t tcRxMs = millis();

    // In a 0x0b CLIMATE_EVENT the two bytes after DST are represented by
    // AskSin++ as command() and subcommand().
    const uint8_t command   = msg.command();
    const uint8_t targetRaw = msg.subcommand();

    haveTcTarget = true;
    updateDesiredFromTc(command, targetRaw);
    lastEffectiveFactor = effectiveFactor();

    // A mode change (normal/special) can change how the cached physical VD
    // position is represented on Link A and towards the CCU.
    updateLinkAChannelStateFromCache();

    // Capture RSSI immediately while the received TC frame is still current.
    const uint8_t tcRequestRssi = this->radio().rssi();

    // Save the newly requested target before any radio TX can potentially
    // trigger a soft recovery.
    saveRecoveryState();

    // In normal operation match the real VD response latency. In VD_LOST mode
    // deliberately send no response at all so the original TC detects the same
    // communication failure it would see with a missing physical VD.
    if (!vdLost && LINK_A_RESPONSE_DELAY_MS > 0) {
      delay(LINK_A_RESPONSE_DELAY_MS);
    }
    bool ackSent = sendAckStatusToTc(msg, tcRequestRssi);
    const uint32_t ackDoneMs = millis();

    startLinkBScheduleIfNeeded();

    // Debug only after the ACK was physically sent.
    dumpMsg(F("RX TC->HY-VD CLIMATE_EVENT"), msg);
    DPRINT(F("  TC cmd=0x")); DHEX(command);
    DPRINT(F(" targetRaw=0x")); DHEX(targetRaw);
    DPRINT(F(" target=")); DPRINT(decodeTcTargetPercent100(targetRaw) / 100U);
    DPRINT(F("."));
    uint8_t frac = (uint8_t)(decodeTcTargetPercent100(targetRaw) % 100U);
    if (frac < 10) DPRINT('0');
    DPRINT(frac);
    DPRINT(F("% factor=")); DPRINT(effectiveFactor());
    DPRINT(F(" reduction=")); DPRINT(reductionPercent());
    DPRINT(F("% desiredRaw=0x")); DHEX(desiredVdTargetRaw);
    DPRINT(F(" ackSent=")); DPRINT(ackSent);
    DPRINT(F(" responseDelay=")); DPRINT(LINK_A_RESPONSE_DELAY_MS); DPRINT(F("ms"));
    DPRINT(F(" cacheValid=")); DPRINT(vdCacheValid ? 1 : 0);
    if (vdCacheValid) {
      DPRINT(F(" cachedVdRaw=0x")); DHEX(lastVdPositionRaw);
      DPRINT(F(" tcVisibleRaw=0x")); DHEX(tcVisiblePositionRaw());
    }
    DPRINT(F(" ackDoneAfter=")); DPRINT(ackDoneMs - tcRxMs);
    DPRINTLN(F("ms"));

    return true;
  }

  bool handleVdAckStatus(Message& msg) {
    if (msg.type() != TYPE_RESPONSE || msg.length() < 0x0e) return false;
    if (msg.command() != RESPONSE_ACK_STATUS) return false;
    if (msg.subcommand() != VD_CHANNEL) return false;

    // Layout: 01 01 POSITION STATUS RSSI
    lastVdPositionRaw = msg.data()[0];
    lastVdStatus      = msg.data()[1];
    lastVdRssi        = msg.data()[2];
    vdCacheValid      = true;
    missCount         = 0;
    noteValidVdRx();

    // Make the real VD status visible to the HY's own CCU representation.
    // If position/status changed, the shared loop queues an asynchronous
    // INFO_ACTUATOR_STATUS. CCU status requests also read this state.
    updateLinkAChannelStateFromCache();

    bool matchesOutstanding = awaitingVdAck && (msg.count() == awaitingVdCounter);
    if (matchesOutstanding) {
      awaitingVdAck = false;

      // A complete HY->VD->HY exchange succeeded. Any collision priority that
      // was accumulated while this HY missed earlier slots is no longer needed:
      // the real VD is known to be synchronized again.
      if (collisionDebt != 0) {
        DPRINT(F("[TX-SCHED] successful VD ACK clears collision debt "));
        DPRINT(collisionDebt);
        DPRINTLN(F(" -> 0"));
        collisionDebt = 0;
        saveRecoveryState();
      }
    }

    dumpMsg(F("RX VD->HY-TC ACK_STATUS"), msg);
    DPRINT(F("  VD posRaw=")); DPRINT(lastVdPositionRaw);
    DPRINT(F(" pos=")); DPRINT(decodeVdPositionPercent100(lastVdPositionRaw) / 100U);
    DPRINT(F("."));
    uint8_t frac = (uint8_t)(decodeVdPositionPercent100(lastVdPositionRaw) % 100U);
    if (frac < 10) DPRINT('0');
    DPRINT(frac);
    DPRINT(F("% status=0x")); DHEX(lastVdStatus);
    DPRINT(F(" rssiByte=0x")); DHEX(lastVdRssi);
    DPRINT(F(" outstanding=")); DPRINTLN(matchesOutstanding);

    return true;
  }

  void serviceRuntime() {
    uint32_t now = millis();

    // A factor/enable change updates the pending right-side target, but does not
    // create an extra transmission. The next regular VD slot will use CMD=03 if
    // the effective reduced target changed.
    uint8_t factor = effectiveFactor();
    if (factor != lastEffectiveFactor) {
      lastEffectiveFactor = factor;
      if (haveTcTarget && specialMode == HY_NORMAL) {
        desiredVdTargetRaw = scaledTargetForVd(currentTcTargetRaw, factor);
        DPRINT(F("HY factor changed: factor=")); DPRINT(factor);
        DPRINT(F(" reduction=")); DPRINT(100U - factor);
        DPRINT(F("% new desiredRaw=0x")); DHEX(desiredVdTargetRaw); DPRINTLN(F(""));
      }
      // The inverse scaling seen by TC/CCU changes with the factor even if the
      // physical VD has not moved.
      updateLinkAChannelStateFromCache();
      saveRecoveryState();
    }

    if (awaitingVdAck && timeReached(now, vdAckDeadlineMs)) {
      awaitingVdAck = false;
      if (missCount != 0xff) ++missCount;
      Serial.print(F("A")); Serial.print(recoveryIndex + 1);
      Serial.print(' '); Serial.print(awaitingVdCounter, HEX);
      Serial.print(' '); Serial.println(missCount);
      // Intentionally no retry. The normal cyclic schedule continues.
    }

    startLinkBScheduleIfNeeded();
    serviceVdWatchdog(now);

  }

  void dumpPeers() {
    DPRINTLN(F("---- Persistent peer state ----"));

    HMID master = this->getMasterID();
    DPRINT(F("MasterID: "));
    if (master.valid()) {
      printHMIDValue(master);
      DPRINTLN(F(""));
    } else {
      DPRINTLN(F("<invalid>"));
    }

    for (uint8_t ch = 1; ch <= ChannelCount; ++ch) {
      DPRINT(F("Peers CH"));
      DPRINT(ch);
      DPRINT(F(": "));
      bool any = false;
      for (uint8_t i = 0; i < this->channel(ch).peers(); ++i) {
        Peer p = this->channel(ch).peerat(i);
        if (!p.valid()) continue;
        any = true;
        DPRINT(F("#")); DPRINT(i); DPRINT(F("="));
        HMID pid = p;
        printHMIDValue(pid);
        DPRINT(F(" "));
      }
      if (any == false) DPRINT(F("<none>"));
      DPRINTLN(F(""));
    }

    DPRINTLN(F("-------------------------------"));
  }

  virtual bool process(Message& msg) {
    HMID me;
    this->getDeviceID(me);

    const bool toMe          = (msg.to() == me);
    const bool toBroadcast   = isBroadcastHMID(msg.to());
    const bool fromThermPeer = isFromChannelPeer(THERM_CHANNEL, msg.from());
    const bool fromValvePeer = isFromChannelPeer(VALVE_CHANNEL, msg.from());

    // ACK/NACK for our unsolicited CCU status update is handled here without
    // Device::waitResponse(). This keeps the shared dispatcher alive.
    if (toMe && handleAsyncCcuStatusAck(msg)) {
      return true;
    }

    // Link A: consume the runtime CLIMATE_EVENT ourselves. Do NOT call the base
    // implementation afterwards: it does not know TYPE=0x58 here and would send
    // a NACK for an ACK-requesting frame.
    if (fromThermPeer && toMe && msg.type() == TYPE_CLIMATE_EVENT) {
      return handleTcClimate(msg);
    }

    // Link B: consume ACK_STATUS asynchronously so it remains available to the
    // next Link-A request. A late ACK is still useful for the cache.
    if (fromValvePeer && toMe && msg.type() == TYPE_RESPONSE) {
      if (handleVdAckStatus(msg)) return true;
    }

    // The updated original proxy classifies valve type 0x10 as runtime traffic.
    // Its precise semantics are not part of the reconstructed TC<->VD protocol
    // document. Do not raw-forward it: Link A and Link B have independent
    // counters and blindly reusing the VD counter would reintroduce the old
    // repeater architecture. Consume and log it until its semantics are known.
    if (fromValvePeer && toMe && msg.type() == TYPE_VALVE_RUNTIME) {
      dumpMsg(F("RX VD runtime 0x10 (consumed, not raw-forwarded)"), msg);
      noteValidVdRx();

      // Observed VD runtime frames use e.g. CTRL=0xA4, which includes BIDI.
      // Since HY is the addressed peer, consuming such a frame without an ACK
      // makes the VD repeat it. Acknowledge it locally while keeping Link A/B
      // counters independent.
      if (msg.ackRequired()) {
        const uint8_t cnt = msg.count();

        // Give CC1101 a short, explicit RX->TX settling interval. The previous
        // diagnostic radio wrapper unintentionally provided such a delay via
        // serial logging; without it the first runtime ACK could wedge the
        // radio on this hardware.
        delay(VD_RUNTIME_ACK_DELAY_MS);

        DPRINT(F("  VD runtime ACK BEGIN cnt=0x")); DHEX(cnt); DPRINTLN(F(""));
        hyRadioStage = 0x10; // semantic marker; deeper TX stages overwrite it
        this->sendAck(msg);
        hyRadioStage = 0;
        DPRINT(F("  ACKed VD runtime 0x10 cnt=0x")); DHEX(cnt); DPRINTLN(F(""));
      }
      return true;
    }

    // Weather broadcasts are intentionally NOT forwarded to the VD. Their
    // necessity for VD actuation is not established; FHEM's virtual TC works via
    // the 0x58 path. Logging only.
    if (fromThermPeer && toBroadcast && msg.type() == TYPE_WEATHER_EVENT) {
      dumpMsg(F("RX TC WEATHER_EVENT (not forwarded)"), msg);
      return true;
    }

    // MultiChannelDevice::process() normally schedules a 100 ms reply delay
    // with setSendTimeout(). Our Radio has SENDDELAY=0, so that default call
    // does nothing. Restore the delay for addressed base-protocol traffic;
    // the custom Link-A/Link-B handlers above keep their separate timing.
    if (toMe || this->isBroadcastMsg(msg)) {
      this->radio().setSendTimeout(CONFIG_RESPONSE_DELAY_MS);
    }
    return DevType::process(msg);
  }
};

typedef ProxyDevice<NUM_CHANNELS> ProxyDeviceType;


// Two complete logical Homematic devices share one physical HAL/radio.
//
// 0x20.. is the original device's EEPROM layout. Device 2 starts at 0x60,
// leaving enough space for the current 42-byte non-AES layout and preserving
// device 1's existing storage. The byte immediately before device 2 is used
// only as an initialization marker for the new second device.
static const uint16_t DEVICE1_EEPROM_ADDR = 0x20;
static const uint16_t DEVICE2_EEPROM_ADDR = 0x60;
static const uint16_t DEVICE2_INIT_MARKER_ADDR = DEVICE2_EEPROM_ADDR - 1;
static const uint8_t  DEVICE2_INIT_MARKER = 0xA2;

Hal hal;
ProxyDeviceType sdev1(devinfo1, DEVICE1_EEPROM_ADDR, 0 * LINKB_COUNTER_STEP, 0);
ProxyDeviceType sdev2(devinfo2, DEVICE2_EEPROM_ADDR, 1 * LINKB_COUNTER_STEP, 1);

static ProxyDeviceType* const virtualDevices[] = { &sdev1, &sdev2 };
static const uint8_t VIRTUAL_DEVICE_COUNT =
  sizeof(virtualDevices) / sizeof(virtualDevices[0]);

// Config-button pairing session:
//   * First press after idle starts a fixed 10-minute session and selects the
//     first HY without a MasterID (or HY #1 if all are already paired).
//   * Every further press inside those same 10 minutes selects the next HY,
//     regardless of whether it is already paired. This allows a CCU that has
//     forgotten one HY to learn it again without deleting HY-side state.
//   * The selected HY re-advertises every 2 s until the CCU starts talking to
//     it, or until the 22 s pairing attempt ends. The 10-minute session itself
//     is NOT extended by later button presses.
static bool     pairingSessionActive = false;
static uint32_t pairingSessionDeadlineMs = 0;
static int8_t   pairingSelectedIndex = -1;
static bool     pairingAttemptActive = false;
static bool     pairingTrafficSeen = false;
static bool     pairingWasAlreadyPaired = false;
static uint32_t pairingAttemptDeadlineMs = 0;
static uint32_t pairingNextAdvertiseMs = 0;
static uint32_t pairingLastTrafficMs = 0;
static uint8_t  pairingButtonPressesPending = 0;

static int8_t findFirstUnpairedHy() {
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (!virtualDevices[i]->getMasterID().valid()) return (int8_t)i;
  }
  return -1;
}

static void beginPairAttempt(uint8_t index) {
  pairingSelectedIndex = (int8_t)index;
  pairingAttemptActive = true;
  pairingTrafficSeen = false;
  pairingWasAlreadyPaired = virtualDevices[index]->getMasterID().valid();
  pairingAttemptDeadlineMs = millis() + PAIR_ATTEMPT_TIMEOUT_MS;
  pairingNextAdvertiseMs = millis();
  pairingLastTrafficMs = millis();

  HMID id;
  virtualDevices[index]->getDeviceID(id);
  DPRINT(F("[PAIR] selected HY #")); DPRINT(index + 1);
  DPRINT(F(" HMID=")); printHMIDValue(id);
  DPRINT(pairingWasAlreadyPaired ? F(" already paired to ") : F(" unpaired"));
  if (pairingWasAlreadyPaired) printHMIDValue(virtualDevices[index]->getMasterID());
  DPRINTLN(F(""));
}

static void handlePairButtonPress() {
  const uint32_t now = millis();

  if (!pairingSessionActive || timeReached(now, pairingSessionDeadlineMs)) {
    pairingSessionActive = true;
    pairingSessionDeadlineMs = now + PAIRING_SESSION_MS;

    int8_t first = findFirstUnpairedHy();
    if (first < 0) first = 0;

    DPRINTLN(F("[PAIR] new 10-minute pairing session"));
    beginPairAttempt((uint8_t)first);
    return;
  }

  uint8_t next = pairingSelectedIndex < 0
               ? 0
               : (uint8_t)((pairingSelectedIndex + 1) % VIRTUAL_DEVICE_COUNT);
  DPRINT(F("[PAIR] session active - advance to next HY #"));
  DPRINTLN(next + 1);
  beginPairAttempt(next);
}

static void notePairingTraffic(uint8_t index) {
  if (!pairingSessionActive || !pairingAttemptActive) return;
  if (pairingSelectedIndex != (int8_t)index) return;
  pairingTrafficSeen = true;
  pairingLastTrafficMs = millis();
}

static void servicePairingSession() {
  const uint32_t now = millis();

  while (pairingButtonPressesPending > 0) {
    --pairingButtonPressesPending;
    handlePairButtonPress();
  }

  if (pairingSessionActive && timeReached(now, pairingSessionDeadlineMs)) {
    DPRINTLN(F("[PAIR] 10-minute pairing session ended"));
    pairingSessionActive = false;
    pairingAttemptActive = false;
    pairingSelectedIndex = -1;
    return;
  }

  if (!pairingAttemptActive || pairingSelectedIndex < 0) return;

  ProxyDeviceType* dev = virtualDevices[(uint8_t)pairingSelectedIndex];

  // Once the CCU starts its configuration exchange, do not inject more
  // DeviceInfo announcements into that exchange.
  if (!pairingTrafficSeen && timeReached(now, pairingNextAdvertiseMs)) {
    DPRINT(F("[PAIR] advertise HY #")); DPRINT(pairingSelectedIndex + 1);
    DPRINTLN(F(" startPairing()"));
    dev->startPairing();
    pairingNextAdvertiseMs = millis() + PAIR_REANNOUNCE_MS;
  }

  if (pairingTrafficSeen) {
    bool complete = false;
    HMID master = dev->getMasterID();

    if (pairingWasAlreadyPaired) {
      // Re-learning at the same CCU: MasterID was valid before the attempt, so
      // completion is defined by a completed/quiet CCU exchange.
      complete = (uint32_t)(now - pairingLastTrafficMs) >= PAIR_FINISH_QUIET_MS;
    }
    else if (master.valid()) {
      complete = (uint32_t)(now - pairingLastTrafficMs) >= PAIR_FINISH_QUIET_MS;
    }

    if (complete) {
      DPRINT(F("[PAIR] HY #")); DPRINT(pairingSelectedIndex + 1);
      DPRINT(F(" pairing exchange finished; MasterID="));
      printHMIDValue(master);
      DPRINTLN(F("; session stays active for further button presses"));
      pairingAttemptActive = false;
      return;
    }
  }

  if (timeReached(now, pairingAttemptDeadlineMs)) {
    DPRINT(F("[PAIR] HY #")); DPRINT(pairingSelectedIndex + 1);
    DPRINTLN(F(" attempt ended after 22 s; 10-minute session remains active"));
    pairingAttemptActive = false;
  }
}

// One physical config button is shared by all logical devices.
class MultiHyConfigButton : public Button {
private:
  ProxyDeviceType& d1;
  ProxyDeviceType& d2;

public:
  MultiHyConfigButton(ProxyDeviceType& a, ProxyDeviceType& b)
    : Button(), d1(a), d2(b) {
    this->setLongPressTime(seconds2ticks(3));
  }

  virtual void state(uint8_t s) {
    uint8_t old = Button::state();
    Button::state(s);

    if (s == Button::released) {
      if (pairingButtonPressesPending != 0xff) ++pairingButtonPressesPending;
      DPRINTLN(F("Config button: pairing selection requested"));
    }
    else if (s == Button::longpressed) {
      if (old == Button::longpressed) {
        DPRINTLN(F("Config button: factory reset ALL virtual HY devices"));
        d1.reset();
      }
      else {
        d1.led().set(LedStates::key_long);
      }
    }
  }
};

MultiHyConfigButton cfgBtn(sdev1, sdev2);

static bool isAnyVirtualDeviceID(const HMID& id) {
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (virtualDevices[i]->isDeviceID(id)) return true;
  }
  return false;
}

static bool anyVdAckPending() {
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (virtualDevices[i]->isAwaitingVdAck()) return true;
  }
  return false;
}

// Dispatch one already-received physical radio packet to the matching logical
// device. The radio is read exactly once, so no logical device can consume
// another logical device's packet accidentally.
static bool dispatchSharedRadioMessage(Message& msg) {
  if (isAnyVirtualDeviceID(msg.from())) {
    return false; // same behavior as Device::pollRadio(): ignore own TX
  }

  // Addressed traffic has exactly one owner.
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    HMID id;
    virtualDevices[i]->getDeviceID(id);
    if (msg.to() == id) {
      notePairingTraffic(i);
      return virtualDevices[i]->process(msg);
    }
  }

  // Pair-by-serial is broadcast. Route it only to the virtual device whose
  // serial number was requested.
  if (msg.isPairSerial()) {
    for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
      if (virtualDevices[i]->isDeviceSerial(msg.data())) {
        notePairingTraffic(i);
        return virtualDevices[i]->process(msg);
      }
    }
    return false;
  }

  // TC weather broadcasts use DST=000000. Prefer the logical HY that actually
  // owns the sender as a peer, so the current radio RSSI remains meaningful.
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (virtualDevices[i]->isFromChannelPeer(THERM_CHANNEL, msg.from()) ||
        virtualDevices[i]->isFromChannelPeer(VALVE_CHANNEL, msg.from())) {
      return virtualDevices[i]->process(msg);
    }
  }

  // Other broadcast/discovery traffic is rare here. Give every virtual device
  // an independent copy because the base protocol handler is allowed to mutate
  // the Message while constructing a reply.
  bool handled = false;
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    Message copy = msg;
    handled |= virtualDevices[i]->process(copy);
  }
  return handled;
}

static int8_t findVdAckOwner() {
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (virtualDevices[i]->isAwaitingVdAck()) return (int8_t)i;
  }
  return -1;
}

static bool anyLinkBSlotWithin(uint32_t now, uint16_t windowMs) {
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (virtualDevices[i]->linkBSlotWithin(now, windowMs)) return true;
  }
  return false;
}

static uint8_t lastLinkBWinner = 0xff;

static uint8_t roundRobinDistance(uint8_t index) {
  if (lastLinkBWinner == 0xff) return index;
  uint8_t first = (uint8_t)((lastLinkBWinner + 1U) % VIRTUAL_DEVICE_COUNT);
  return (uint8_t)((index + VIRTUAL_DEVICE_COUNT - first) % VIRTUAL_DEVICE_COUNT);
}

// Central scheduler for all virtual HY -> VD transmissions.
// One skipped collision advances counter/time from the planned slot and raises
// collisionDebt. A device with higher debt wins a later collision, preventing
// the same HY from losing repeatedly.
static void serviceLinkBScheduler() {
  const uint32_t now = millis();
  const int8_t ackOwner = findVdAckOwner();

  if (ackOwner >= 0) {
    // The physical radio belongs to this VD ACK window. Any other Link-B slot
    // becoming due now is deliberately dropped, not delayed.
    for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
      if (i == (uint8_t)ackOwner) continue;
      if (virtualDevices[i]->isLinkBDue(now)) {
        virtualDevices[i]->advanceLinkBSlotWithoutTx(F("other HY awaiting VD ACK"));
      }
    }
    return;
  }

  int8_t winner = -1;
  uint8_t dueCount = 0;
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (!virtualDevices[i]->isLinkBDue(now)) continue;
    ++dueCount;

    if (winner < 0) {
      winner = (int8_t)i;
      continue;
    }

    ProxyDeviceType* a = virtualDevices[i];
    ProxyDeviceType* b = virtualDevices[(uint8_t)winner];

    if (a->linkBCollisionDebt() > b->linkBCollisionDebt()) {
      winner = (int8_t)i;
    }
    else if (a->linkBCollisionDebt() == b->linkBCollisionDebt()) {
      int32_t dt = (int32_t)(a->linkBSlotTime() - b->linkBSlotTime());
      if (dt < 0 || (dt == 0 && roundRobinDistance(i) < roundRobinDistance((uint8_t)winner))) {
        winner = (int8_t)i;
      }
    }
  }

  if (winner < 0) return;

  ProxyDeviceType* chosen = virtualDevices[(uint8_t)winner];

  // Fairness look-ahead: if a HY that has unresolved collision debt has its
  // exact slot inside the ACK guard window, skip the current lower-debt slot
  // now so the owed HY can transmit at ITS correct time instead of losing yet
  // again 50...300 ms later.
  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    if (i == (uint8_t)winner) continue;
    ProxyDeviceType* other = virtualDevices[i];
    if (!other->linkBScheduleActive()) continue;
    if (other->linkBCollisionDebt() <= chosen->linkBCollisionDebt()) continue;

    uint32_t otherSlot = other->linkBSlotTime();
    if (!timeReached(now, otherSlot) &&
        (uint32_t)(otherSlot - now) <= LINKB_COLLISION_GUARD_MS) {
      DPRINT(F("[TX-SCHED] HY #")); DPRINT(i + 1);
      DPRINT(F(" has higher debt=")); DPRINT(other->linkBCollisionDebt());
      DPRINT(F(" and is due in ")); DPRINT(otherSlot - now);
      DPRINTLN(F("ms -> current slot loses"));

      chosen->advanceLinkBSlotWithoutTx(F("higher-debt HY due inside guard"));
      // Keep the other HY's collision debt intact. It is cleared only after
      // that HY actually sends at its slot and receives the matching VD ACK.
      return;
    }
  }

  // If two or more slots are already due together, the selected HY is the
  // collision winner. Its debt is deliberately NOT changed here; only a
  // matching VD ACK proves that communication has recovered.

  if (chosen->sendLinkBSlot()) {
    lastLinkBWinner = (uint8_t)winner;
  }

  // If several slots were already due together, the winner now owns the ACK
  // window. The remaining due slots are skipped immediately on the next loop.
}

static uint32_t lastSharedRadioRxMs = 0;

static bool pollSharedRadio() {
  Message msg;
  uint8_t num = hal.radio.read(msg);
  if (num >= 10) {
    lastSharedRadioRxMs = millis();
    return dispatchSharedRadioMessage(msg);
  }
  return false;
}

// AskSin++ normally initializes one ChannelDevice together with the HAL.
// Here we lay out and attach two devices, but initialize the physical HAL,
// CC1101 and system clock only once.
//
// Existing HY #1 EEPROM contents remain usable. HY #2 is initialized exactly
// once using a marker outside both device regions.
static bool initVirtualDevices() {
  sdev1.layoutChannels();
  sdev2.layoutChannels();

  sdev1.dumpSize();
  sdev2.dumpSize();

  // Keep the original checksum/magic so an already paired HY #1 is not wiped
  // merely because a second logical device was added.
  bool first = storage().setup(sdev1.checksum());

  if (first) {
    sdev1.firstinit();
    sdev2.firstinit();
    storage().setByte(DEVICE2_INIT_MARKER_ADDR, DEVICE2_INIT_MARKER);
    storage().store();
  }
  else if (storage().getByte(DEVICE2_INIT_MARKER_ADDR) != DEVICE2_INIT_MARKER) {
    DPRINTLN(F("Initializing EEPROM area for HY #2"));
    sdev2.firstinit();
    storage().setByte(DEVICE2_INIT_MARKER_ADDR, DEVICE2_INIT_MARKER);
    storage().store();
  }

  sdev1.keystore().init();
  sdev2.keystore().init();

  sdev1.setHal(hal);
  sdev2.setHal(hal);

  // One physical HAL/radio init only.
  HMID primaryId;
  sdev1.getDeviceID(primaryId);
  hal.init(primaryId);

  // Frequency configuration is physical-radio state, not per virtual device.
  // Reuse the original device's config area.
  hal.config(sdev1.getConfigArea());

  return first;
}

void setup() {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  Serial.begin(57600);
  Serial.println(F("HY v21"));

  // A direct watchdog ISR jump has no hardware reset cause. A real power/reset
  // must never restore stale .noinit Link-B state.
  const bool softRecovery = (hySoftWatchdogMagic == HY_SOFT_WDT_MAGIC)
                         && (hyResetCause == 0);
  const uint32_t recoveryOldMillis = hyWatchdogOldMillis;
  const uint8_t recoveryRadioStage = hyRecoveredRadioStage;

  DPRINTLN(F("=== HB-SR-HY multi-device shared-radio test ==="));
  DPRINTLN(F("Virtual HY #1: HMID FE0101 / serial SRUE000001"));
  DPRINTLN(F("Virtual HY #2: HMID FE0102 / serial SRUE000002"));
  DPRINTLN(F("Each virtual HY still has CH1=TC side and CH2=VD side"));
  DPRINTLN(F("One CC1101 RX dispatcher routes packets by destination HMID"));
  DPRINTLN(F("Link-B ACK_STATUS reception is asynchronous"));
  DPRINT(F("Link A measured response delay: +")); DPRINT(LINK_A_RESPONSE_DELAY_MS); DPRINTLN(F(" ms before ACK"));
  DPRINT(F("Link B timebase calibration: ")); DPRINT(LINKB_TIME_SCALE_NUM);
  DPRINT(F("/")); DPRINTLN(LINKB_TIME_SCALE_DEN);
  DPRINT(F("Link B VD ACK timeout: ")); DPRINT(VD_ACK_TIMEOUT_MS); DPRINTLN(F(" ms"));
  DPRINT(F("VD runtime ACK delay: ")); DPRINT(VD_RUNTIME_ACK_DELAY_MS); DPRINTLN(F(" ms"));
  DPRINT(F("CC1101 MISO watchdog: ")); DPRINT(CC1101_MISO_TIMEOUT_US); DPRINTLN(F(" us"));
  DPRINT(F("AVR SPI transfer timeout: ")); DPRINT(AVR_SPI_TRANSFER_TIMEOUT_US); DPRINTLN(F(" us"));
  DPRINTLN(F("VD watchdog=600s; radio RX recovery=80ms"));
  DPRINT(F("Reset cause MCUSR=0x")); DHEX(hyResetCause); DPRINTLN(F(""));
  DPRINTLN(F("Config button: 10-minute session; first press=first unpaired, further presses=next HY"));
  DPRINT(F("Link-B initial counters: HY#1=0x")); DHEX(sdev1.linkBCounter());
  DPRINT(F(" HY#2=0x")); DHEX(sdev2.linkBCounter()); DPRINTLN(F(""));
  DPRINTLN(F("CCU actuator-status ACK handling: asynchronous (no blocking waitAck)"));
  DPRINTLN(F("CCU actuator-status startup guard: no TX before first valid VD runtime state"));

  bool first = initVirtualDevices();

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    DPRINTLN(F("Config button held at boot - hold for factory reset of both virtual devices"));
    delay(3500);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      DPRINTLN(F("Factory reset requested for both virtual devices"));
      sdev1.reset();
    }
  }

  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);

  if (first) {
    DPRINTLN(F("First init - both virtual device storage areas initialized"));
  }

  sdev1.initDone();
  sdev2.initDone();

  // The CC1101 is physical hardware and is configured once.
  sdev1.configureRadio();

  if (softRecovery) {
    Serial.print(F("W ")); Serial.print(recoveryRadioStage, HEX);
    Serial.print(' '); Serial.println(recoveryOldMillis);
    bool r1 = sdev1.restoreRecoveryState(recoveryOldMillis);
    bool r2 = sdev2.restoreRecoveryState(recoveryOldMillis);
    Serial.print(F("WS ")); Serial.print(r1 ? 1 : 0);
    Serial.print('/'); Serial.println(r2 ? 1 : 0);

    hySoftWatchdogMagic = 0;
    hyRecoveredRadioStage = 0;
    hyWatchdogOldMillis = 0;
  }

  for (uint8_t i = 0; i < VIRTUAL_DEVICE_COUNT; ++i) {
    HMID id;
    virtualDevices[i]->getDeviceID(id);
    DPRINT(F("---- Virtual HY #")); DPRINT(i + 1); DPRINT(F(" HMID="));
    printHMIDValue(id);
    DPRINTLN(F(" ----"));
    virtualDevices[i]->dumpPeers();

    HMID linkBPeer;
    if (virtualDevices[i]->getFirstPeerOfChannel(VALVE_CHANNEL, linkBPeer) == false) {
      DPRINT(F("WARNING: HY #")); DPRINT(i + 1);
      DPRINTLN(F(" CH2 has no real-VD peer"));
    }
  }

  // Mains powered: continuously receptive.
  hal.battery.init(seconds2ticks(60UL * 60), sysclock);

  // Last-resort protection without a soft watchdog RESET. The classic
  // Pro Mini bootloader is not watchdog-reset safe. Interrupt mode performs a
  // direct application restart after 8 s and records the current radio stage.
  enableHySoftWatchdog();
  wdt_reset();
  Serial.println(F("WD8"));

  // On normal boots clear any stale marker. A genuine soft recovery was
  // already restored and cleared above.
  if (!softRecovery) {
    hySoftWatchdogMagic = 0;
    hyRecoveredRadioStage = 0;
    hyWatchdogOldMillis = 0;
  }

  hyRadioStage = 0;
}

void loop() {
  wdt_reset();
  hal.runready();

  // Read at most one packet per pass. loop() repeats quickly, while preserving
  // the RSSI associated with the just-read packet until its owner processes it.
  pollSharedRadio();

  // Pairing radio TX is executed from the normal loop, never from the button
  // callback. A pairing session remains available for ten minutes.
  servicePairingSession();

  // Per-HY housekeeping only: factor changes, VD ACK timeout, schedule start.
  sdev1.serviceRuntime();
  sdev2.serviceRuntime();

  // Exactly one central owner decides all HY->VD Link-B transmissions.
  serviceLinkBScheduler();

  // CCU status retries are asynchronous. Give Link-B/VD timing priority and
  // don't start/retry a CCU status if a VD slot is due within the ACK guard.
  const uint32_t now = millis();
  const bool radioQuietForCcu =
      (uint32_t)(now - lastSharedRadioRxMs) >= CCU_STATUS_RADIO_QUIET_MS;

  bool allowCcuTx = !anyVdAckPending() &&
                    radioQuietForCcu &&
                    !anyLinkBSlotWithin(now, CCU_STATUS_ACK_TIMEOUT_MS);
  sdev1.serviceAsyncCcuStatus(allowCcuTx);
  sdev2.serviceAsyncCcuStatus(allowCcuTx);

  if (allowCcuTx) {
    sdev1.serviceChangedChannelsAsync(true);
    // Re-evaluate after HY #1 may have started a status transaction. The
    // physical radio may still send HY #2 status too, but serializing keeps the
    // long-test behavior deterministic and reduces avoidable CCU collisions.
    if (!sdev1.hasPendingCcuStatus()) {
      sdev2.serviceChangedChannelsAsync(true);
    }
  }

  wdt_reset();
}
