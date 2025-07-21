#define EI_NOTEXTERNAL
#define SIMPLE_CC1101_INIT  // Einfachere CC1101 Initialisierung
#define USE_WOR            // Wake-On-Radio aktivieren
#define DEVICE_LED_PIN 4
#define LED_INVERT 0
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>

#include <Register.h>
#include <MultiChannelDevice.h>
#include <Switch.h>

// Device pins
#define LED_PIN 4
#define CONFIG_BUTTON_PIN 8
#define CC1101_CS_PIN 10
#define CC1101_GDO0_PIN 2
#define CC1101_SCK_PIN 13
#define CC1101_MOSI_PIN 11
#define CC1101_MISO_PIN 12
#define RELAY_PIN 5  // Pin für das Relais/Aktor

#define PEERS_PER_CHANNEL 6

using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0x12,0x34,0x56},       // Device ID (should be unique)
    "JPM0000001",           // Device Serial (should be unique, 10 chars)
    {0xF1,0x01},            // Device Model (HM-LC-SW1-SM)
    0x10,                    // Firmware Version
    as::DeviceType::Switch, // Device Type
    {0x01,0x00}             // Info Bytes
};

// Configure the used hardware
typedef AvrSPI<CC1101_CS_PIN, CC1101_MOSI_PIN, CC1101_MISO_PIN, CC1101_SCK_PIN> SPIType;
typedef Radio<SPIType, CC1101_GDO0_PIN> RadioType;
typedef StatusLed<LED_PIN> LedType;
typedef AskSin<LedType, NoBattery, RadioType> HalType;

// Channel device implementation with a switch
template <class HALTYPE>
class MySwChannel : public Channel<HALTYPE,SwitchList1,SwitchList3,EmptyList,PEERS_PER_CHANNEL,List0> {
  
private:
  bool relay_state;
  
public:
  typedef Channel<HALTYPE,SwitchList1,SwitchList3,EmptyList,PEERS_PER_CHANNEL,List0> BaseChannel;
  
  MySwChannel () : BaseChannel(), relay_state(false) {}
  virtual ~MySwChannel() {}

  void init (uint8_t pin) {
    pinMode(pin,OUTPUT);
    digitalWrite(pin,LOW);
  }

  bool status () const {
    return relay_state;
  }
  
  uint8_t flags () const {
    return 0;
  }

  virtual void switchState(uint8_t oldstate,uint8_t newstate) {
    relay_state = (newstate==AS_CM_JT_ON);
    digitalWrite(RELAY_PIN, relay_state ? HIGH : LOW);
    Channel<HALTYPE,SwitchList1,SwitchList3,EmptyList,PEERS_PER_CHANNEL,List0>::changed(true);
  }
};

// The device configuration
typedef MultiChannelDevice<HalType, MySwChannel<HalType>, 1> SwitchDevice;

HalType hal;
SwitchDevice sdev(devinfo,0x20);
ConfigButton<SwitchDevice> cfgBtn(sdev);

void setup () {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  
  // CC1101 benötigt etwas Zeit zum Starten
  _delay_ms(2000);
  
  // Initialize the hardware
  hal.init(NULL);
  
  // Initialize the config button
  cfgBtn.init(CONFIG_BUTTON_PIN);
  
  // Initialize the output pin
  sdev.channel(1).init(RELAY_PIN);
  
  // Signal that init is done
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if( worked == false && poll == false ) {
    hal.activity.savePower<Sleep<>>(hal);
  }
}
