//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++ proxy for thermostat <-> valve
// Pairing/config is handled by normal AskSin++ base logic.
// Runtime proxying:
//   - Thermostat peer (CH1) -> this device, type 0x58  => forward raw to valve peer (CH2)
//   - Valve peer      (CH2) -> this device, type 0x02/0x10 => forward raw to thermostat peer (CH1)
// Everything else is passed to the AskSin++ base implementation.
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

// Pairing troubleshooting: disable AES to avoid key/index mismatch during teach-in.
// #define USE_AES
// #define HM_DEF_KEY 0x5f,0x8f,0xe3,0x01,0xaf,0x69,0x38,0xcf,0x1f,0xc1,0xca,0x6c,0x3d,0xf0,0x4b,0x01
// #define HM_DEF_KEY_INDEX 2
#define USE_WOR

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

const struct DeviceInfo PROGMEM devinfo = {
  {0xfe,0x01,0x02},
  "SRUE000002",
  {0xfe,0x01},
  0x11,
  0xfe,
  {0x01,0x00}
};

typedef AvrSPI<10,11,12,13> RadioSPI;
typedef AskSin<StatusLed<LED_PIN>,BatterySensor,Radio<RadioSPI,2> > Hal;

static uint8_t pct255(uint8_t v255) {
  return (uint8_t)((uint16_t)v255 * 100 / 255);
}
static uint8_t pct200(uint8_t v200) {
  return (uint8_t)((uint16_t)v200 * 100 / 200);
}
static bool isBroadcastHMID(const HMID& id) {
  return id.id0() == 0x00 && id.id1() == 0x00 && id.id2() == 0x00;
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

DEFREGISTER(Reg1,0x01,0x02)
class SwList1 : public RegList1<Reg1> {
public:
  SwList1 (uint16_t addr) : RegList1<Reg1>(addr) {}

  bool enable () const { return this->readRegister(0x01,0); }
  bool enable (bool v) const { return this->writeRegister(0x01,v); }

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

static void dumpMsg(const __FlashStringHelper* tag, const Message& msg) {
  DPRINT(tag);
  DPRINT(F(" len=0x")); DHEX(msg.length());
  DPRINT(F(" cnt=0x")); DHEX(msg.count());
  DPRINT(F(" flags=0x")); DHEX(msg.flags());
  DPRINT(F(" type=0x")); DHEX(msg.type());
  DPRINT(F(" from="));
  const HMID& f = msg.from();
  DHEX(f.id0()); DHEX(f.id1()); DHEX(f.id2());
  DPRINT(F(" to="));
  const HMID& t = msg.to();
  DHEX(t.id0()); DHEX(t.id1()); DHEX(t.id2());
  DPRINT(F(" raw: "));
  for (uint8_t i = 0; i < msg.length(); i++) {
    uint8_t b = msg.buffer()[i];
    if (b < 0x10) DPRINT('0');
    DHEX(b);
    DPRINT(' ');
  }
  DPRINTLN(F(""));
}

static void printHMIDValue(const HMID& id) {
  DHEX(id.id0()); DHEX(id.id1()); DHEX(id.id2());
}

class ProxyChannel : public Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> {
public:
  typedef Channel<Hal,SwList1,ProxyList3,ProxyList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ProxyChannel () : BaseChannel() {}
  virtual ~ProxyChannel () {}

  uint8_t status () const { return 0; }
  uint8_t flags  () const { return 0; }

  void configChanged() {
    DPRINT(F("ConfigChanged CH"));
    DPRINT(this->number());
    DPRINT(F(" - ENABLE: "));
    DPRINT(this->getList1().enable());
    DPRINT(F(" FACTOR: "));
    DPRINTLN(this->getList1().factor());
  }
};

template <uint8_t ChannelCount>
class ProxyDevice : public MultiChannelDevice<Hal,ProxyChannel,ChannelCount,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ProxyChannel,ChannelCount,SwList0> DevType;

  ProxyDevice(const DeviceInfo& i, uint16_t addr) : DevType(i,addr) {}
  virtual ~ProxyDevice() {}

  bool getFirstPeerOfChannel(uint8_t channelNo, HMID& peerid) {
    if (channelNo < 1 || channelNo > ChannelCount) return false;
    Peer p = this->channel(channelNo).peerat(0);
    if (!p.valid()) return false;
    peerid = p;
    return true;
  }

  bool isFromChannelPeer(uint8_t channelNo, const HMID& sender) {
    if (channelNo < 1 || channelNo > ChannelCount) return false;
    uint8_t pidx = this->channel(channelNo).peerfor(sender);
    if (pidx >= this->channel(channelNo).peers()) return false;
    Peer p = this->channel(channelNo).peerat(pidx);
    return p.valid();
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
        DPRINT(F("#"));
        DPRINT(i);
        DPRINT(F("="));
        HMID pid = p;
        printHMIDValue(pid);
        DPRINT(F(" "));
      }
      if (any == false) {
        DPRINT(F("<none>"));
      }
      DPRINTLN(F(""));
    }

    HMID first1;
    DPRINT(F("First peer CH1: "));
    if (getFirstPeerOfChannel(1, first1)) {
      printHMIDValue(first1);
      DPRINTLN(F(""));
    } else {
      DPRINTLN(F("<none>"));
    }

    HMID first2;
    DPRINT(F("First peer CH2: "));
    if (getFirstPeerOfChannel(2, first2)) {
      printHMIDValue(first2);
      DPRINTLN(F(""));
    } else {
      DPRINTLN(F("<none>"));
    }

    DPRINTLN(F("-------------------------------"));
  }

  void logThermMeta(Message& msg, bool toMe) {
    if (toMe) dumpMsg(F("RX-THERM->DEV"), msg);
    else      dumpMsg(F("RX-THERM(SNIFF)"), msg);

    DPRINT(F("  THERM meta: type=0x")); DHEX(msg.type());
    DPRINT(F(" flags=0x")); DHEX(msg.flags());
    DPRINT(F(" cnt=0x")); DHEX(msg.count());
    DPRINT(F(" len=0x")); DHEX(msg.length());
    if (msg.length() > 9)  { DPRINT(F(" p0=0x")); DHEX(msg.buffer()[9]); }
    if (msg.length() > 10) { DPRINT(F(" p1=0x")); DHEX(msg.buffer()[10]); }
    if (msg.length() > 11) { DPRINT(F(" p2=0x")); DHEX(msg.buffer()[11]); }
    DPRINTLN(F(""));

    if (msg.type() == 0x70 && msg.length() >= 12) {
      uint8_t p1 = msg.buffer()[10];
      DPRINT(F("  THERM 0x70: temp10≈"));
      DPRINT((uint16_t)p1 / 10);
      DPRINT(F("."));
      DPRINT((uint16_t)p1 % 10);
      DPRINTLN(F("C"));
    }
    else if (msg.type() == 0x58 && msg.length() >= 11) {
      uint8_t p0 = msg.buffer()[9];
      uint8_t p1 = msg.buffer()[10];
      DPRINT(F("  THERM 0x58: payload0=0x")); DHEX(p0);
      DPRINT(F(" payload1=0x")); DHEX(p1);
      if (p0 == 0x03) {
        DPRINT(F(" (~")); DPRINT(pct255(p1)); DPRINT(F("% 255)"));
      }
      DPRINTLN(F(""));
    }
  }

  void logValveMeta(Message& msg, bool toMe) {
    if (toMe) dumpMsg(F("RX-VALVE->DEV"), msg);
    else      dumpMsg(F("RX-VALVE(SNIFF)"), msg);

    if (msg.type() == 0x02 && msg.length() >= 14) {
      uint8_t ch      = msg.buffer()[9];
      uint8_t subcom  = msg.buffer()[10];
      uint8_t raw200  = msg.buffer()[11];
      uint8_t errByte = msg.buffer()[12];
      uint8_t extra   = msg.buffer()[13];

      DPRINT(F("  VALVE 0x02: ch=")); DPRINT(ch);
      DPRINT(F(" sub=0x")); DHEX(subcom);
      DPRINT(F(" raw200=")); DPRINT(raw200);
      DPRINT(F(" (~")); DPRINT(pct200(raw200)); DPRINT(F("%)"));
      DPRINT(F(" err=0x")); DHEX(errByte);
      DPRINT(F(" extra=0x")); DHEX(extra);
      DPRINTLN(F(""));
    }
    else if (msg.type() == 0x10) {
      DPRINTLN(F("  VALVE 0x10 received"));
    }
  }

  bool forwardRaw(Message& src, const HMID& newTo, bool forceBurst, const __FlashStringHelper* tag) {
    Message out;

    uint8_t len = src.length();
    if (len < 10 || len > 0x40) {
      DPRINT(F("  -> forward aborted: suspicious len="));
      DPRINTLN(len);
      return false;
    }

    uint8_t p1 = (len > 10) ? src.buffer()[10] : 0x00;
    out.init(len, src.count(), src.type(), src.flags(), 0x00, p1);

    HMID me;
    this->getDeviceID(me);
    out.from(me);
    out.to(newTo);

    for (uint8_t i = 9; i < len; ++i) {
      out.buffer()[i] = src.buffer()[i];
    }

    if (forceBurst) {
      out.burstRequired(true);
    }

    dumpMsg(tag, out);
    this->send(out, newTo);
    return true;
  }

  virtual bool process(Message& msg) {
    bool ownHandled = false;
    HMID me;
    this->getDeviceID(me);

    const bool toMe          = (msg.to() == me);
    const bool toBroadcast   = isBroadcastHMID(msg.to());
    const bool fromThermPeer = isFromChannelPeer(1, msg.from());
    const bool fromValvePeer = isFromChannelPeer(2, msg.from());
    // uint8_t mtype = msg.type();
    // uint8_t mcomm = msg.command();
    // uint8_t msubc = msg.subcommand();
    // DPRINT(F("type=0x")); DHEX(mtype);
    // DPRINT(F(" command=0x")); DHEX(mcomm);
    // DPRINT(F(" subcommand=0x")); DHEX(msubc);
    // DPRINTLN(F(""));

    const bool thermRuntimeToMe = fromThermPeer && toMe && (msg.type() == 0x58);
    const bool valveRuntimeToMe = fromValvePeer && toMe && (msg.type() == 0x02 || msg.type() == 0x10);

    if (thermRuntimeToMe) {
      logThermMeta(msg, true);

      HMID valve;
      if (getFirstPeerOfChannel(2, valve)) {
        DPRINT(F("  -> Forward THERM->VALVE: "));
        printHMIDValue(valve);
        DPRINT(F(" burst=")); DPRINTLN(1);
        forwardRaw(msg, valve, true, F("TX->VALVE"));
      } else {
        DPRINTLN(F("  -> no valve peer on CH2"));
      }
      ownHandled = true;
    }

    if (valveRuntimeToMe) {
      logValveMeta(msg, true);

      HMID therm;
      if (getFirstPeerOfChannel(1, therm)) {
        DPRINT(F("  -> Forward VALVE->THERM: "));
        printHMIDValue(therm);
        DPRINTLN(F(" burst=0"));
        forwardRaw(msg, therm, false, F("TX->THERM"));
      } else {
        DPRINTLN(F("  -> no thermostat peer on CH1"));
      }
      ownHandled = true;
    }

    if (fromThermPeer && !thermRuntimeToMe) {
      if (toBroadcast && msg.type() == 0x70) {
        logThermMeta(msg, false);
      }
    }
    else if (fromValvePeer && !valveRuntimeToMe) {
      logValveMeta(msg, false);
    }
    else {
      dumpMsg(F("pass->base"), msg);
    }

    // DPRINT(F("  -> base handled="));
    // DPRINTLN(handled);
    bool handled = DevType::process(msg);
    return handled || ownHandled;
  }
};

typedef ProxyDevice<NUM_CHANNELS> ProxyDeviceType;

Hal hal;
ProxyDeviceType sdev(devinfo, 0x20);
ConfigButton<ProxyDeviceType> cfgBtn(sdev, CONFIG_BUTTON_PIN);

void setup() {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  DPRINTLN(F("=== HB-SR-HY runtime proxy + base pairing ==="));
  DPRINTLN(F("Channel 1 = thermostat side"));
  DPRINTLN(F("Channel 2 = valve side"));
  DPRINTLN(F("Runtime forward: THERM 0x58 -> VALVE"));
  DPRINTLN(F("Runtime forward: VALVE 0x02/0x10 -> THERM"));
  DPRINTLN(F("0x70 is logged only"));
  DPRINTLN(F("Broadcast traffic is logged only"));
  DPRINTLN(F("Pairing / unpairing / config handled by AskSin++ base"));
  DPRINTLN(F("No periodic status, no async status, no synthetic test values"));

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
    DPRINTLN(F("Erstes Init - Device konfiguriert"));
  }

  sdev.initDone();
  sdev.dumpPeers();

  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60), sysclock);
}

void loop() {
  hal.runready();
  sdev.pollRadio();
  hal.activity.stayAwake(seconds2ticks(5));
}