//- -----------------------------------------------------------------------------------------------------------------------
// HB-SR-HY v25c compact EEPROM + bounded broadcast pairing
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
// EEPROM / factor semantics:
//   v25 deliberately uses a new compact EEPROM layout for all 20 logical HYs.
//   The first v25 boot reinitializes all 20 logical devices; previous pairing
//   and peer data are intentionally discarded so the final 20-HY teach-in path
//   can be tested cleanly.
//   Register 0x02 remains "factor":
//       factor = percentage passed to the real VD
//       factor 100 -> reduction x = 0 %
//       factor  80 -> reduction x = 20 %
//
// Important scope limitations matching the still-open points in the protocol notes:
//   * AES/security handshakes for the custom 0x58/0x02 runtime path are not implemented here.
//   * The exact original VD RX-window width is still an empirical point; cyclic Link-B timing uses no fixed offset.
//   * A real power cycle still starts a fresh Link-B phase. A soft-watchdog recovery, however, preserves
//     counter/phase/target/cache in .noinit RAM so the real VD stays synchronized.
//   * HY does not invent a VD error bit. If no valid VD traffic is seen for ten minutes, HY instead
//     stops answering Link A so the original TC detects the missing valve exactly as with a lost VD.
//   Runtime for ALL 20 HYs is compact/custom. One reusable AskSin++ adapter handles only pairing/config EEPROM.
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
#include <new>

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
#define LOGICAL_HY_COUNT 20

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

// The complete compact 20-HY runtime array is placed in .noinit further below.
// It survives only our direct watchdog application restart; on a real reset it
// is rebuilt from the persistent AskSin++ EEPROM configuration.
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

// A VD is considered unreachable only after ten minutes without any valid
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
//
// One short button press starts/resumes an AUTOMATIC pairing chain:
// HY1 -> HY2 -> HY3 -> ... as long as the CCU keeps completing pairings.
// If one HY gets no usable pairing response before PAIR_ATTEMPT_TIMEOUT_MS,
// the chain stops on that HY. The next button press retries exactly that
// HY and, after success, continues automatically with the following HYs.
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

// All 20 logical identities are stored in flash. No Device object is duplicated
// for these identities; the reusable config adapter temporarily binds to one.
const struct DeviceInfo PROGMEM hyDeviceInfo[LOGICAL_HY_COUNT] = {
  {
    {0xfe,0x01,0x01},
    "SRUE000001",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x02},
    "SRUE000002",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x03},
    "SRUE000003",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x04},
    "SRUE000004",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x05},
    "SRUE000005",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x06},
    "SRUE000006",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x07},
    "SRUE000007",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x08},
    "SRUE000008",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x09},
    "SRUE000009",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0a},
    "SRUE000010",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0b},
    "SRUE000011",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0c},
    "SRUE000012",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0d},
    "SRUE000013",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0e},
    "SRUE000014",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x0f},
    "SRUE000015",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x10},
    "SRUE000016",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x11},
    "SRUE000017",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x12},
    "SRUE000018",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x13},
    "SRUE000019",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  },
  {
    {0xfe,0x01,0x14},
    "SRUE000020",
    {0xfe,0x01},
    0x11,
    0xfe,
    {0x01,0x00}
  }
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

class ConfigChannel : public Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t runtimeStatus;
  uint8_t runtimeFlags;
  bool    runtimeValid;

public:
  typedef Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ConfigChannel () : BaseChannel(), runtimeStatus(0), runtimeFlags(0), runtimeValid(false) {}
  virtual ~ConfigChannel () {}

  // AskSin++ uses these values for CONFIG_STATUS_REQUEST replies. Our shared
  // multi-HY loop also uses them for asynchronous INFO_ACTUATOR_STATUS updates
  // to the paired master/CCU.
  uint8_t status () const { return runtimeStatus; }
  uint8_t flags  () const { return runtimeFlags; }

  void runtimeState(uint8_t stat, uint8_t flg) {
    runtimeStatus = stat;
    runtimeFlags  = flg;
    runtimeValid  = true;
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


typedef MultiChannelDevice<Hal,ConfigChannel,NUM_CHANNELS,SwList0> ConfigDevice;

// -------------------------------------------------------------------------------------------------
// Compact 20-HY runtime
// -------------------------------------------------------------------------------------------------

enum HySlotFlags : uint8_t {
  HYS_HAVE_TC_TARGET = 0x01,
  HYS_SCHEDULE       = 0x02,
  HYS_LAST_NORMAL    = 0x04,
  HYS_AWAIT_VD_ACK   = 0x08,
  HYS_VD_LOST        = 0x10,
  HYS_CACHE_VALID    = 0x20,
  HYS_CCU_DIRTY      = 0x40,
  HYS_ENABLED        = 0x80
};

// POD only: this object lives in .noinit so it survives the direct application
// restart. No constructors must run over it.
struct HySlot {
  uint32_t nextVdBaseMs;
  uint32_t vdAckDeadlineMs;
  uint32_t lastVdSeenMs;

  uint8_t master[3];
  uint8_t tcPeer[3];
  uint8_t vdPeer[3];

  uint8_t flags;
  uint8_t currentTcCommand;
  uint8_t currentTcTargetRaw;
  uint8_t desiredVdTargetRaw;
  uint8_t lastSentVdTargetRaw;
  uint8_t linkBCounter;
  uint8_t awaitingVdCounter;
  uint8_t missCount;
  uint8_t collisionDebt;
  uint8_t lastVdPositionRaw;
  uint8_t lastVdStatus;
  uint8_t factor;          // effective pass factor 0..100; disabled => 100
  uint8_t specialMode;
  uint8_t ccuCounter;
  uint8_t txTryMax;
};

static_assert(sizeof(HySlot) <= 40, "HySlot unexpectedly large");

// Explicit prototypes for functions that use sketch-local types.
//
// Arduino's .ino preprocessor otherwise generates these prototypes near the
// top of the generated .cpp, before ConfigDevice/HySlot are defined. That
// causes exactly the "does not name a type" / "redeclared as different kind
// of symbol" errors seen with PlatformIO.
static ConfigDevice* activateConfigDevice(uint8_t index, bool attachHal);
static void advertisePairingBroadcast(ConfigDevice& d);
static bool firstPeer(ConfigDevice& d, uint8_t channelNo, uint8_t out[3]);
static uint8_t effectiveFactorFromConfig(ConfigDevice& d);
static uint8_t commandForNextVdSlot(const HySlot& s);
static uint8_t targetForNextVdSlot(const HySlot& s);
static void setAdapterRuntimeState(uint8_t index, ConfigDevice& d);

HySlot hySlots[LOGICAL_HY_COUNT] __attribute__((section(".noinit")));

// Exactly one physical radio means exactly one unsolicited CCU status transaction
// can be in flight.
static Message  pendingCcuStatusMsg;
static int8_t   pendingCcuOwner = -1;
static uint8_t  pendingCcuRetriesLeft = 0;
static uint32_t pendingCcuDeadlineMs = 0;

// Exactly one AskSin++ device object exists at any time. It is reconstructed in
// this buffer when a logical HY needs pairing/configuration service.
alignas(ConfigDevice) static uint8_t configDeviceStorage[sizeof(ConfigDevice)];
static ConfigDevice* configDevice = nullptr;
static int8_t configDeviceIndex = -1;
static bool configDeviceAttached = false;

// Final compact EEPROM layout for all 20 logical HYs.
//
// AskSin++ uses:
//   * 4 bytes StorageConfig immediately BEFORE the device base address
//   * currently 42 bytes from device base up to getUserStorage().getAddress()
//
// Therefore one logical HY needs exactly 46 bytes with the present
// AskSin++ 5.0.3 / non-AES / 2-channel configuration. The slots are tiled
// directly without overlap:
//
//   HY1  base 0x020
//   HY2  base 0x04E
//   ...
//   HY20 base 0x38A
//
// HY20 user-storage start is expected at 0x3B4 (= 948), leaving 72 bytes
// before the four-byte v25 layout marker at 0x3FC..0x3FF.
static const uint16_t DEVICE_EEPROM_BASE = 0x020;
static const uint16_t DEVICE_EEPROM_STRIDE = 46;
static const uint16_t HY25_LAYOUT_MAGIC_ADDR = 0x3FC;
static const uint8_t HY25_LAYOUT_MAGIC[4] = { 'H','Y','2','5' };

static_assert(DEVICE_EEPROM_BASE +
              (uint16_t)LOGICAL_HY_COUNT * DEVICE_EEPROM_STRIDE
              <= HY25_LAYOUT_MAGIC_ADDR,
              "20-HY EEPROM slots overlap the v25 layout marker");

Hal hal;

static HMID hyId(uint8_t index) {
  return HMID(0xfe, 0x01, (uint8_t)(index + 1U));
}

static int8_t hyIndexFromId(const HMID& id) {
  if (id.id0() != 0xfe || id.id1() != 0x01) return -1;
  uint8_t n = id.id2();
  return (n >= 1 && n <= LOGICAL_HY_COUNT) ? (int8_t)(n - 1U) : -1;
}

static void hmidToBytes(const HMID& id, uint8_t out[3]) {
  out[0] = id.id0();
  out[1] = id.id1();
  out[2] = id.id2();
}

static HMID bytesToHmid(const uint8_t in[3]) {
  return HMID(in[0], in[1], in[2]);
}

static bool idBytesValid(const uint8_t in[3]) {
  return (in[0] | in[1] | in[2]) != 0;
}

static bool idBytesEqual(const uint8_t in[3], const HMID& id) {
  return in[0] == id.id0() && in[1] == id.id1() && in[2] == id.id2();
}

static bool same3(const uint8_t a[3], const uint8_t b[3]) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static uint16_t deviceEepromAddr(uint8_t index) {
  return DEVICE_EEPROM_BASE + (uint16_t)index * DEVICE_EEPROM_STRIDE;
}

static void makeSerial(uint8_t index, uint8_t out[10]) {
  const char prefix[4] = {'S','R','U','E'};
  memcpy(out, prefix, 4);
  uint32_t n = (uint32_t)index + 1UL;
  for (int8_t p = 9; p >= 4; --p) {
    out[p] = (uint8_t)('0' + (n % 10UL));
    n /= 10UL;
  }
}

static bool serialMatches(uint8_t index, const uint8_t* serial) {
  uint8_t tmp[10];
  makeSerial(index, tmp);
  return memcmp(tmp, serial, 10) == 0;
}

static void destroyConfigDevice() {
  if (configDevice != nullptr) {
    configDevice->~ConfigDevice();
    configDevice = nullptr;
    configDeviceIndex = -1;
    configDeviceAttached = false;
  }
}

static ConfigDevice* activateConfigDevice(uint8_t index, bool attachHal) {
  if (index >= LOGICAL_HY_COUNT) return nullptr;

  if (configDevice == nullptr || configDeviceIndex != (int8_t)index) {
    destroyConfigDevice();
    configDevice = new (configDeviceStorage)
        ConfigDevice(hyDeviceInfo[index], deviceEepromAddr(index));
    configDevice->layoutChannels();
    configDeviceIndex = (int8_t)index;
  }

  if (attachHal && !configDeviceAttached) {
    configDevice->keystore().init();
    configDevice->setHal(hal);
    configDeviceAttached = true;
  }
  return configDevice;
}

static bool layoutMagicValid() {
  for (uint8_t i = 0; i < 4; ++i) {
    if (storage().getByte(HY25_LAYOUT_MAGIC_ADDR + i) != HY25_LAYOUT_MAGIC[i]) {
      return false;
    }
  }
  return true;
}

static void writeLayoutMagic() {
  for (uint8_t i = 0; i < 4; ++i) {
    storage().setByte(HY25_LAYOUT_MAGIC_ADDR + i, HY25_LAYOUT_MAGIC[i]);
  }
}

// Derive the real AskSin++ footprint from the actual ConfigDevice layout.
// getUserStorage().getAddress() points to the first byte after the persistent
// device/channel/peer data. StorageConfig occupies STORAGE_CFG_START bytes
// immediately before the device base.
static uint16_t configDeviceFootprint() {
  ConfigDevice* d = activateConfigDevice(0, false);
  if (d == nullptr) return 0xffff;
  const uint16_t forward =
      (uint16_t)(d->getUserStorage().getAddress() - deviceEepromAddr(0));
  return (uint16_t)STORAGE_CFG_START + forward;
}

static bool eepromLayoutSafe() {
  const uint16_t footprint = configDeviceFootprint();
  if (footprint == 0xffff || footprint > DEVICE_EEPROM_STRIDE) return false;

  ConfigDevice* last = activateConfigDevice(LOGICAL_HY_COUNT - 1U, false);
  if (last == nullptr) return false;

  return last->getUserStorage().getAddress() <= HY25_LAYOUT_MAGIC_ADDR;
}

// v25 is intentionally a clean layout transition. If the v25 magic is absent,
// all 20 logical devices are first-initialized at the compact addresses.
static bool ensurePersistentLayout() {
  if (!eepromLayoutSafe()) {
    Serial.println(F("EEP OVR"));
    while (true) { }
  }

  // eepromLayoutSafe() intentionally probes HY20 and therefore reconstructs
  // the reusable adapter. Re-activate HY1 before reading its checksum.
  ConfigDevice* d0 = activateConfigDevice(0, false);
  const bool storageFirst = storage().setup(d0->checksum());
  const bool initV25 = storageFirst || !layoutMagicValid();

  if (initV25) {
    // HY1 keeps base 0x020, therefore its physical-radio StorageConfig stays
    // at 0x01c..0x01f. Preserve these four bytes when migrating from an
    // existing installation so an RF frequency calibration is not lost.
    uint8_t radioConfig[STORAGE_CFG_START];
    if (!storageFirst) {
      for (uint8_t i = 0; i < STORAGE_CFG_START; ++i) {
        radioConfig[i] = storage().getByte(DEVICE_EEPROM_BASE - STORAGE_CFG_START + i);
      }
    }

    for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
      ConfigDevice* d = activateConfigDevice(i, false);
      d->firstinit();
    }

    if (!storageFirst) {
      for (uint8_t i = 0; i < STORAGE_CFG_START; ++i) {
        storage().setByte(DEVICE_EEPROM_BASE - STORAGE_CFG_START + i, radioConfig[i]);
      }
    }

    writeLayoutMagic();
    storage().store();
  }

  return initV25;
}

static bool firstPeer(ConfigDevice& d, uint8_t channelNo, uint8_t out[3]) {
  out[0] = out[1] = out[2] = 0;
  ConfigChannel& ch = d.channel(channelNo);
  for (uint8_t i = 0; i < ch.peers(); ++i) {
    Peer p = ch.peerat(i);
    if (p.valid()) {
      HMID id = p;
      hmidToBytes(id, out);
      return true;
    }
  }
  return false;
}

static uint8_t effectiveFactorFromConfig(ConfigDevice& d) {
  SwList1 l1 = d.channel(THERM_CHANNEL).getList1();
  if (!l1.enable()) return 100;
  uint8_t factor = l1.factor();
  return factor > 100 ? 100 : factor;
}

static uint8_t tcVisiblePositionRaw(uint8_t index);

static void recalcDesired(uint8_t index) {
  HySlot& s = hySlots[index];

  switch (s.currentTcCommand) {
    case CMD_REFRESH:
    case CMD_NEW_TARGET: {
      s.specialMode = HY_NORMAL;
      uint16_t tcPercent100 = decodeTcTargetPercent100(s.currentTcTargetRaw);
      uint16_t vdPercent100 =
          (uint16_t)(((uint32_t)tcPercent100 * s.factor + 50UL) / 100UL);
      s.desiredVdTargetRaw = encodeTcTargetPercent100(vdPercent100);
      break;
    }

    case CMD_VENT_OPEN:
      s.specialMode = HY_FULL_OPEN;
      s.desiredVdTargetRaw = s.currentTcTargetRaw;
      break;

    case CMD_VENT_CLOSED:
      s.specialMode = HY_VENT_CLOSED;
      s.desiredVdTargetRaw = 0;
      break;

    case CMD_DECALC:
    default:
      s.specialMode = HY_OTHER_SPECIAL;
      s.desiredVdTargetRaw = s.currentTcTargetRaw;
      break;
  }
}

static void syncSlotFromConfig(uint8_t index) {
  ConfigDevice* d = activateConfigDevice(index, true);
  if (d == nullptr) return;

  HySlot& s = hySlots[index];

  uint8_t newMaster[3] = {0,0,0};
  uint8_t newTc[3] = {0,0,0};
  uint8_t newVd[3] = {0,0,0};

  HMID master = d->getMasterID();
  hmidToBytes(master, newMaster);
  firstPeer(*d, THERM_CHANNEL, newTc);
  firstPeer(*d, VALVE_CHANNEL, newVd);

  const uint8_t newFactor = effectiveFactorFromConfig(*d);
  const bool factorChanged = s.factor != newFactor;
  const bool masterChanged = !same3(s.master, newMaster);
  const bool tcChanged = !same3(s.tcPeer, newTc);
  const bool vdChanged = !same3(s.vdPeer, newVd);

  memcpy(s.master, newMaster, 3);
  memcpy(s.tcPeer, newTc, 3);
  memcpy(s.vdPeer, newVd, 3);

  s.factor = newFactor;
  s.txTryMax = d->getList0().transmitDevTryMax();
  if (s.txTryMax == 0) s.txTryMax = 1;

  if (d->channel(THERM_CHANNEL).getList1().enable()) s.flags |= HYS_ENABLED;
  else s.flags &= ~HYS_ENABLED;

  if (factorChanged && (s.flags & HYS_HAVE_TC_TARGET)) {
    recalcDesired(index);
    if (s.flags & HYS_CACHE_VALID) s.flags |= HYS_CCU_DIRTY;
  }

  if (masterChanged && (s.flags & HYS_CACHE_VALID)) {
    s.flags |= HYS_CCU_DIRTY;
  }

  if (tcChanged) {
    // A new thermostat starts with a fresh target/Link-B acquisition phase.
    s.flags &= ~(HYS_HAVE_TC_TARGET | HYS_SCHEDULE | HYS_AWAIT_VD_ACK | HYS_LAST_NORMAL);
    s.nextVdBaseMs = 0;
  }

  if (vdChanged) {
    s.flags &= ~(HYS_SCHEDULE | HYS_AWAIT_VD_ACK | HYS_CACHE_VALID | HYS_VD_LOST);
    s.nextVdBaseMs = 0;
    s.lastVdSeenMs = idBytesValid(s.vdPeer) ? millis() : 0;
    s.collisionDebt = 0;
    s.missCount = 0;
  }
}

static void initColdSlots() {
  memset(hySlots, 0, sizeof(hySlots));
  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    HySlot& s = hySlots[i];
    s.currentTcCommand = CMD_REFRESH;
    s.factor = 100;
    s.specialMode = HY_NORMAL;
    s.linkBCounter = (uint8_t)(i * LINKB_COUNTER_STEP);
    s.ccuCounter = (uint8_t)(i * LINKB_COUNTER_STEP);
    s.txTryMax = 3;
    syncSlotFromConfig(i);
    if (idBytesValid(s.vdPeer)) s.lastVdSeenMs = millis();
  }
}

static uint32_t linkBIntervalMs(uint8_t index, uint8_t counter) {
  uint32_t address = 0xFE0100UL | (uint32_t)(index + 1U);
  uint32_t seed = (address << 8) | counter;
  uint32_t result = (uint32_t)(seed * 1103515245UL + 12345UL);
  result >>= 16;
  return (480UL + (result & 0xffUL)) * 250UL;
}

static void restoreSlotsAfterSoftRestart(uint32_t oldMillis) {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    HySlot& s = hySlots[i];

    // Any ACK transaction in flight is gone after the restart.
    s.flags &= ~HYS_AWAIT_VD_ACK;

    if (s.flags & HYS_SCHEDULE) {
      int32_t deadline = (int32_t)(s.nextVdBaseMs - oldMillis);
      while (deadline <= (int32_t)now) {
        deadline += (int32_t)calibrateLinkBInterval(linkBIntervalMs(i, s.linkBCounter));
        ++s.linkBCounter;
      }
      s.nextVdBaseMs = (uint32_t)deadline;
    }

    if (s.lastVdSeenMs != 0) {
      uint32_t age = oldMillis - s.lastVdSeenMs;
      s.lastVdSeenMs = now - age;
    }

    // A pending CCU transaction lives in normal BSS and is lost on restart.
    // Re-emit current cached state afterwards.
    if (s.flags & HYS_CACHE_VALID) s.flags |= HYS_CCU_DIRTY;
  }

  Serial.print(F("WS "));
  Serial.println(LOGICAL_HY_COUNT);
}

static uint8_t tcVisiblePositionRaw(uint8_t index) {
  HySlot& s = hySlots[index];
  if ((s.flags & HYS_CACHE_VALID) == 0) return 0;

  if (s.specialMode != HY_NORMAL) return s.lastVdPositionRaw;

  if (s.factor == 0) return 200;

  uint16_t vdPercent100 = decodeVdPositionPercent100(s.lastVdPositionRaw);
  uint32_t tcPercent100 =
      ((uint32_t)vdPercent100 * 100UL + s.factor / 2U) / s.factor;
  if (tcPercent100 > 10000UL) tcPercent100 = 10000UL;
  return encodeVdPositionPercent100((uint16_t)tcPercent100);
}

static bool writeFromSlot(uint8_t index, Message& msg, const HMID& to, bool addRpten=true) {
  msg.from(hyId(index));
  msg.to(to);
  if (addRpten) msg.setRpten();
  hal.prepareSend(msg);
  return hal.radio.write(msg, msg.burstRequired());
}

static bool sendAckStatusToTc(uint8_t index, const Message& request, uint8_t requestRssi) {
  HySlot& s = hySlots[index];

  if (s.flags & HYS_VD_LOST) {
    return false;
  }

  Message ack = request;
  ack.init(0x0e,
           request.count(),
           TYPE_RESPONSE,
           0x02,                    // WKMEUP; RPTEN is added below
           RESPONSE_ACK_STATUS,
           VD_CHANNEL);
  ack.data()[0] = tcVisiblePositionRaw(index);
  ack.data()[1] = (s.flags & HYS_CACHE_VALID) ? s.lastVdStatus : 0;
  ack.data()[2] = requestRssi;

  return writeFromSlot(index, ack, request.from(), true);
}

static bool sendRuntimeAckToVd(uint8_t index, const Message& request) {
  Message ack = request;
  ack.ack().init(0x00);             // keeps request counter
  return writeFromSlot(index, ack, request.from(), true);
}

static void noteValidVdRx(uint8_t index) {
  HySlot& s = hySlots[index];
  s.lastVdSeenMs = millis();

  if (s.flags & HYS_VD_LOST) {
    s.flags &= ~HYS_VD_LOST;
    Serial.print(F("V")); Serial.print(index + 1);
    Serial.println(F(" OK"));
  }
}

static void serviceVdWatchdog(uint8_t index, uint32_t now) {
  HySlot& s = hySlots[index];
  if (!idBytesValid(s.vdPeer)) return;

  if (s.lastVdSeenMs == 0) s.lastVdSeenMs = now;

  if ((s.flags & HYS_VD_LOST) == 0 &&
      (uint32_t)(now - s.lastVdSeenMs) >= VD_COMM_LOST_MS) {
    s.flags |= HYS_VD_LOST;
    Serial.print(F("V")); Serial.print(index + 1);
    Serial.println(F(" LOST"));
  }
}

static void startLinkBScheduleIfNeeded(uint8_t index) {
  HySlot& s = hySlots[index];
  if ((s.flags & HYS_SCHEDULE) ||
      (s.flags & HYS_HAVE_TC_TARGET) == 0 ||
      !idBytesValid(s.vdPeer)) {
    return;
  }

  uint8_t oldSreg = SREG;
  cli();
  s.nextVdBaseMs = millis();
  s.flags |= HYS_SCHEDULE;
  SREG = oldSreg;
}

static uint8_t commandForNextVdSlot(const HySlot& s) {
  if (s.specialMode == HY_NORMAL) {
    bool changed = (s.flags & HYS_LAST_NORMAL) == 0 ||
                   s.desiredVdTargetRaw != s.lastSentVdTargetRaw;
    return changed ? CMD_NEW_TARGET : CMD_REFRESH;
  }
  if (s.specialMode == HY_VENT_CLOSED) return CMD_VENT_CLOSED;
  return s.currentTcCommand;
}

static uint8_t targetForNextVdSlot(const HySlot& s) {
  return s.specialMode == HY_VENT_CLOSED ? 0 : s.desiredVdTargetRaw;
}

static bool linkBDue(uint8_t index, uint32_t now) {
  HySlot& s = hySlots[index];
  return (s.flags & HYS_SCHEDULE) && timeReached(now, s.nextVdBaseMs);
}

static bool linkBSlotWithin(uint8_t index, uint32_t now, uint16_t windowMs) {
  HySlot& s = hySlots[index];
  if ((s.flags & HYS_SCHEDULE) == 0) return false;
  if (timeReached(now, s.nextVdBaseMs)) return true;
  return (uint32_t)(s.nextVdBaseMs - now) <= (uint32_t)windowMs;
}

static void advanceLinkBSlotWithoutTx(uint8_t index) {
  HySlot& s = hySlots[index];
  if ((s.flags & HYS_SCHEDULE) == 0) return;

  uint8_t counter = s.linkBCounter;
  uint32_t interval = calibrateLinkBInterval(linkBIntervalMs(index, counter));
  uint32_t next = s.nextVdBaseMs + interval;

  uint8_t oldSreg = SREG;
  cli();
  s.nextVdBaseMs = next;
  s.linkBCounter = (uint8_t)(counter + 1U);
  if (s.collisionDebt != 0xff) ++s.collisionDebt;
  SREG = oldSreg;

  Serial.print(F("K")); Serial.print(index + 1);
  Serial.print(' '); Serial.println(counter, HEX);
}

static bool sendLinkBSlot(uint8_t index) {
  HySlot& s = hySlots[index];
  if (!idBytesValid(s.vdPeer)) {
    s.flags &= ~(HYS_SCHEDULE | HYS_AWAIT_VD_ACK);
    return false;
  }

  const uint8_t counter = s.linkBCounter;
  const uint8_t command = commandForNextVdSlot(s);
  const uint8_t target = targetForNextVdSlot(s);
  const uint32_t interval =
      calibrateLinkBInterval(linkBIntervalMs(index, counter));
  const uint32_t next = s.nextVdBaseMs + interval;

  Message out;
  out.init(0x0b,
           counter,
           TYPE_CLIMATE_EVENT,
           CTRL_TC_TO_VD,
           command,
           target);

  // Commit the deterministic next state BEFORE radio access. If TX left the
  // FIFO and the radio later wedges, recovery continues at the next slot.
  uint8_t oldSreg = SREG;
  cli();
  s.nextVdBaseMs = next;
  s.linkBCounter = (uint8_t)(counter + 1U);
  SREG = oldSreg;

  bool sent = writeFromSlot(index, out, bytesToHmid(s.vdPeer), false);

  if (sent) {
    s.vdAckDeadlineMs = millis() + VD_ACK_TIMEOUT_MS;
    s.awaitingVdCounter = counter;
    s.flags |= HYS_AWAIT_VD_ACK;
  }
  else {
    s.flags &= ~HYS_AWAIT_VD_ACK;
    if (s.missCount != 0xff) ++s.missCount;
  }

  if (s.specialMode == HY_NORMAL) {
    s.flags |= HYS_LAST_NORMAL;
    s.lastSentVdTargetRaw = target;
  }
  else {
    s.flags &= ~HYS_LAST_NORMAL;
  }

  Serial.print(F("B")); Serial.print(index + 1);
  Serial.print(' '); Serial.print(counter, HEX);
  Serial.print(' '); Serial.println(sent ? 1 : 0);
  return sent;
}

static bool handleTcClimate(uint8_t index, Message& msg) {
  HySlot& s = hySlots[index];
  if (msg.type() != TYPE_CLIMATE_EVENT || msg.length() < 0x0b) return false;
  if (!idBytesEqual(s.tcPeer, msg.from())) return false;

  const uint8_t requestRssi = hal.radio.rssi();

  s.flags |= HYS_HAVE_TC_TARGET;
  s.currentTcCommand = msg.command();
  s.currentTcTargetRaw = msg.subcommand();
  recalcDesired(index);

  bool ackSent = false;
  if ((s.flags & HYS_VD_LOST) == 0) {
    delay(LINK_A_RESPONSE_DELAY_MS);
    ackSent = sendAckStatusToTc(index, msg, requestRssi);
  }

  startLinkBScheduleIfNeeded(index);

  Serial.print(F("T")); Serial.print(index + 1);
  Serial.print(' '); Serial.print(msg.count(), HEX);
  Serial.print(' '); Serial.println(ackSent ? 1 : 0);
  return true;
}

static bool handleVdAckStatus(uint8_t index, Message& msg) {
  HySlot& s = hySlots[index];
  if (!idBytesEqual(s.vdPeer, msg.from())) return false;
  if (msg.type() != TYPE_RESPONSE || msg.length() < 0x0e) return false;
  if (msg.command() != RESPONSE_ACK_STATUS || msg.subcommand() != VD_CHANNEL) return false;

  bool changed = (s.flags & HYS_CACHE_VALID) == 0 ||
                 s.lastVdPositionRaw != msg.data()[0] ||
                 s.lastVdStatus != msg.data()[1];

  s.lastVdPositionRaw = msg.data()[0];
  s.lastVdStatus = msg.data()[1];
  s.flags |= HYS_CACHE_VALID;
  s.missCount = 0;
  noteValidVdRx(index);

  bool outstanding =
      (s.flags & HYS_AWAIT_VD_ACK) && msg.count() == s.awaitingVdCounter;

  if (outstanding) {
    s.flags &= ~HYS_AWAIT_VD_ACK;
    if (s.collisionDebt != 0) s.collisionDebt = 0;
  }

  if (changed) s.flags |= HYS_CCU_DIRTY;

  Serial.print(F("D")); Serial.print(index + 1);
  Serial.print(' '); Serial.print(msg.count(), HEX);
  Serial.print(' '); Serial.println(outstanding ? 1 : 0);
  return true;
}

static bool handleVdRuntime(uint8_t index, Message& msg) {
  HySlot& s = hySlots[index];
  if (!idBytesEqual(s.vdPeer, msg.from()) || msg.type() != TYPE_VALVE_RUNTIME) {
    return false;
  }

  noteValidVdRx(index);

  bool ack = true;
  if (msg.ackRequired()) {
    delay(VD_RUNTIME_ACK_DELAY_MS);
    hyRadioStage = 0x10;
    ack = sendRuntimeAckToVd(index, msg);
    hyRadioStage = 0;
  }

  Serial.print(F("X")); Serial.print(index + 1);
  Serial.print(' '); Serial.print(msg.count(), HEX);
  Serial.print(' '); Serial.println(ack ? 1 : 0);
  return true;
}

static void serviceSlotRuntime(uint8_t index) {
  HySlot& s = hySlots[index];
  uint32_t now = millis();

  if ((s.flags & HYS_AWAIT_VD_ACK) && timeReached(now, s.vdAckDeadlineMs)) {
    s.flags &= ~HYS_AWAIT_VD_ACK;
    if (s.missCount != 0xff) ++s.missCount;

    Serial.print(F("A")); Serial.print(index + 1);
    Serial.print(' '); Serial.print(s.awaitingVdCounter, HEX);
    Serial.print(' '); Serial.println(s.missCount);
  }

  startLinkBScheduleIfNeeded(index);
  serviceVdWatchdog(index, now);
}

// -------------------------------------------------------------------------------------------------
// Central 20-slot Link-B scheduler
// -------------------------------------------------------------------------------------------------

static uint8_t lastLinkBWinner = 0xff;

static int8_t findVdAckOwner() {
  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (hySlots[i].flags & HYS_AWAIT_VD_ACK) return (int8_t)i;
  }
  return -1;
}

static bool anyVdAckPending() {
  return findVdAckOwner() >= 0;
}

static uint8_t roundRobinDistance(uint8_t index) {
  if (lastLinkBWinner == 0xff) return index;
  uint8_t first = (uint8_t)((lastLinkBWinner + 1U) % LOGICAL_HY_COUNT);
  return (uint8_t)((index + LOGICAL_HY_COUNT - first) % LOGICAL_HY_COUNT);
}

static void serviceLinkBScheduler() {
  uint32_t now = millis();
  int8_t ackOwner = findVdAckOwner();

  if (ackOwner >= 0) {
    for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
      if (i == (uint8_t)ackOwner) continue;
      if (linkBDue(i, now)) advanceLinkBSlotWithoutTx(i);
    }
    return;
  }

  int8_t winner = -1;

  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (!linkBDue(i, now)) continue;

    if (winner < 0) {
      winner = (int8_t)i;
      continue;
    }

    HySlot& a = hySlots[i];
    HySlot& b = hySlots[(uint8_t)winner];

    if (a.collisionDebt > b.collisionDebt) {
      winner = (int8_t)i;
    }
    else if (a.collisionDebt == b.collisionDebt) {
      int32_t dt = (int32_t)(a.nextVdBaseMs - b.nextVdBaseMs);
      if (dt < 0 ||
          (dt == 0 && roundRobinDistance(i) <
                      roundRobinDistance((uint8_t)winner))) {
        winner = (int8_t)i;
      }
    }
  }

  if (winner < 0) return;

  HySlot& chosen = hySlots[(uint8_t)winner];

  // Protect a higher-debt HY whose exact slot is about to enter the ACK window.
  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (i == (uint8_t)winner) continue;
    HySlot& other = hySlots[i];
    if ((other.flags & HYS_SCHEDULE) == 0) continue;
    if (other.collisionDebt <= chosen.collisionDebt) continue;

    if (!timeReached(now, other.nextVdBaseMs) &&
        (uint32_t)(other.nextVdBaseMs - now) <= LINKB_COLLISION_GUARD_MS) {
      advanceLinkBSlotWithoutTx((uint8_t)winner);
      return;
    }
  }

  if (sendLinkBSlot((uint8_t)winner)) {
    lastLinkBWinner = (uint8_t)winner;
  }
}

static bool anyLinkBSlotWithin(uint32_t now, uint16_t windowMs) {
  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (linkBSlotWithin(i, now, windowMs)) return true;
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// Asynchronous CCU actuator status (one shared transaction)
// -------------------------------------------------------------------------------------------------

class SlotStatusView {
  uint8_t stat;
  uint8_t flg;
public:
  SlotStatusView(uint8_t s, uint8_t f) : stat(s), flg(f) {}
  uint8_t number() const { return THERM_CHANNEL; }
  uint8_t status() const { return stat; }
  uint8_t flags() const { return flg; }
  void patchStatus(Message&) {}
  void changed(bool) {}
};

static bool transmitPendingCcuStatus() {
  if (pendingCcuOwner < 0 || pendingCcuRetriesLeft == 0) return false;

  bool sent = hal.radio.write(pendingCcuStatusMsg,
                              pendingCcuStatusMsg.burstRequired());
  --pendingCcuRetriesLeft;
  pendingCcuDeadlineMs = millis() + CCU_STATUS_ACK_TIMEOUT_MS;
  return sent;
}

static bool handlePendingCcuAck(uint8_t index, Message& msg) {
  if (pendingCcuOwner != (int8_t)index) return false;

  HySlot& s = hySlots[index];
  if (!idBytesEqual(s.master, msg.from()) ||
      msg.to() != hyId(index) ||
      msg.count() != pendingCcuStatusMsg.count()) {
    return false;
  }

  if (msg.isAck()) {
    pendingCcuOwner = -1;
    pendingCcuRetriesLeft = 0;
    return true;
  }

  if (msg.isNack()) {
    s.flags |= HYS_CCU_DIRTY;
    pendingCcuOwner = -1;
    pendingCcuRetriesLeft = 0;
    return true;
  }

  return false;
}

static bool queueCcuStatus(uint8_t index) {
  HySlot& s = hySlots[index];

  if (!idBytesValid(s.master) || (s.flags & HYS_CACHE_VALID) == 0) {
    s.flags &= ~HYS_CCU_DIRTY;
    return false;
  }

  SlotStatusView view(tcVisiblePositionRaw(index), s.lastVdStatus);
  uint8_t count = ++s.ccuCounter;

  InfoActuatorStatusMsg& pm = pendingCcuStatusMsg.infoActuatorStatus();
  pm.init(count, view, hal.radio.rssi());
  pendingCcuStatusMsg.to(bytesToHmid(s.master));
  pendingCcuStatusMsg.from(hyId(index));
  pendingCcuStatusMsg.setRpten();
  hal.prepareSend(pendingCcuStatusMsg);

  pendingCcuOwner = (int8_t)index;
  pendingCcuRetriesLeft = s.txTryMax ? s.txTryMax : 1;
  s.flags &= ~HYS_CCU_DIRTY;

  transmitPendingCcuStatus();
  return true;
}

static void serviceCcuStatus(bool allowTx) {
  if (pendingCcuOwner >= 0) {
    if (!timeReached(millis(), pendingCcuDeadlineMs) || !allowTx) return;

    if (pendingCcuRetriesLeft > 0) {
      transmitPendingCcuStatus();
      return;
    }

    uint8_t owner = (uint8_t)pendingCcuOwner;
    hySlots[owner].flags |= HYS_CCU_DIRTY;
    pendingCcuOwner = -1;
    return;
  }

  if (!allowTx) return;

  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (hySlots[i].flags & HYS_CCU_DIRTY) {
      if (queueCcuStatus(i)) return;
    }
  }
}

// -------------------------------------------------------------------------------------------------
// Reusable AskSin++ configuration adapter
// -------------------------------------------------------------------------------------------------

static void setAdapterRuntimeState(uint8_t index, ConfigDevice& d) {
  HySlot& s = hySlots[index];
  d.channel(THERM_CHANNEL).runtimeState(
      (s.flags & HYS_CACHE_VALID) ? tcVisiblePositionRaw(index) : 0,
      (s.flags & HYS_CACHE_VALID) ? s.lastVdStatus : 0);
  d.channel(VALVE_CHANNEL).runtimeState(0,0);
}

static bool processConfigMessage(uint8_t index, Message& msg) {
  ConfigDevice* d = activateConfigDevice(index, true);
  if (d == nullptr) return false;

  setAdapterRuntimeState(index, *d);

  // Runtime Link A/B uses explicit timing. Generic CCU/config replies keep the
  // traditional ~100 ms delay.
  hal.radio.setSendTimeout(CONFIG_RESPONSE_DELAY_MS);
  bool handled = d->process(msg);

  syncSlotFromConfig(index);
  return handled;
}

// Pairing campaign for all 20 logical identities.
//
// After >=10 minutes without a button press a fresh campaign is created from a
// snapshot of the local pairing state:
//
//   phase U: devices that were unpaired in the snapshot
//   phase P: devices that were already paired in the snapshot
//
// U + P contain every HY exactly once. After that the automatic chain STOPS.
//
// A further button press within 10 minutes starts one numeric pass:
//
//   phase A: HY1 -> HY2 -> ... -> HY20
//
// Phase A also stops after HY20. Further button presses within 10 minutes may
// start another numeric pass. After >=10 minutes the next button starts a new
// U/P snapshot again.
//
// If a HY times out, the chain stops on exactly that HY. A button press within
// 10 minutes retries it. A button after >=10 minutes discards the old position
// and starts a fresh U/P snapshot.
//
// IMPORTANT: pairing advertisements are always broadcast, even for a locally
// paired HY. AskSin++ startPairing() normally sends DeviceInfo to getMasterID(),
// which makes a paired device address the remembered CCU directly; that CCU can
// answer even outside teach-in mode. Broadcasting gives us a meaningful
// "CCU is listening for teach-in" timeout again.
enum PairPhase : uint8_t {
  PAIR_PHASE_INITIAL_UNPAIRED = 0,
  PAIR_PHASE_INITIAL_PAIRED   = 1,
  PAIR_PHASE_ALL              = 2
};

static bool      pairingCampaignValid = false;
static bool      pairingInitialPassDone = false;
static PairPhase pairingPhase = PAIR_PHASE_INITIAL_UNPAIRED;
static uint32_t  pairingInitialPairedMask = 0;
static uint8_t   pairingCursor = 0;

static int8_t    pairingSelectedIndex = -1;
static int8_t    pairingResumeIndex = -1;
static bool      pairingAttemptActive = false;
static bool      pairingTrafficSeen = false;
static bool      pairingWasAlreadyPaired = false;
static uint32_t  pairingAttemptDeadlineMs = 0;
static uint32_t  pairingNextAdvertiseMs = 0;
static uint32_t  pairingLastTrafficMs = 0;
static uint8_t   pairingButtonPressesPending = 0;

static bool      pairingHaveLastButton = false;
static uint32_t  pairingLastButtonMs = 0;

static bool slotPaired(uint8_t index) {
  return idBytesValid(hySlots[index].master);
}

static bool pairingMaskBit(uint8_t index) {
  return (pairingInitialPairedMask & ((uint32_t)1U << index)) != 0;
}

static void logPairPhase() {
  Serial.print(F("PH "));
  if (pairingPhase == PAIR_PHASE_INITIAL_UNPAIRED) Serial.println(F("U"));
  else if (pairingPhase == PAIR_PHASE_INITIAL_PAIRED) Serial.println(F("P"));
  else Serial.println(F("A"));
}

static void resetPairingCampaign() {
  pairingInitialPairedMask = 0;

  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (slotPaired(i)) {
      pairingInitialPairedMask |= ((uint32_t)1U << i);
    }
  }

  pairingCampaignValid = true;
  pairingInitialPassDone = false;
  pairingPhase = PAIR_PHASE_INITIAL_UNPAIRED;
  pairingCursor = 0;
  pairingSelectedIndex = -1;
  pairingResumeIndex = -1;
  pairingAttemptActive = false;
  pairingTrafficSeen = false;

  logPairPhase();
}

static bool pairPhaseContains(uint8_t index) {
  if (pairingPhase == PAIR_PHASE_INITIAL_UNPAIRED) {
    return !pairingMaskBit(index);
  }

  if (pairingPhase == PAIR_PHASE_INITIAL_PAIRED) {
    return pairingMaskBit(index);
  }

  return true;
}

// Return true and write the next candidate, or false when the current complete
// pass has exhausted all 20 logical HYs.
static bool nextPairCandidate(uint8_t& result) {
  while (true) {
    while (pairingCursor < LOGICAL_HY_COUNT) {
      const uint8_t index = pairingCursor++;
      if (pairPhaseContains(index)) {
        result = index;
        return true;
      }
    }

    if (pairingPhase == PAIR_PHASE_INITIAL_UNPAIRED) {
      pairingPhase = PAIR_PHASE_INITIAL_PAIRED;
      pairingCursor = 0;
      logPairPhase();
      continue;
    }

    // Initial U/P pass or numeric A pass is complete. Never wrap here.
    return false;
  }
}

static void finishPairingPass() {
  pairingAttemptActive = false;
  pairingSelectedIndex = -1;
  pairingResumeIndex = -1;

  if (pairingPhase == PAIR_PHASE_INITIAL_PAIRED) {
    pairingInitialPassDone = true;
  }

  Serial.println(F("P END"));
}

static void beginPairAttempt(uint8_t index) {
  pairingSelectedIndex = (int8_t)index;
  pairingAttemptActive = true;
  pairingTrafficSeen = false;
  pairingWasAlreadyPaired = slotPaired(index);
  pairingAttemptDeadlineMs = millis() + PAIR_ATTEMPT_TIMEOUT_MS;
  pairingNextAdvertiseMs = millis();
  pairingLastTrafficMs = millis();

  Serial.print(F("P "));
  Serial.println(index + 1);
}

static bool beginNextPairAttempt() {
  uint8_t next = 0;
  if (!nextPairCandidate(next)) {
    finishPairingPass();
    return false;
  }

  beginPairAttempt(next);
  return true;
}

static void startNumericPass() {
  pairingPhase = PAIR_PHASE_ALL;
  pairingCursor = 0;
  pairingResumeIndex = -1;
  logPairPhase();
  beginNextPairAttempt();
}

static void handlePairButtonPress() {
  const uint32_t now = millis();

  // Ten-minute reset is based on button inactivity.
  const bool freshCampaign =
      !pairingHaveLastButton ||
      (uint32_t)(now - pairingLastButtonMs) >= PAIRING_SESSION_MS;

  pairingHaveLastButton = true;
  pairingLastButtonMs = now;

  if (freshCampaign || !pairingCampaignValid) {
    resetPairingCampaign();
  }

  // Do not skip an attempt already in progress. The press only refreshes the
  // ten-minute button-activity timer.
  if (pairingAttemptActive) return;

  // Same campaign after a timeout: retry exactly the HY that did not complete.
  if (!freshCampaign &&
      pairingResumeIndex >= 0 &&
      pairingResumeIndex < (int8_t)LOGICAL_HY_COUNT) {
    beginPairAttempt((uint8_t)pairingResumeIndex);
    return;
  }

  // The initial U/P pass has already covered all 20 once. Within the same
  // ten-minute button window, every further press starts exactly one numeric
  // HY1..HY20 pass.
  if (!freshCampaign && pairingInitialPassDone) {
    startNumericPass();
    return;
  }

  pairingResumeIndex = -1;
  beginNextPairAttempt();
}

static void notePairingTraffic(uint8_t index) {
  if (!pairingAttemptActive) return;
  if (pairingSelectedIndex != (int8_t)index) return;

  if (!pairingTrafficSeen) {
    Serial.print(F("PT "));
    Serial.println(index + 1);
  }

  pairingTrafficSeen = true;
  pairingLastTrafficMs = millis();
}

// Always advertise as an unaddressed pairing broadcast.
//
// AskSin++ ChannelDevice::startPairing() calls sendDeviceInfo(), and that sends
// to getMasterID(). For a locally paired HY this is a directed packet to the
// remembered CCU, which the CCU may service even while teach-in mode is off.
// A broadcast reproduces the advertisement used by a truly unpaired device.
static void advertisePairingBroadcast(ConfigDevice& d) {
  d.sendDeviceInfo(HMID::broadcast, d.nextcount());
  d.led().set(LedStates::pairing);
  d.activity().stayAwake(seconds2ticks(20));
}

static void servicePairingSession() {
  const uint32_t now = millis();

  while (pairingButtonPressesPending > 0) {
    --pairingButtonPressesPending;
    handlePairButtonPress();
  }

  if (!pairingAttemptActive || pairingSelectedIndex < 0) return;

  const uint8_t index = (uint8_t)pairingSelectedIndex;
  ConfigDevice* d = activateConfigDevice(index, true);

  if (!pairingTrafficSeen && timeReached(now, pairingNextAdvertiseMs)) {
    advertisePairingBroadcast(*d);
    pairingNextAdvertiseMs = millis() + PAIR_REANNOUNCE_MS;
  }

  if (pairingTrafficSeen) {
    bool complete = false;

    if (pairingWasAlreadyPaired || slotPaired(index)) {
      complete =
          (uint32_t)(now - pairingLastTrafficMs) >= PAIR_FINISH_QUIET_MS;
    }

    if (complete) {
      Serial.print(F("P+ "));
      Serial.println(index + 1);

      pairingAttemptActive = false;
      pairingSelectedIndex = -1;
      pairingResumeIndex = -1;
      beginNextPairAttempt();
      return;
    }
  }

  if (timeReached(now, pairingAttemptDeadlineMs)) {
    Serial.print(F("P! "));
    Serial.println(index + 1);

    // Stop here. Within ten minutes the next button retries exactly this HY.
    // After ten minutes a fresh U/P snapshot is created instead.
    pairingResumeIndex = (int8_t)index;
    pairingAttemptActive = false;
  }
}

class MultiHyConfigButton : public Button {
public:
  MultiHyConfigButton() : Button() {
    this->setLongPressTime(seconds2ticks(3));
  }

  virtual void state(uint8_t s) {
    uint8_t old = Button::state();
    Button::state(s);

    if (s == Button::released) {
      if (pairingButtonPressesPending != 0xff) ++pairingButtonPressesPending;
    }
    else if (s == Button::longpressed) {
      if (old == Button::longpressed) {
        ConfigDevice* d = activateConfigDevice(0, true);
        d->reset(); // invalidates shared storage; next boot first-inits all 20
      }
      else {
        hal.led.set(LedStates::key_long);
      }
    }
  }
};

MultiHyConfigButton cfgBtn;

// -------------------------------------------------------------------------------------------------
// One physical RX dispatcher
// -------------------------------------------------------------------------------------------------

static uint32_t lastSharedRadioRxMs = 0;

static bool isOwnHyId(const HMID& id) {
  return hyIndexFromId(id) >= 0;
}

static int8_t findSlotByTcPeer(const HMID& sender) {
  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    if (idBytesEqual(hySlots[i].tcPeer, sender)) return (int8_t)i;
  }
  return -1;
}

static bool dispatchSharedRadioMessage(Message& msg) {
  if (isOwnHyId(msg.from())) return false;

  int8_t addressed = hyIndexFromId(msg.to());
  if (addressed >= 0) {
    uint8_t index = (uint8_t)addressed;

    if (handlePendingCcuAck(index, msg)) return true;

    if (idBytesEqual(hySlots[index].tcPeer, msg.from()) &&
        msg.type() == TYPE_CLIMATE_EVENT) {
      return handleTcClimate(index, msg);
    }

    if (idBytesEqual(hySlots[index].vdPeer, msg.from())) {
      if (msg.type() == TYPE_RESPONSE && handleVdAckStatus(index, msg)) {
        return true;
      }
      if (msg.type() == TYPE_VALVE_RUNTIME && handleVdRuntime(index, msg)) {
        return true;
      }
    }

    notePairingTraffic(index);
    return processConfigMessage(index, msg);
  }

  // Pair-by-serial is a broadcast; bind only the requested virtual identity.
  if (msg.isPairSerial()) {
    for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
      if (serialMatches(i, msg.data())) {
        notePairingTraffic(i);
        return processConfigMessage(i, msg);
      }
    }
    return false;
  }

  // TC WEATHER_EVENT uses destination 000000. It belongs to the HY whose CH1
  // peer matches the sender and is intentionally not forwarded to the VD.
  if (isBroadcastHMID(msg.to()) && msg.type() == TYPE_WEATHER_EVENT) {
    return findSlotByTcPeer(msg.from()) >= 0;
  }

  return false;
}

static bool pollSharedRadio() {
  Message msg;
  uint8_t num = hal.radio.read(msg);
  if (num >= 10) {
    lastSharedRadioRxMs = millis();
    return dispatchSharedRadioMessage(msg);
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// Setup / loop
// -------------------------------------------------------------------------------------------------

void setup() {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  Serial.begin(57600);
  Serial.println(F("HY v25c C20 E46"));

  const bool softRecovery = (hySoftWatchdogMagic == HY_SOFT_WDT_MAGIC)
                         && (hyResetCause == 0);
  const uint32_t recoveryOldMillis = hyWatchdogOldMillis;
  const uint8_t recoveryRadioStage = hyRecoveredRadioStage;

  bool first = ensurePersistentLayout();

  // Initialize the one physical HAL. HY1 remains at base 0x020, so its
  // four-byte radio StorageConfig stays at the traditional 0x01c..0x01f area.
  HMID primary = hyId(0);
  hal.init(primary);

  ConfigDevice* d0 = activateConfigDevice(0, true);
  hal.config(d0->getConfigArea());
  hal.radio.configureRxBandwidth();

  if (softRecovery) {
    Serial.print(F("W "));
    Serial.print(recoveryRadioStage, HEX);
    Serial.print(' ');
    Serial.println(recoveryOldMillis);

    restoreSlotsAfterSoftRestart(recoveryOldMillis);

    // Reconcile EEPROM after soft recovery. A configuration transaction may
    // have completed immediately before the radio fault; the compact runtime
    // state must therefore be refreshed from persistent List0/List1/peers.
    for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
      syncSlotFromConfig(i);
    }

    hySoftWatchdogMagic = 0;
    hyRecoveredRadioStage = 0;
    hyWatchdogOldMillis = 0;
  }
  else {
    initColdSlots();
  }

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    delay(3500);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      ConfigDevice* d = activateConfigDevice(0, true);
      d->reset();
    }
  }
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);

  if (first) Serial.println(F("EEP V25"));

  Serial.print(F("MEM "));
  Serial.print(sizeof(HySlot));
  Serial.print(' ');
  Serial.print(sizeof(hySlots));
  Serial.print(' ');
  Serial.println(sizeof(ConfigDevice));

  Serial.print(F("ES "));
  Serial.println(configDeviceFootprint());

  // Verify the actual AskSin++ storage end of the last logical HY at runtime.
  // It must remain below 1024 on the ATmega328P.
  ConfigDevice* eeProbe = activateConfigDevice(LOGICAL_HY_COUNT - 1U, true);
  Serial.print(F("EE "));
  Serial.println(eeProbe->getUserStorage().getAddress());

  for (uint8_t i = 0; i < 2; ++i) {
    Serial.print(F("S")); Serial.print(i + 1);
    Serial.print(F(" F")); Serial.print(hySlots[i].factor);
    Serial.print(F(" T")); Serial.print(idBytesValid(hySlots[i].tcPeer) ? 1 : 0);
    Serial.print(F(" V")); Serial.print(idBytesValid(hySlots[i].vdPeer) ? 1 : 0);
    Serial.print(F(" M")); Serial.println(idBytesValid(hySlots[i].master) ? 1 : 0);
  }

  hal.battery.init(seconds2ticks(60UL * 60), sysclock);

  enableHySoftWatchdog();
  wdt_reset();
  Serial.println(F("WD8"));

  if (!softRecovery) {
    hySoftWatchdogMagic = 0;
    hyRecoveredRadioStage = 0;
    hyWatchdogOldMillis = 0;
  }

  pendingCcuOwner = -1;
  pendingCcuRetriesLeft = 0;
  hyRadioStage = 0;
}

void loop() {
  wdt_reset();
  hal.runready();

  pollSharedRadio();
  servicePairingSession();

  for (uint8_t i = 0; i < LOGICAL_HY_COUNT; ++i) {
    // Completely empty HYs need no runtime work.
    if (!idBytesValid(hySlots[i].tcPeer) &&
        !idBytesValid(hySlots[i].vdPeer) &&
        (hySlots[i].flags & HYS_HAVE_TC_TARGET) == 0) {
      continue;
    }
    serviceSlotRuntime(i);
  }

  serviceLinkBScheduler();

  uint32_t now = millis();
  bool radioQuietForCcu =
      (uint32_t)(now - lastSharedRadioRxMs) >= CCU_STATUS_RADIO_QUIET_MS;

  bool allowCcuTx = !anyVdAckPending() &&
                    radioQuietForCcu &&
                    !anyLinkBSlotWithin(now, CCU_STATUS_ACK_TIMEOUT_MS);

  serviceCcuStatus(allowCcuTx);

  wdt_reset();
}
