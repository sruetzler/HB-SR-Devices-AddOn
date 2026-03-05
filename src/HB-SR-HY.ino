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
#define NUM_CHANNELS 1

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

// List4 (Peer-Burst Flag)
DEFREGISTER(Reg4)
class SwList4 : public RegList4<Reg4> {
public:
  SwList4 (uint16_t addr) : RegList4<Reg4>(addr) {}
  bool peerNeedsBurst () const { return this->readBit(1, 0, false); }
  bool peerNeedsBurst (bool v) { return this->writeBit(1, 0, v); }
  void defaults () { clear(); }
};

// ------------------------------------------------------------
// ACK_EVENT (0x02) wie Ventile im Mitschnitt
//
// len=0x0E  => 5 Payload-Bytes
// payload: [ channel ][ subcom ][ valveRaw ][ errBits ][ extra ]
//
// AskSin++: initWithCount(len,type,flags,channel) schreibt channel als 1. Payload-Byte,
// danach kommen subcom + pload[0..2] (3 Bytes) => insgesamt 5 Payload-Bytes.
//
// Flags: 0x82 (wie echte Ventile)
// ------------------------------------------------------------
class ValveEventMsg : public Message {
public:
  void init(uint8_t msgcnt, uint8_t channel, uint8_t valveRaw, uint8_t error, uint8_t extra) {
    if (valveRaw > 200) valveRaw = 200;

    initWithCount(0x0E, 0x02, 0x82, channel);   // bleibt 0x82
    cnt    = msgcnt;
    subcom = 0x01;

    pload[0] = valveRaw;                 // valveRaw (0..200)
    pload[1] = (error & 0x07) << 1;      // errBits
    pload[2] = extra;                    // extra status byte (echt oft 0x20/0x21)
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

// Helpers
static uint8_t map255to200(uint8_t v255) {
  // Rundung: (v*200 + 127) / 255
  uint16_t tmp = (uint16_t)v255 * 200 + 127;
  return (uint8_t)(tmp / 255);
}
static uint8_t pct255(uint8_t v255) {
  return (uint8_t)((uint16_t)v255 * 100 / 255);
}
static uint8_t pct200(uint8_t v200) {
  return (uint8_t)((uint16_t)v200 * 100 / 200);
}

// Channel
class ConfigChannel : public Channel<Hal,SwList1,EmptyList,SwList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t m_status;
  uint8_t m_valveRaw;   // 0..200 (Istwert, den wir im ACK_EVENT zurückmelden)
  uint8_t m_error;      // 0..4
  uint8_t m_extra;      // wir senden 0x20/0x21 wie echte Ventile (toggle)

  class AsyncStatusAlarm : public Alarm {
    ConfigChannel& ch;
  public:
    AsyncStatusAlarm(ConfigChannel& c) : Alarm(0), ch(c) {}
    virtual ~AsyncStatusAlarm() {}
    virtual void trigger(AlarmClock&) { ch.sendAsyncStatusEvent(); }
  } asyncAlarm;

public:
  typedef Channel<Hal,SwList1,EmptyList,SwList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;

  ConfigChannel () :
    BaseChannel(),
    m_status(0),
    m_valveRaw(0),
    m_error(0),
    m_extra(0x21),  // Start wie im Mitschnitt häufig
    asyncAlarm(*this) {}
  virtual ~ConfigChannel () {}

  // Diese zwei braucht AskSin++ intern (AckStatus/InfoActuatorStatus)
  uint8_t status () const { return m_status; }
  uint8_t flags  () const { return 0; }

  void configChanged() {
    DPRINT(F("ConfigChanged - ENABLE: ")); DPRINTLN(this->getList1().enable());
    DPRINT(F("ConfigChanged - FACTOR: ")); DPRINTLN(this->getList1().factor());
  }

  void setError(uint8_t e) {
    if (e <= 4) {
      m_error = e;
      scheduleAsyncStatusEvent();
      changed(true);
    }
  }

  // Das ist der entscheidende Punkt für Ventil-Emu:
  // Wenn Thermostat wirklich Ventil-Soll sendet, übernehmen wir es als unseren "Istwert",
  // damit das Thermostat danach "sieht", dass das Ventil so steht.
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
    // Toggle zwischen 0x20 und 0x21 (wie echt beobachtet)
    // nur Bit0 toggeln, Basis 0x20 beibehalten
    m_extra ^= 0x01;
    m_extra = (m_extra & 0x01) ? 0x21 : 0x20;
    return m_extra;
  }

  void scheduleAsyncStatusEvent() {
    sysclock.cancel(asyncAlarm);
    asyncAlarm.set(millis2ticks(3000));
    sysclock.add(asyncAlarm);
    DPRINTLN(F("  -> Async Status-Event geplant in 3 Sekunden"));
  }

  // unsolicited Status (new cnt) an Peers (+ optional Master)
  void sendAsyncStatusEvent() {
    DPRINTLN(F("=== ASYNC STATUS EVENT ==="));

    ValveEventMsg msg;
    uint8_t cnt = this->device().nextcount();
    uint8_t extra = nextExtra();
    msg.init(cnt, this->number(), m_valveRaw, m_error, extra);

    HMID me;
    this->device().getDeviceID(me);
    msg.from(me);

    // (1) Master (CCU)
    HMID master = this->device().getMasterID();
    if (master.valid()) {
      msg.to(master);
      this->device().send(msg, master);
    }

    // (2) alle Peers in list4
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

  // SOFORT-Reply auf 0x58 (gleicher cnt!) – OHNE burstRequired(), damit flags=0x82 bleibt
  void sendImmediateReply(const HMID& peer, uint8_t rxCnt, bool burstFromList4) {
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
    this->device().send(reply, peer);  // flags bleibt 0x82
  }
};

// Device
template <uint8_t ChannelCount>
class ConfigDeviceType : public MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> DevType;
  ConfigDeviceType(const DeviceInfo& i, uint16_t addr) : DevType(i,addr) {}
  virtual ~ConfigDeviceType() {}

  // Peer check: ist Sender als Peer in irgendeinem Channel gelinkt?
  bool isPeer(const HMID& sender) {
    for (uint8_t myCh = 1; myCh <= ChannelCount; myCh++) {
      uint8_t pidx = this->channel(myCh).peerfor(sender);
      if (pidx < this->channel(myCh).peers()) {
        Peer p = this->channel(myCh).peerat(pidx);
        if (p.valid()) return true;
      }
    }
    return false;
  }

  virtual bool process(Message& msg) {
    // ---- nur für Debug: alle 0x58 vom Thermostat/Peer loggen ----
    if (msg.type() == 0x58) {

      // Für 0x58 mit 2 Payload-Bytes braucht man len=0x0B => msg.length() == 11.
      if (msg.length() < 11) {
        DPRINTLN(F("-> 0x58 empfangen aber zu kurz (kein payload0+payload1)"));
        return true;
      }

      const HMID& sender = msg.from();
      const HMID& dest   = msg.to();
      uint8_t rxCnt      = msg.count();

      // Layout bei len=0x0B (ohne Längenbyte):
      // [0]=cnt [1]=flags [2]=type [3..5]=from [6..8]=to [9]=payload0 [10]=payload1
      uint8_t payload0 = msg.buffer()[9];
      uint8_t payload1 = msg.buffer()[10];

      // Unser HMID (Device-ID)
      HMID me;
      this->getDeviceID(me);
      bool toMe = (msg.to() == me);

      if (!toMe) {
        // sniff only: loggen, aber NICHT antworten und NICHT state übernehmen
        dumpMsg(F("RX-0x58(SNIFF)"), msg);
        return true;
      }

      // Heuristik:
      // - Wenn direkt an uns adressiert (Ventil-Emu): payload1 ist Ventil-Soll 0..255
      // - Sonst: nichts übernehmen (nur loggen + ggf. ignorieren)
      bool hasValveSetpoint = false;
      uint8_t valveRaw200 = 0;

      hasValveSetpoint = true;
      valveRaw200 = map255to200(payload1);

      // Peer-Suche: nur über Sender-ID (Peer-Beziehung)
      for (uint8_t myCh = 1; myCh <= ChannelCount; myCh++) {
        uint8_t pidx = this->channel(myCh).peerfor(sender);
        if (pidx >= this->channel(myCh).peers()) continue;

        Peer p = this->channel(myCh).peerat(pidx);
        if (!p.valid()) continue;

        bool needsBurst = this->channel(myCh).getList4(p).peerNeedsBurst();

        // Logging
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

        if (hasValveSetpoint) {
          // Prozentanzeige aus 0..255
          uint8_t pct255 = (uint16_t)payload1 * 100 / 255;
          DPRINT(F(" | valveSoll≈")); DPRINT(pct255); DPRINT(F("%(255)"));
          DPRINT(F(" -> raw200=")); DPRINTLN(valveRaw200);
        } else {
          DPRINTLN(F(" | (kein Ventil-Soll übernommen; nur Reply)"));
        }

        DPRINT(F("  -> Peer gefunden in Channel ")); DPRINTLN(myCh);

        // (1) state übernehmen NUR wenn wir wirklich einen Ventil-Sollwert haben
        if (hasValveSetpoint) {
          this->channel(myCh).setFromThermostatValveRaw200(valveRaw200);        
        }

        // (2) SOFORT reply (gleicher Counter!) – ohne Burst-Flag, damit flags=0x82 bleibt
        this->channel(myCh).sendImmediateReply(sender, rxCnt, needsBurst);

        return true;
      }

      // Kein Peer -> trotzdem nicht weiterverarbeiten
      // (sonst könnte Basisklasse evtl. irgendwas damit machen)
      // Optional Debug:
      // DPRINTLN(F("  -> 0x58: Sender ist kein Peer"));
      return true;
    }

    // alles andere normal
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
  DPRINTLN(F("Extra logging: all msgs from peers (thermostat)"));
  DPRINTLN(F("0x58: reply ACK_EVENT (0x02/0x82) with our current valveRaw"));
  DPRINTLN(F("0x58 heuristic: payload0==0x03 => payload1(0..255) as valve setpoint"));
  DPRINTLN(F("0x58 heuristic: payload0==0x00 => likely temp*10 (logged only, not used)"));
  DPRINTLN(F("ACK extra byte: toggling 0x20/0x21 (closer to real valves)"));

  bool first = sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);

  if (first) {
    DPRINTLN(F("Erstes Init - Device konfiguriert"));
  }

  sdev.initDone();

  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60), sysclock);
}

void loop() {
  hal.runready();
  sdev.pollRadio();

  // Für Tests wach halten
  hal.activity.stayAwake(seconds2ticks(30));
}