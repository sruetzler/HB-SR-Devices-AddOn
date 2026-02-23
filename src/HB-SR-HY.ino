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
    {0xfe,0x01,0x01},       // Device ID
    "SRUE000001",          // Device Serial
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

// Register für Channel 1 (List1)
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
    factor(0);
  }
};

// Minimal-Channel mit Konfigurationsdatenpunkten und Peer-Event-Handling
class ConfigChannel : public Channel<Hal,SwList1,EmptyList,DefList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t m_status;
  
public:
  typedef Channel<Hal,SwList1,EmptyList,DefList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ConfigChannel () : BaseChannel(), m_status(0) {}
  virtual ~ConfigChannel () {}
  
  uint8_t status () const { return m_status; }
  uint8_t flags () const { return 0; }
  
  void configChanged() {
    DPRINT(F("ConfigChanged - ENABLE: ")); DPRINTLN(this->getList1().enable());
    DPRINT(F("ConfigChanged - FACTOR: ")); DPRINTLN(this->getList1().factor());
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
  }
  sdev.initDone();
  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60),sysclock);
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
  DPRINT(F("enable: ")); DPRINTLN(sdev.channel(1).getList1().enable());
  DPRINT(F("factor: ")); DPRINTLN(sdev.channel(1).getList1().factor());
}

// Beispiel: Peer-Ausgang (Status-Info senden)
void sendPeerStatus() {
  // Beispiel für Status-Broadcast - implementiere basierend auf deinen Anforderungen
  DPRINTLN(F("Send peer status"));
}
