//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2018-09-29 jp112sdl Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

#define USE_AES
#define HM_DEF_KEY 0x5f,0x8f,0xe3,0x01,0xaf,0x69,0x38,0xcf,0x1f,0xc1,0xca,0x6c,0x3d,0xf0,0x4b,0x01
#define HM_DEF_KEY_INDEX 2
#define USE_WOR

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Switch.h>

// Pins
#define LED_PIN 4
#define CONFIG_BUTTON_PIN 8

#define PEERS_PER_CHANNEL 4
#define NUM_CHANNELS 2

using namespace as;

// DeviceInfo
const struct DeviceInfo PROGMEM devinfo = {
    {0xfe,0x01,0x01},       // Device ID
    "SRUE000001",           // Device Serial
    {0xfe,0x01},            // Device Model
    0x11,                   // Firmware Version
    0xfe,                   // Device Type
    {0x01,0x00}             // Info Bytes
};

// Hardware
typedef AvrSPI<10,11,12,13> RadioSPI;
typedef AskSin<StatusLed<LED_PIN>,BatterySensor,Radio<RadioSPI,2> > Hal;

// Helpers
static uint8_t map255to200(uint8_t v255) {
  uint16_t tmp = (uint16_t)v255 * 200 + 127;
  return (uint8_t)(tmp / 255);
}
static uint8_t pct255(uint8_t v255) {
  return (uint8_t)((uint16_t)v255 * 100 / 255);
}
static uint8_t pct200(uint8_t v200) {
  return (uint8_t)((uint16_t)v200 * 100 / 200);
}

// List0
DEFREGISTER(Reg0,DREG_INTKEY,DREG_LEDMODE,MASTERID_REGS,DREG_LOWBATLIMIT)
class SwList0 : public RegList0<Reg0> {
public:
  SwList0(uint16_t addr) : RegList0<Reg0>(addr) {}
  void defaults () {
    clear();
    lowBatLimit(22);
  }
};

// List1
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

// List3 / List4 (Peer flags)
DEFREGISTER(Reg3)
class ThermoList3 : public RegList3<Reg3> {
public:
  ThermoList3 (uint16_t addr) : RegList3<Reg3>(addr) {}
  bool peerNeedsBurst () const { return this->readBit(1, 0, false); }
  bool peerNeedsBurst (bool v) { return this->writeBit(1, 0, v); }
  void defaults () { clear(); }
};

DEFREGISTER(Reg4)
class ValveList4 : public RegList4<Reg4> {
public:
  ValveList4 (uint16_t addr) : RegList4<Reg4>(addr) {}
  bool peerNeedsBurst () const { return this->readBit(1, 0, false); }
  bool peerNeedsBurst (bool v) { return this->writeBit(1, 0, v); }
  void defaults () { clear(); }
};

// ------------------------------------------------------------
// ACK_EVENT (0x02) für Ventil-Emu
// On-air:
// [cnt][flags][type][from3][to3][channel][subcom][valveRaw][errBits][extra]
// ------------------------------------------------------------
class ValveEventMsg : public Message {
public:
  void init(uint8_t msgcnt, uint8_t channel, uint8_t valveRaw, uint8_t error, uint8_t extra) {
    if (valveRaw > 200) valveRaw = 200;
    initWithCount(0x0E, 0x02, 0x82, channel);
    cnt    = msgcnt;
    subcom = 0x01;
    pload[0] = valveRaw;
    pload[1] = (error & 0x07) << 1;
    pload[2] = extra;
  }
};

// ------------------------------------------------------------
// 0x58 Thermostat -> Ventil
// Rohaufbau, weil AskSin++ init/subcom hier nicht das gewünschte Byte-Layout liefert.
// Ziel on-air:
// [cnt][flags][type][from3][to3][0x03][level]
// ------------------------------------------------------------
class LevelSetMsg : public Message {
public:
  void init(uint8_t msgcnt, uint8_t level) {
    if (level > 250) level = 250;

    Message::init(0x0B, msgcnt, 0x58, 0xA2, 0x03, level);

    // zur Sicherheit explizit die beiden letzten Payload-Bytes setzen
    buffer()[9]  = 0x03;
    buffer()[10] = level;
  }
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

// ------------------------------------------------------------
// Channel
// Channel 1 = Ventil-Emu
// Channel 2 = Thermostat-Testsender
// ------------------------------------------------------------
class ConfigChannel : public Channel<Hal,SwList1,EmptyList,ValveList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t m_status;
  uint8_t m_valveRaw;   // 0..200
  uint8_t m_error;      // 0..4
  uint8_t m_extra;      // toggles 0x20/0x21

  class AsyncStatusAlarm : public Alarm {
    ConfigChannel& ch;
  public:
    AsyncStatusAlarm(ConfigChannel& c) : Alarm(0), ch(c) {}
    virtual ~AsyncStatusAlarm() {}
    virtual void trigger(AlarmClock&) { ch.sendAsyncStatusEvent(); }
  } asyncAlarm;

  class PeriodicStatusAlarm : public Alarm {
    ConfigChannel& ch;
  public:
    PeriodicStatusAlarm(ConfigChannel& c) : Alarm(0), ch(c) {}
    virtual ~PeriodicStatusAlarm() {}
    virtual void trigger(AlarmClock&) { ch.periodicStatusTrigger(); }
  } periodicAlarm;

  class TestValveCommandAlarm : public Alarm {
    ConfigChannel& ch;
  public:
    TestValveCommandAlarm(ConfigChannel& c) : Alarm(0), ch(c) {}
    virtual ~TestValveCommandAlarm() {}
    virtual void trigger(AlarmClock&) { ch.testValveCommandTrigger(); }
  } testAlarm;

  uint8_t m_testIndex;
  bool m_testEnabled;

public:
  typedef Channel<Hal,SwList1,EmptyList,ValveList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;

  ConfigChannel () :
    BaseChannel(),
    m_status(0),
    m_valveRaw(0),
    m_error(0),
    m_extra(0x21),
    asyncAlarm(*this),
    periodicAlarm(*this),
    testAlarm(*this),
    m_testIndex(0),
    m_testEnabled(false) {}

  virtual ~ConfigChannel () {}

  uint8_t status () const { return m_status; }
  uint8_t flags  () const { return 0; }

  void configChanged() {
    DPRINT(F("ConfigChanged - ENABLE: ")); DPRINTLN(this->getList1().enable());
    DPRINT(F("ConfigChanged - FACTOR: ")); DPRINTLN(this->getList1().factor());
  }

  bool isValveChannel() const {
    return this->number() == 1;
  }

  bool isThermostatChannel() const {
    return this->number() == 2;
  }

  void setError(uint8_t e) {
    if (e <= 4) {
      m_error = e;
      changed(true);
    }
  }

  void setFromThermostatValveRaw200(uint8_t raw200) {
    if (raw200 > 200) raw200 = 200;
    m_valveRaw = raw200;

    DPRINT(F("Ventilstellung übernommen: "));
    DPRINT(pct200(raw200));
    DPRINT(F("% (raw200: "));
    DPRINT(raw200);
    DPRINTLN(F(")"));
  }

  uint8_t nextExtra() {
    m_extra ^= 0x01;
    m_extra = (m_extra & 0x01) ? 0x21 : 0x20;
    return m_extra;
  }

  void scheduleAsyncStatusEvent() {
    if (!isValveChannel()) return;
    sysclock.cancel(asyncAlarm);
    asyncAlarm.set(millis2ticks(3000));
    sysclock.add(asyncAlarm);
    DPRINTLN(F("  -> Async Status-Event geplant in 3 Sekunden"));
  }

  void sendAsyncStatusEvent() {
    if (!isValveChannel()) return;

    DPRINTLN(F("=== ASYNC STATUS EVENT ==="));

    ValveEventMsg msg;
    uint8_t cnt = this->device().nextcount();
    uint8_t extra = nextExtra();
    msg.init(cnt, this->number(), m_valveRaw, m_error, extra);

    HMID me;
    this->device().getDeviceID(me);
    msg.from(me);

    HMID master = this->device().getMasterID();
    if (master.valid()) {
      msg.to(master);
      this->device().send(msg, master);
    }

    for (uint8_t i = 0; i < this->peers(); i++) {
      Peer p = this->peerat(i);
      if (!p.valid()) continue;

      HMID peerid = p;
      bool needsBurst = this->getList4(p).peerNeedsBurst();

      msg.to(peerid);
      msg.burstRequired(needsBurst);

      DPRINT(F("  -> Async an Peer #")); DPRINT(i); DPRINT(F(": "));
      DHEX(peerid.id0()); DHEX(peerid.id1()); DHEX(peerid.id2());
      DPRINT(F(" burst=")); DPRINTLN(needsBurst);

      this->device().send(msg, peerid);
    }
  }

  void sendImmediateReply(const HMID& peer, uint8_t rxCnt, bool burstFromList4) {
    if (!isValveChannel()) return;

    ValveEventMsg reply;
    uint8_t extra = nextExtra();
    reply.init(rxCnt, this->number(), m_valveRaw, m_error, extra);

    HMID me;
    this->device().getDeviceID(me);
    reply.from(me);
    reply.to(peer);

    DPRINT(F("  -> REPLY an Thermostat cnt=0x")); DHEX(rxCnt);
    DPRINT(F(" to=")); DHEX(peer.id0()); DHEX(peer.id1()); DHEX(peer.id2());
    DPRINT(F(" valveRaw=")); DPRINT(m_valveRaw);
    DPRINT(F(" (")); DPRINT(pct200(m_valveRaw)); DPRINT(F("%))"));
    DPRINT(F(" err=")); DPRINT(m_error);
    DPRINT(F(" extra=0x")); DHEX(extra);
    DPRINT(F(" burst(list4)=")); DPRINTLN(burstFromList4);

    dumpMsg(F("  TX-REPLY"), reply);
    this->device().send(reply, peer);
  }

  void enablePeriodicStatus(bool firstShort = false) {
    if (!isValveChannel()) return;
    sysclock.cancel(periodicAlarm);
    uint32_t sec = firstShort ? 5 : 300;
    periodicAlarm.set(seconds2ticks(sec));
    sysclock.add(periodicAlarm);
    DPRINT(F("  -> Periodischer Status aktiviert (first in ~"));
    DPRINT(sec);
    DPRINTLN(F("s)"));
  }

  void periodicStatusTrigger() {
    if (!isValveChannel()) return;

    DPRINTLN(F("=== PERIODIC STATUS EVENT ==="));

    ValveEventMsg msg;
    uint8_t cnt = this->device().nextcount();
    uint8_t extra = nextExtra();
    msg.init(cnt, this->number(), m_valveRaw, m_error, extra);

    HMID me;
    this->device().getDeviceID(me);
    msg.from(me);

    HMID master = this->device().getMasterID();
    if (master.valid()) {
      msg.to(master);
      this->device().send(msg, master);
    }

    for (uint8_t i = 0; i < this->peers(); i++) {
      Peer p = this->peerat(i);
      if (!p.valid()) continue;

      HMID peerid = p;
      bool needsBurst = this->getList4(p).peerNeedsBurst();

      msg.to(peerid);
      msg.burstRequired(needsBurst);

      DPRINT(F("  -> Status an Peer #")); DPRINT(i); DPRINT(F(": "));
      DHEX(peerid.id0()); DHEX(peerid.id1()); DHEX(peerid.id2());
      DPRINT(F(" burst=")); DPRINTLN(needsBurst);

      this->device().send(msg, peerid);
    }

    uint16_t nextSec = 270 + (rand() % 31); // 270..300
    periodicAlarm.set(seconds2ticks(nextSec));
    sysclock.add(periodicAlarm);

    DPRINT(F("  -> next periodic in "));
    DPRINT(nextSec);
    DPRINTLN(F("s)"));
  }

  void enableTestMode(bool firstShort = false) {
    if (!isThermostatChannel()) return;
    m_testEnabled = true;
    sysclock.cancel(testAlarm);
    uint32_t sec = firstShort ? 10 : 60;
    testAlarm.set(seconds2ticks(sec));
    sysclock.add(testAlarm);
    DPRINTLN(F("  -> Test-Modus für Channel 2 aktiviert (sendet jede Minute an Ventil)"));
  }

  uint8_t nextTestLevel() {
    // wenige Änderungen um das Ventilverhalten zu beobachten
    static const uint8_t vals[] = {51, 60, 70, 76, 70, 60, 51};
    uint8_t level = vals[m_testIndex % (sizeof(vals) / sizeof(vals[0]))];
    m_testIndex++;
    return level;
  }

  void sendTestValveCommand() {
    if (!isThermostatChannel()) return;
    if (!m_testEnabled) return;

    Peer p = this->peerat(0);
    if (!p.valid()) {
      DPRINTLN(F("=== TEST: kein Peer am Channel 2 ==="));
      return;
    }

    uint8_t level = nextTestLevel();

    DPRINTLN(F("=== TEST: Sende 0x58 (Valve-Command) an Ventil ==="));
    DPRINT(F("  Test-Wert #")); DPRINT(m_testIndex); DPRINT(F(": level=")); DPRINT(level);
    DPRINT(F(" (~")); DPRINT(pct255(level)); DPRINTLN(F("%)"));

    LevelSetMsg msg;
    uint8_t cnt = this->device().nextcount();
    msg.init(cnt, level);

    HMID me;
    this->device().getDeviceID(me);
    msg.from(me);

    HMID peerid = p;
    msg.to(peerid);

    DPRINT(F("  -> 0x58 an Peer: "));
    DHEX(peerid.id0()); DHEX(peerid.id1()); DHEX(peerid.id2());
    DPRINT(F(" level=")); DPRINT(level);
    DPRINT(F(" payload0=0x03 payload1=0x"));
    DHEX(level);
    DPRINTLN(F(""));

    dumpMsg(F("  TX-0x58"), msg);
    this->device().send(msg, peerid);
  }

  void testValveCommandTrigger() {
    if (!isThermostatChannel()) return;
    sendTestValveCommand();
    testAlarm.set(seconds2ticks(60));
    sysclock.add(testAlarm);
  }
};

// Device
template <uint8_t ChannelCount>
class ConfigDeviceType : public MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> DevType;
  ConfigDeviceType(const DeviceInfo& i, uint16_t addr) : DevType(i,addr) {}
  virtual ~ConfigDeviceType() {}

  virtual bool process(Message& msg) {
    if (msg.type() == 0x58) {
      if (msg.length() < 11) {
        DPRINTLN(F("-> 0x58 empfangen aber zu kurz (kein payload0+payload1)"));
        return true;
      }

      const HMID& sender = msg.from();
      const HMID& dest   = msg.to();
      uint8_t rxCnt      = msg.count();
      uint8_t payload0   = msg.buffer()[9];
      uint8_t payload1   = msg.buffer()[10];

      HMID me;
      this->getDeviceID(me);
      bool toMe = (dest == me);

      if (!toMe) {
        dumpMsg(F("RX-0x58(SNIFF)"), msg);
        return true;
      }

      for (uint8_t myCh = 1; myCh <= ChannelCount; myCh++) {
        uint8_t pidx = this->channel(myCh).peerfor(sender);
        if (pidx >= this->channel(myCh).peers()) continue;

        Peer p = this->channel(myCh).peerat(pidx);
        if (!p.valid()) continue;

        bool needsBurst = this->channel(myCh).getList4(p).peerNeedsBurst();

        dumpMsg(F("RX-0x58"), msg);
        DPRINTLN(F("-> HvacSetpoint (0x58) empfangen"));
        DPRINT(F("  Von Sender: "));
        DHEX(sender.id0()); DHEX(sender.id1()); DHEX(sender.id2());
        DPRINT(F(" | cnt=0x")); DHEX(rxCnt);
        DPRINT(F(" | to="));
        DHEX(dest.id0()); DHEX(dest.id1()); DHEX(dest.id2());
        DPRINT(F(" | toMe=")); DPRINT(toMe);
        DPRINT(F(" | payload0=0x")); DHEX(payload0);
        DPRINT(F(" | payload1=0x")); DHEX(payload1);

        bool hasValveSetpoint = false;
        uint8_t valveRaw200 = 0;

        if (payload0 == 0x03) {
          hasValveSetpoint = true;
          valveRaw200 = map255to200(payload1);
          DPRINT(F(" | valveSoll≈")); DPRINT(pct255(payload1)); DPRINT(F("%(255)"));
          DPRINT(F(" -> raw200=")); DPRINTLN(valveRaw200);
        }
        else {
          DPRINTLN(F(" | (kein Ventil-Soll übernommen; nur Reply)"));
        }

        DPRINT(F("  -> Peer gefunden in Channel ")); DPRINTLN(myCh);

        if (this->channel(myCh).isValveChannel()) {
          if (hasValveSetpoint) {
            this->channel(myCh).setFromThermostatValveRaw200(valveRaw200);
          }
          this->channel(myCh).sendImmediateReply(sender, rxCnt, needsBurst);
          if (hasValveSetpoint) {
            this->channel(myCh).scheduleAsyncStatusEvent();
          }
        }

        return true;
      }

      return true;
    }

    if (msg.type() == 0x70) {
      if (msg.length() >= 12) {
        dumpMsg(F("RX-THERM"), msg);
        uint8_t p0 = msg.buffer()[9];
        uint8_t p1 = msg.buffer()[10];

        DPRINT(F("  THERM meta: type=0x70 flags=0x")); DHEX(msg.flags());
        DPRINT(F(" cnt=0x")); DHEX(msg.count());
        DPRINT(F(" len=0x")); DHEX(msg.length());
        DPRINT(F(" p0=0x")); DHEX(p0);
        DPRINT(F(" p1=0x")); DHEX(p1);
        DPRINT(F(" | guess70: temp10≈"));
        DPRINT((uint16_t)p1 / 10);
        DPRINT(F("."));
        DPRINT((uint16_t)p1 % 10);
        DPRINTLN(F("C"));
      }
      return DevType::process(msg);
    }

    return DevType::process(msg);
  }
};

typedef ConfigDeviceType<NUM_CHANNELS> ConfigDevice;

Hal hal;
ConfigDevice sdev(devinfo, 0x20);
ConfigButton<ConfigDevice> cfgBtn(sdev, CONFIG_BUTTON_PIN);

void setup() {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  DPRINTLN(F("=== HB-SR-HY Ventilstellungs-Empfänger (Ventil-Emu) ==="));
  DPRINTLN(F("0x58 toMe: payload0=command, payload1=data (0..255 -> 0..200)"));
  DPRINTLN(F("Channel 1 = valve emu, Channel 2 = thermostat test sender"));
  DPRINTLN(F("ACK_EVENT (0x02/0x82): only Channel 1 replies"));
  DPRINTLN(F("Periodic status enabled only on Channel 1"));

  bool first = sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);

  if (first) {
    DPRINTLN(F("Erstes Init - Device konfiguriert"));
  }

  sdev.initDone();

  // Channel 1: periodischer Status
  sdev.channel(1).enablePeriodicStatus(true);

  // Channel 2: Testmodus
  sdev.channel(2).enableTestMode(true);
  DPRINTLN(F("Channel 2 im Test-Modus: sendet jede Minute Werte ans Ventil"));

  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60), sysclock);
}

void loop() {
  hal.runready();
  sdev.pollRadio();

  // Für Tests wach halten
  hal.activity.stayAwake(seconds2ticks(30));
}