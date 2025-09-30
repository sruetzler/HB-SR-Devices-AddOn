//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2018-09-29 jp112sdl Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

// define this to read the device id, serial and device type from bootloader section
// #define USE_OTA_BOOTLOADER
#define USE_AES
#define HM_DEF_KEY 0x5f,0x8f,0xe3,0x01,0xaf,0x69,0x38,0xcf,0x1f,0xc1,0xca,0x6c,0x3d,0xf0,0x4b,0x01

#define HM_DEF_KEY_INDEX 2

#define USE_WOR

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>

#include <Switch.h>


// we use a Pro Mini
// Arduino pin for the LED
// D4 == PIN 4 on Pro Mini
#define LED_PIN 4
// Arduino pin for the config button
// B0 == PIN 8 on Pro Mini
#define CONFIG_BUTTON_PIN 8

// number of available peers per channel
#define PEERS_PER_CHANNEL 4

// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0x01,0x99,0x01},       // Device ID
    "SRUE00001",          // Device Serial
    {0xfe,0x01},            // Device Model
    0x11,                   // Firmware Version
    0xfe,                   // Device Type: Thermostat (oder eigenen Wert)
    {0x01,0x00}             // Info Bytes
};

/**
 * Configure the used hardware
 */
typedef AvrSPI<10,11,12,13> RadioSPI;
typedef AskSin<StatusLed<LED_PIN>,BatterySensor,Radio<RadioSPI,2> > Hal;

DEFREGISTER(Reg0,DREG_INTKEY,DREG_LEDMODE,MASTERID_REGS,DREG_LOWBATLIMIT)
class SwList0 : public RegList0<Reg0> {
public:
  SwList0(uint16_t addr) : RegList0<Reg0>(addr) {}
  void defaults () {
    clear();
    lowBatLimit(22);
  }
};


// Zusätzliche Register-IDs für Konfigurationsdaten

#define REG_ENABLE     0x20
#define REG_LEARN      0x21
#define REG_NEWFACTOR  0x22
#define REG_ACTFACTOR  0x23



// Minimal-Channel mit nur Konfigurationsdatenpunkten und Peer-Event-Handling
class ConfigChannel : public SwitchChannel<Hal,PEERS_PER_CHANNEL,SwList0> {
public:
  typedef SwitchChannel<Hal,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ConfigChannel () {}
  virtual ~ConfigChannel () {}

  // Zugriff auf Konfigurationsdatenpunkte
  bool enable() const { return getList1().getByte(REG_ENABLE) != 0; }
  void enable(bool v) { getList1().setByte(REG_ENABLE, v ? 1 : 0); }
  bool learn() const { return getList1().getByte(REG_LEARN) != 0; }
  void learn(bool v) { getList1().setByte(REG_LEARN, v ? 1 : 0); }
  uint8_t newFactor() const { return getList1().getByte(REG_NEWFACTOR); }
  void newFactor(uint8_t v) { getList1().setByte(REG_NEWFACTOR, v); }
  uint8_t actFactor() const { return getList1().getByte(REG_ACTFACTOR); }
  void actFactor(uint8_t v) { getList1().setByte(REG_ACTFACTOR, v); }

  // Peer-Event-Handling
  virtual bool process(const Message& msg) {
    DPRINTLN(F("Peer-Event empfangen!"));
    return true;
  }
};

// Device mit nur einem Channel
class ConfigDevice : public MultiChannelDevice<Hal,ConfigChannel,1,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ConfigChannel,1,SwList0> DevType;
  ConfigDevice(const DeviceInfo& i, uint16_t addr) : DevType(i,addr) {}
  virtual ~ConfigDevice() {}
};

Hal hal;
ConfigDevice sdev(devinfo, 0x20);
ConfigButton<ConfigDevice> cfgBtn(sdev, CONFIG_BUTTON_PIN);

void setup() {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  bool first = sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
  if(first) {
    HMID devid;
    sdev.getDeviceID(devid);
    Peer ipeer(devid,1);
    sdev.channel(1).peer(ipeer);
    // Konfigurationsdatenpunkte initialisieren
    sdev.channel(1).getList1().setByte(REG_ENABLE, 1);
    sdev.channel(1).getList1().setByte(REG_LEARN, 0);
    sdev.channel(1).getList1().setByte(REG_NEWFACTOR, 0);
    sdev.channel(1).getList1().setByte(REG_ACTFACTOR, 0);
  }
  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60),sysclock);
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if( worked == false && poll == false ) {
    hal.activity.savePower<Sleep<> >(hal);
  }
}

// Beispiel: Zugriff auf Konfigurationsdatenpunkte im Channel
void printConfig() {
  DPRINT(F("enable: ")); DPRINTLN(sdev.channel(1).getList1().getByte(REG_ENABLE));
  DPRINT(F("learn: ")); DPRINTLN(sdev.channel(1).getList1().getByte(REG_LEARN));
  DPRINT(F("newFactor: ")); DPRINTLN(sdev.channel(1).getList1().getByte(REG_NEWFACTOR));
  DPRINT(F("actFactor: ")); DPRINTLN(sdev.channel(1).getList1().getByte(REG_ACTFACTOR));
}

// Beispiel: Peer-Ausgang
void sendPeerStatus() {
  sdev.sendInfoActuatorStatus(HMID::broadcast, sdev.nextcount(), sdev.channel(1), true);
}
