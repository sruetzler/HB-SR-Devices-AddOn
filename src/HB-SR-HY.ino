//- -----------------------------------------------------------------------------------------------------------------------
// HB-SR-HY
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
//   * The real VD ACK_STATUS is cached asynchronously by normal pollRadio() processing.
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
//   * The exact original VD RX-window is unknown; VD_CYCLIC_OFFSET_MS must be validated on real hardware.
//   * Link-B phase/nextSendTime is not persisted across a HY power cycle. Without a wall clock, storing
//     millis() would not preserve the phase; the real VD may therefore need its normal resynchronisation.
//   * No synthetic "VD lost" error is invented for stale cache data because that status mapping is still open.
//   Pairing/configuration remains handled by the AskSin++ base implementation.
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

// Pairing troubleshooting: disable AES to avoid key/index mismatch during teach-in.
// #define USE_AES
// #define HM_DEF_KEY 0x5f,0x8f,0xe3,0x01,0xaf,0x69,0x38,0xcf,0x1f,0xc1,0xca,0x6c,0x3d,0xf0,0x4b,0x01
// #define HM_DEF_KEY_INDEX 2

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Switch.h>

#define LED_PIN 4
#define CONFIG_BUTTON_PIN 8

#define PEERS_PER_CHANNEL 4
#define NUM_CHANNELS 2

using namespace as;

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
// FHEM-compatible practical offset relative to the calculated Link-B base slot.
// The exact RX-window location of the original VD is still an empirical point.
static const uint16_t VD_CYCLIC_OFFSET_MS = 200;

// Real ACKs are normally seen after only a few milliseconds. This is only a
// bookkeeping timeout; it NEVER causes an immediate retransmission.
static const uint16_t VD_ACK_TIMEOUT_MS = 1000;

// Link-A timing measured directly on CC1101 GDO0 against a real TC/VD pair.
// A 95 ms software delay places HY's ACK in the receive window in which the
// original HM-CC-VD answers. This timing is required by the real HM-CC-TC.
static const uint16_t LINK_A_RESPONSE_DELAY_MS = 95;

// HY keeps its own two-channel device identity for the CCU. Link A still
// behaves protocol-wise like the valve side towards the real thermostat.
const struct DeviceInfo PROGMEM devinfo = {
  {0xfe,0x01,0x02},
  "SRUE000002",
  {0xfe,0x01},
  0x11,
  0xfe,
  {0x01,0x00}
};

typedef AvrSPI<10,11,12,13> RadioSPI;

// SENDDELAY is set to 0 deliberately. The generic AskSin++ Radio defaults to a
// 100 ms minimum interval between transmissions. HY must be able to answer a
// TC immediately even if Link B happened to transmit shortly before.
typedef AskSin<StatusLed<LED_PIN>,BatterySensor,Radio<RadioSPI,2,0xff,0> > Hal;

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

  // AskSin++ uses these values both for CONFIG_STATUS_REQUEST replies and for
  // unsolicited INFO_ACTUATOR_STATUS messages sent to the paired master/CCU.
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
      // ChannelDevice::pollRadio() will turn this into an
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
  uint32_t nextVdBaseMs;       // base slot, without the constant +offset
  bool     lastSentWasNormal;
  uint8_t  lastSentVdTargetRaw;

  bool     awaitingVdAck;
  uint8_t  awaitingVdCounter;
  uint32_t vdAckDeadlineMs;
  uint8_t  missCount;

  // Cache of the most recent real VD ACK_STATUS.
  bool     vdCacheValid;
  uint8_t  lastVdPositionRaw;  // scale /2
  uint8_t  lastVdStatus;
  uint8_t  lastVdRssi;
  uint32_t lastVdSeenMs;

public:
  ProxyDevice(const DeviceInfo& i, uint16_t addr)
    : DevType(i,addr),
      haveTcTarget(false),
      currentTcCommand(CMD_REFRESH),
      currentTcTargetRaw(0),
      specialMode(HY_NORMAL),
      desiredVdTargetRaw(0),
      lastEffectiveFactor(100),
      hyTcCounter(0),
      vdScheduleStarted(false),
      nextVdBaseMs(0),
      lastSentWasNormal(false),
      lastSentVdTargetRaw(0),
      awaitingVdAck(false),
      awaitingVdCounter(0),
      vdAckDeadlineMs(0),
      missCount(0),
      vdCacheValid(false),
      lastVdPositionRaw(0),
      lastVdStatus(0),
      lastVdRssi(0),
      lastVdSeenMs(0) {}

  virtual ~ProxyDevice() {}

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

    DPRINT(F("Link B schedule started: cnt=0x"));
    DHEX(hyTcCounter);
    DPRINT(F(" first offset="));
    DPRINT(VD_CYCLIC_OFFSET_MS);
    DPRINTLN(F("ms"));
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

  void sendLinkBSlot() {
    HMID valve;
    if (getFirstPeerOfChannel(VALVE_CHANNEL, valve) == false) {
      vdScheduleStarted = false;
      awaitingVdAck = false;
      return;
    }

    HMID hyTcAddress;
    this->getDeviceID(hyTcAddress);

    const uint8_t counter = hyTcCounter;
    const uint8_t command = commandForNextVdSlot();
    const uint8_t target  = targetForNextVdSlot();

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

    // One physical attempt only. No AskSin++ send(), no waitResponse(), no
    // automatic retry. The VD response is consumed asynchronously by process().
    // Capture the actual local send instant so a delayed loop iteration cannot
    // introduce a permanent phase error in all following slots.
    uint32_t txStartedMs = millis();
    bool sent = this->radio().write(out, false);

    // FHEM stores, after a successful transmission, the current message
    // counter together with the already calculated next send time. The
    // interval from a transmitted CNT=n to the following CNT=n+1 is therefore
    // timing(HY_TC_ID, n). The counter is incremented for the frame sent in the
    // next slot.
    // The practical FHEM-compatible cyclicMsgOffset is added by
    // serviceRuntime() on every cycle:
    //   next TX = txStartedMs + timing(HY_TC_ID, counter)
    //                            + VD_CYCLIC_OFFSET_MS
    uint32_t interval = linkBIntervalMs(hyTcAddress, counter);
    nextVdBaseMs = txStartedMs + interval;
    const uint8_t nextCounter = (uint8_t)(counter + 1U);
    hyTcCounter = nextCounter;

    if (sent) {
      awaitingVdAck = true;
      awaitingVdCounter = counter;
      vdAckDeadlineMs = millis() + VD_ACK_TIMEOUT_MS;
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

    // Logging comes after the timing-sensitive radio write.
    dumpMsg(F("TX HY-TC->VD CLIMATE_EVENT"), out);
    DPRINT(F("  LinkB sent=")); DPRINT(sent);
    DPRINT(F(" cnt=0x")); DHEX(counter);
    DPRINT(F(" cmd=0x")); DHEX(command);
    DPRINT(F(" targetRaw=0x")); DHEX(target);
    DPRINT(F(" nextCnt=0x")); DHEX(nextCounter);
    DPRINT(F(" nextBaseIn=")); DPRINT(interval);
    DPRINT(F("ms nextTxOffset=+")); DPRINT(VD_CYCLIC_OFFSET_MS);
    DPRINTLN(F("ms"));
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

    // The response to Link A must not depend on whether Link B is currently
    // awake, reachable, waiting for an ACK, or even has a peer configured.
    // Match the response latency measured directly against a real HM-CC-VD.
    // The old HM-CC-TC accepts the ACK only in this receive-window timing.
    if (LINK_A_RESPONSE_DELAY_MS > 0) {
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
    lastVdSeenMs      = millis();
    vdCacheValid      = true;
    missCount         = 0;

    // Make the real VD status visible to the HY's own CCU representation.
    // If position/status changed, AskSin++ will send INFO_ACTUATOR_STATUS to
    // MasterID after process() returns. CCU status requests also read this.
    updateLinkAChannelStateFromCache();

    bool matchesOutstanding = awaitingVdAck && (msg.count() == awaitingVdCounter);
    if (matchesOutstanding) {
      awaitingVdAck = false;
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
    }

    if (awaitingVdAck && timeReached(now, vdAckDeadlineMs)) {
      awaitingVdAck = false;
      if (missCount != 0xff) ++missCount;
      DPRINT(F("VD ACK timeout cnt=0x")); DHEX(awaitingVdCounter);
      DPRINT(F(" missCount=")); DPRINTLN(missCount);
      // Intentionally no retry. The normal cyclic schedule continues.
    }

    startLinkBScheduleIfNeeded();

    if (vdScheduleStarted) {
      uint32_t due = nextVdBaseMs + (uint32_t)VD_CYCLIC_OFFSET_MS;
      if (timeReached(now, due)) {
        sendLinkBSlot();
      }
    }
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

      // Observed VD runtime frames use e.g. CTRL=0xA4, which includes BIDI.
      // Since HY is the addressed peer, consuming such a frame without an ACK
      // makes the VD repeat it. Acknowledge it locally while keeping Link A/B
      // counters independent.
      if (msg.ackRequired()) {
        const uint8_t cnt = msg.count();
        this->sendAck(msg);
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

    // Pairing, config, CCU traffic and all unrelated messages retain the normal
    // AskSin++ behavior.
    return DevType::process(msg);
  }
};

typedef ProxyDevice<NUM_CHANNELS> ProxyDeviceType;

Hal hal;
ProxyDeviceType sdev(devinfo, 0x20);
ConfigButton<ProxyDeviceType> cfgBtn(sdev, CONFIG_BUTTON_PIN);

void setup() {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  DPRINTLN(F("=== HB-SR-HY stateful HM-CC-TC / HM-CC-VD protocol proxy ==="));
  DPRINTLN(F("CH1 = original thermostat peer"));
  DPRINTLN(F("CH2 = original valve peer"));
  DPRINTLN(F("Link A: cached real-VD ACK_STATUS via AskSin++ AckStatusMsg"));
  DPRINTLN(F("Link A ACK uses observed real-VD flags CTRL=0x82"));
  DPRINT(F("Link A measured response delay: +")); DPRINT(LINK_A_RESPONSE_DELAY_MS); DPRINTLN(F(" ms before ACK"));
  DPRINTLN(F("Link B: independent counter + deterministic 120..183.75 s timing"));
  DPRINT(F("Link B practical cyclic offset: +")); DPRINT(VD_CYCLIC_OFFSET_MS); DPRINTLN(F(" ms"));
  DPRINTLN(F("No immediate VD retry; real VD ACK is cached asynchronously"));
  DPRINTLN(F("VD cache is mirrored to CCU on CH1 via AskSin++ INFO_ACTUATOR_STATUS"));
  DPRINTLN(F("CCU status requests read the same CH1 shadow state"));
  DPRINTLN(F("Normal targets scaled; 01/02/04/unknown commands conservative/transparent"));
  DPRINTLN(F("List1 factor compatibility: factor=80 means reduction x=20%"));
  DPRINTLN(F("Restart note: Link-B phase is not persistent; VD may need normal resync"));
  DPRINTLN(F("HY identity: model=FE01 type=FE advertisedChannels=2"));

  bool first = sdev.init(hal);

  HMID me;
  sdev.getDeviceID(me);
  DPRINT(F("DeviceID: "));
  printHMIDValue(me);
  DPRINTLN(F(""));

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    DPRINTLN(F("Config button held at boot - hold for factory reset"));
    delay(3500);
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      DPRINTLN(F("Factory reset requested"));
      sdev.reset();
    }
  }

  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);

  if (first) {
    DPRINTLN(F("First init - device storage initialized"));
  }

  sdev.initDone();
  sdev.dumpPeers();
  HMID linkBPeer;
  if (sdev.getFirstPeerOfChannel(VALVE_CHANNEL, linkBPeer) == false) {
    DPRINTLN(F("WARNING: CH2 has no real-VD peer; Link-B cache will stay empty and Link-A reports 0% until a VD ACK is cached"));
  }

  // HY is mains powered and intentionally remains continuously receptive.
  // Do not enter an AskSin++ power-save mode in loop().
  hal.battery.init(seconds2ticks(60UL*60), sysclock);
}

void loop() {
  hal.runready();
  sdev.pollRadio();
  sdev.serviceRuntime();
}
