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

// number of channels (einfach ändern für mehr Channels)
#define NUM_CHANNELS 1

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
    factor(100);
  }
};

// Link-Register (List4) - für Peer-Speicherung
DEFREGISTER(Reg4)
class SwList4 : public RegList4<Reg4> {
public:
  SwList4 (uint16_t addr) : RegList4<Reg4>(addr) {}
  
  bool peerNeedsBurst () const { return this->readBit(1, 0, false); }
  bool peerNeedsBurst (bool v) { return this->writeBit(1, 0, v); }
  
  void defaults () {
    clear();
  }
};

// Minimal-Channel mit Konfigurationsdatenpunkten und Peer-Event-Handling
class ConfigChannel : public Channel<Hal,SwList1,EmptyList,SwList4,PEERS_PER_CHANNEL,SwList0> {
private:
  uint8_t m_status;
  uint8_t m_valvePosition;  // Aktuelle Ventilstellung (0-200 oder 0-100%)
  uint8_t m_error;          // ERROR Status (0=NO_ERROR, 1=VALVE_DRIVE_BLOCKED, 2=VALVE_DRIVE_LOOSE, 3=ADJUSTING_RANGE_TO_SMALL, 4=LOWBAT)
  
public:
  typedef Channel<Hal,SwList1,EmptyList,SwList4,PEERS_PER_CHANNEL,SwList0> BaseChannel;
  ConfigChannel () : BaseChannel(), m_status(0), m_valvePosition(0), m_error(0) {}
  virtual ~ConfigChannel () {}
  
  uint8_t status () const { return m_status; }
  uint8_t flags () const { return 0; }
  
  // Gibt die aktuelle Ventilstellung zurück
  uint8_t valvePosition() const { return m_valvePosition; }
  
  // Gibt den aktuellen ERROR Status zurück
  uint8_t error() const { return m_error; }
  
  // Setzt den ERROR Status (0=NO_ERROR, 1=VALVE_DRIVE_BLOCKED, 2=VALVE_DRIVE_LOOSE, 3=ADJUSTING_RANGE_TO_SMALL, 4=LOWBAT)
  void setError(uint8_t errorCode) {
    if (errorCode <= 4) {  // Nur gültige Werte 0-4
      m_error = errorCode;
      DPRINT(F("ERROR gesetzt auf: ")); DPRINTLN(errorCode);
      changed(true);
    }
  }
  
  void configChanged() {
    DPRINT(F("ConfigChanged - ENABLE: ")); DPRINTLN(this->getList1().enable());
    DPRINT(F("ConfigChanged - FACTOR: ")); DPRINTLN(this->getList1().factor());
  }
  
  // Diese Methode wird aufgerufen, wenn ein LEVEL-Command vom Thermostat empfangen wird
  bool set(uint8_t value, uint16_t ramp) {
    DPRINTLN(F("*** set() aufgerufen ***"));
    m_valvePosition = value;
    
    // Wert ist 0-200, umrechnen in Prozent (0-100%)
    uint8_t percent = (value * 100) / 200;
    
    DPRINT(F("Ventilstellung empfangen: ")); 
    DPRINT(percent); 
    DPRINT(F("% (raw: "));
    DPRINT(value);
    DPRINTLN(F(")"));
    
    // Hier kann die empfangene Ventilstellung verarbeitet werden
    // z.B. Relais schalten, Pumpe steuern, etc.
    handleValvePosition(percent);
    
    changed(true);
    return true;
  }
  
  // Alternative Methode für ACTION_COMMAND
  void peerSetLevel(uint8_t value) {
    DPRINTLN(F("*** peerSetLevel() aufgerufen ***"));
    set(value, 0);
  }
  
  // Verarbeitet die Ventilstellung
  void handleValvePosition(uint8_t percentPosition) {
    // Einfach die Ventilstellung ausgeben
    DPRINT(F("Verarbeite Ventilstellung: ")); DPRINT(percentPosition); DPRINTLN(F("%"));
    
    // Hier können Sie später eigene Logik implementieren
    // z.B. if (percentPosition > 50) digitalWrite(RELAY_PIN, HIGH);
    
    // Optional: ERROR Status setzen basierend auf Bedingungen
    // z.B. if (percentPosition > 95) setError(1); // VALVE_DRIVE_BLOCKED
  }
  
  // Sendet Status Update (VALVE_STATE und ERROR) an CCU
  void sendStatusUpdate() {
    DPRINTLN(F("Sende Status-Update an CCU"));
    changed(true);
  }
  
  bool process (const Message& msg) {
  DPRINT(F("RX msg: type=0x"));
  DSERIAL.print(msg.type(), HEX);
  DPRINT(F(" len="));
  DPRINTLN(msg.length());

  DPRINT(F("raw: "));
  for (uint8_t i = 0; i < msg.length(); i++) {
    uint8_t b = msg.buffer()[i];
    if (b < 0x10) DPRINT('0');
    DSERIAL.print(b, HEX);
    DPRINT(' ');
  }
  DPRINT(F("\r\n"));

  // Beispiel: HVAC Setpoint (0x58) vom Thermostat
  if (msg.type() == 0x58) {
    DPRINTLN(F("-> type 0x58 (HvacSetpoint) angekommen"));
    
    // Payload Structure:
    // Byte 10: Command/Subtype (meist 0x00)
    // Byte 11: Ventilstellung (0-198, da mul="2" in XML)
    if (msg.length() >= 12) {
      uint8_t cmd = msg.buffer()[10];
      uint8_t rawValue = msg.buffer()[11];
      
      DPRINT(F("  Command: 0x")); DHEXLN(cmd);
      DPRINT(F("  Raw Value: ")); DDECLN(rawValue);
      
      // Conversion: XML hat mul="2", also CCU sendet Wert*2
      // Umrechnung: rawValue / 2 = echte Prozent (0-99%)
      uint8_t percent = rawValue / 2;
      
      DPRINT(F("  Ventilstellung: ")); DDEC(percent); DPRINTLN(F("%"));
      
      // Speichere Ventilstellung
      m_valvePosition = rawValue;
      
      // Verarbeite die Ventilstellung (eigene Logik hier)
      handleValvePosition(percent);
      
      // Sende ACK mit aktuellem Status zurück (VALVE_STATE + ERROR)
      changed(true);
      
      return true;
    }
    return true;
  }

  return false;
}

/*
  // Message Handler für eingehende Peer-Nachrichten
  bool process(const Message& msg) {
    DPRINTLN(F("*** Message empfangen ***"));
    DPRINT(F("Type: ")); DPRINTLN(msg.type());
    DPRINT(F("Length: ")); DPRINTLN(msg.length());
    
    // LEVEL_SET Command (0x11)
    if (msg.type() == 0x11 && msg.length() >= 12) {
      uint8_t value = msg.buffer()[11]; // LEVEL Value an Position 11
      DPRINT(F("LEVEL_SET empfangen, Value: ")); DPRINTLN(value);
      return set(value, 0);
    }
    
    // ACTION_SET (0x3E)
    if (msg.type() == 0x3E && msg.length() >= 12) {
      uint8_t value = msg.buffer()[11];
      DPRINT(F("ACTION_SET empfangen, Value: ")); DPRINTLN(value);
      return set(value, 0);
    }
    
    return false;
  }*/
};

// Device mit konfigurierbarer Channel-Anzahl
template <uint8_t ChannelCount>
class ConfigDeviceType : public MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> {
public:
  typedef MultiChannelDevice<Hal,ConfigChannel,ChannelCount,SwList0> DevType;
  ConfigDeviceType(const DeviceInfo& i, uint16_t addr) : DevType(i,addr) {}
  virtual ~ConfigDeviceType() {}

  // Device-Level Message Handler
  virtual bool process(Message& msg) {
    // DPRINTLN(F("*** Device process() aufgerufen ***"));
    // DPRINT(F("Type: 0x")); DHEXLN(msg.type());
    // DPRINT(F("Len : ")); DPRINTLN(msg.length());
    
    // // Logge alle Bytes der Nachricht
    // DPRINT(F("Raw: "));
    // for (uint8_t i = 0; i < msg.length(); i++) {
    //   uint8_t b = msg.buffer()[i];
    //   if (b < 0x10) DPRINT('0');
    //   DSERIAL.print(b, HEX);
    //   DPRINT(' ');
    // }
    // DPRINTLN(F(""));
    
    // HvacSetpoint (0x58) vom Thermostat
    if (msg.type() == 0x58 && msg.length() >= 11) {
      // DPRINTLN(F("-> HvacSetpoint (0x58) empfangen"));
      
      // uint8_t cmd = msg.buffer()[9];
      uint8_t rawValue = msg.buffer()[10];
      
      // Conversion: mul="2" in XML -> rawValue / 2 = Prozent
      // uint8_t percent = rawValue / 2;
      
      // Hole Sender-Adresse aus der Nachricht
      const HMID& sender = msg.from();
      
      // DPRINT(F("  Von Sender: "));
      // DHEX(sender.id0()); DHEX(sender.id1()); DHEX(sender.id2());
      // DPRINTLN(F(""));
      
      // Finde den Channel, der mit diesem Peer verlinkt ist
      for (uint8_t ch = 1; ch <= ChannelCount; ch++) {
        uint8_t peerIdx = this->channel(ch).peerfor(sender);
        if (peerIdx < this->channel(ch).peers()) {
          // DPRINT(F("  -> Peer gefunden in Channel ")); DDEC(ch);
          // DPRINT(F(" (Index ")); DDEC(peerIdx); DPRINTLN(F(")"));
          this->channel(ch).set(rawValue, 0);
          return true;
        }
      }
    }
    
    // Fallback: Standard-Verarbeitung
    return DevType::process(msg);
  }

};

// Typ-Alias für die konfigurierte Device-Klasse
typedef ConfigDeviceType<NUM_CHANNELS> ConfigDevice;

Hal hal;
ConfigDevice sdev(devinfo, 0x20);
ConfigButton<ConfigDevice> cfgBtn(sdev, CONFIG_BUTTON_PIN);

void setup() {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  DPRINTLN(F("=== HB-SR-HY Ventilstellungs-Empfänger ==="));
  bool first = sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
  if(first) {
    DPRINTLN(F("Erstes Init - erstelle interne Peers"));
    HMID devid;
    sdev.getDeviceID(devid);
    // Erstelle internen Peer für jeden Channel
    for (uint8_t ch = 1; ch <= NUM_CHANNELS; ch++) {
      Peer ipeer(devid, ch);
      sdev.channel(ch).peer(ipeer);
      DPRINT(F("  Peer für Channel ")); DDEC(ch); DPRINTLN(F(" erstellt"));
    }
  }
  sdev.initDone();
  DPRINTLN(F("Device bereit - warte auf Links/Nachrichten"));
  hal.activity.stayAwake(seconds2ticks(15));
  hal.battery.init(seconds2ticks(60UL*60),sysclock);
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  // Kein Sleep-Modus für Test - immer empfangsbereit
  // Später für Batteriebetrieb wieder aktivieren:
  if( worked == false && poll == false ) {
    // hal.activity.savePower<Sleep<> >(hal);
  }
}

// Beispiel: Zugriff auf Konfigurationsdatenpunkte im Channel
void printConfig() {
  DPRINT(F("enable: ")); DPRINTLN(sdev.channel(1).getList1().enable());
  DPRINT(F("factor: ")); DPRINTLN(sdev.channel(1).getList1().factor());
  DPRINT(F("Aktuelle Ventilstellung: ")); DPRINT(sdev.channel(1).valvePosition());
  DPRINTLN(F("%"));
  DPRINT(F("ERROR Status: ")); DPRINTLN(sdev.channel(1).error());
}

// Beispiel: Peer-Konfiguration anzeigen
void printPeerConfig() {
  DPRINTLN(F("Channel 1 Konfiguration:"));
  DPRINT(F("Enable: ")); DPRINTLN(sdev.channel(1).getList1().enable());
  DPRINT(F("Factor: ")); DPRINTLN(sdev.channel(1).getList1().factor());
}
