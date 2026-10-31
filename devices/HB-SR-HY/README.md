# HB-SR-HY

HY sitzt als netzversorgter Protokoll-Proxy zwischen einem HM-CC-TC und einem
HM-CC-VD. Kanal 1 verhält sich gegenüber dem TC wie ein VD, Kanal 2 gegenüber
dem VD wie ein TC. Beide Funkverbindungen haben unabhängige Zeitpläne.

## Entwicklung

Diesen Ordner in VS Code öffnen und die PlatformIO-Erweiterung verwenden.
Die einzige aktive Projektkonfiguration ist `platformio.ini` im Geräteordner.

```sh
pio run
pio run --target upload
pio device monitor
```

Board: Pro Mini, ATmega328P, 3,3 V / 8 MHz. Serieller Monitor: 57600 Baud.
Die Firmware wurde beim Zusammenführen der Repositories nicht verändert.

## Inbetriebnahme des Boards

Das bestückte Board verwendet einen ATmega328P-PU mit externem 8-MHz-Quarz
und 3,3-V-Versorgung. PlatformIO verwendet dafür das Boardprofil
`pro8MHzatmega328`. Die folgenden Schritte installieren einmalig Optiboot
über ISP; spätere Firmware-Uploads erfolgen über FTDI.

### Versorgung und Anschlüsse

Vor Änderungen an der Verdrahtung die Versorgung ausschalten. Am ATmega
VCC (DIP-Pin 7) und AVCC (20) mit 3,3 V sowie beide GND-Pins (8 und 22)
mit Masse verbinden. AREF (21) ist kein Massepin. Versorgung direkt am
Chip messen; 100-nF-Abblockkondensatoren gehören nahe an die Versorgungspins.
Der 8-MHz-Quarz liegt zwischen XTAL1 (9) und XTAL2 (10), mit den vorgesehenen
Lastkondensatoren nach GND. Eine zuvor verwendete Fremdtaktquelle entfernen.

Den Pololu USB AVR Programmer v2.1 für ISP wie folgt anschließen:

| ISP-Signal | ATmega328P-PU, DIP-Pin |
| --- | --- |
| RESET | 1 |
| MOSI | 17 |
| MISO | 18 |
| SCK | 19 |
| VCC, Spannungsreferenz | 7, 3,3 V |
| GND | Gemeinsame Masse |

Für einen zuverlässigen ISP-Zugriff am besten **Pin 3 des verwendeten
CC1101-Moduls vorübergehend abklemmen**. Die Verbindung bei ausgeschalteter
Versorgung trennen und nach der ISP-Programmierung vor dem normalen
Funkbetrieb wiederherstellen. Gemeint ist die Modul-Pinnummer, nicht die
Pinnummer des CC1101-ICs.

Die Programmierung setzt eine stabile Zielversorgung voraus; nicht allein
vom eingesteckten USB-Kabel auf eine versorgte Platine schließen. Bei
mehreren Adaptern nur eine Versorgung verwenden und keine 5-V-Signale
einspeisen. Im avrdude-Verbose-Modus zeigt `Vtarget` die vom Pololu gemessene
Spannung am ISP-Anschluss; erwartet werden etwa 3,3 V.

Für spätere FTDI-Uploads einen Adapter mit 3,3-V-Logik verwenden:

| FTDI-Signal | Anschluss am Board |
| --- | --- |
| TX | ATmega RXD, DIP-Pin 2 |
| RX | ATmega TXD, DIP-Pin 3 |
| GND | Gemeinsame Masse |
| DTR | Über 100 nF an RESET, mit etwa 10 kOhm Pull-up nach VCC |

Ohne Auto-Reset muss RESET passend zum Beginn des Uploads manuell ausgelöst
werden. Nicht mehrere TX-Ausgänge parallel verbinden. Die UART-Bridge des
Pololu kann alternativ zum Mitlesen dienen: ATmega TXD an Pololu RX und
gemeinsame Masse. UART wird nicht über das sechspolige ISP-Kabel übertragen.

### Bootloader bauen

PlatformIO und dessen AVR-Toolchain müssen installiert sein. Unter Linux
liegt das PlatformIO-Programm der VS-Code-Erweiterung normalerweise unter
`~/.platformio/penv/bin/pio`, auch wenn `pio` nicht im Terminal-PATH liegt.
Die folgenden Shell-Befehle in derselben Bash-Sitzung ausführen.

```sh
BOOT_SRC=$(mktemp -d /tmp/hb-sr-hy-optiboot.XXXXXX)
git clone --depth 1 https://github.com/Optiboot/optiboot.git "$BOOT_SRC"
BOOT_DIR="$BOOT_SRC/optiboot/bootloaders/optiboot"
make -C "$BOOT_DIR" clean
make -C "$BOOT_DIR" atmega328 \
  AVR_FREQ=8000000L BAUD_RATE=38400 LED_START_FLASHES=0 \
  GCCROOT="$HOME/.platformio/packages/toolchain-atmelavr/bin/"
BOOT_HEX="$BOOT_DIR/optiboot_atmega328.hex"
test -s "$BOOT_HEX"
```

Das Ziel baut für `F_CPU=8000000L` und UART0 mit 38400 Baud, ohne
LED-Startblinken. Der bei der Inbetriebnahme gebaute Bootloader belegte
452 Byte innerhalb des 512-Byte-Bootbereichs `0x7E00` bis `0x7FFF`.
Der Build meldet bei 8 MHz eine tatsächliche Baudrate von 38461, also
etwa +0,16 % Abweichung von 38400. Eine andere Upload-Baudrate erfordert
einen entsprechend gebauten und installierten Bootloader; nur die
PlatformIO-Einstellung zu ändern reicht nicht aus.

### Bootloader über ISP installieren

Die Portnamen mit `ls /dev/serial/by-id/` prüfen. Der folgende Port gehört
zum hier verwendeten Pololu; bei einem anderen Gerät die Seriennummer
anpassen. Ein laufender Upload muss vorher beendet sein.

```sh
AVRDUDE="$HOME/.platformio/packages/tool-avrdude/avrdude"
AVRCONF="$HOME/.platformio/packages/tool-avrdude/avrdude.conf"
ISP_PORT=/dev/serial/by-id/usb-Pololu_Corporation_Pololu_USB_AVR_Programmer_v2.1_00513563-if01
"$AVRDUDE" -C "$AVRCONF" -c stk500v2 -p m328p -B20 -v -n -u \
  -P "$ISP_PORT" \
  -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h -U lock:r:-:h
```

Nur bei stabiler Versorgung und korrekter Signatur `0x1e950f` fortfahren.
`-B20` wählt eine ISP-Taktperiode von ungefähr 20 Mikrosekunden, nicht die
UART-Baudrate. Bei fehlerhafter Signatur nicht mit `-F` erzwingen.

**Achtung: Der nächste Befehl löscht die vorhandene Anwendung. Ohne gesetzte
EESAVE-Fuse wird auch das EEPROM mit Geräte-Konfiguration und Peers gelöscht.
Vorher benötigte Inhalte sichern.** Die Ziel-High-Fuse `0xDE` schützt das
EEPROM nicht bei künftigen Chip-Erases.

| Einstellung | Wert |
| --- | --- |
| Unlock-Bits vor dem Schreiben | `0x3F` |
| Low-Fuse, Full-Swing-Quarz, CKDIV8 aus | `0xF7` |
| High-Fuse, 512-Byte-Bootbereich, Boot-Reset aktiv | `0xDE` |
| Extended-Fuse, Brown-out 2,7 V | `0xFD` |
| Lock-Bits nach dem Schreiben | `0x0F` |

```sh
"$AVRDUDE" -C "$AVRCONF" -c stk500v2 -p m328p -B20 -u \
  -P "$ISP_PORT" -e \
  -U lock:w:0x3F:m \
  -U lfuse:w:0xF7:m -U hfuse:w:0xDE:m -U efuse:w:0xFD:m \
  -U "flash:w:$BOOT_HEX:i" -U lock:w:0x0F:m
```

avrdude verifiziert jeden Schreibschritt automatisch. Bei einem Fehler
abbrechen und nicht mit dem nächsten Schritt fortfahren. Danach noch einmal
unabhängig prüfen:

```sh
"$AVRDUDE" -C "$AVRCONF" -c stk500v2 -p m328p -B20 -n -u \
  -P "$ISP_PORT" -U "flash:v:$BOOT_HEX:i" \
  -U lfuse:v:0xF7:m -U hfuse:v:0xDE:m -U efuse:v:0xFD:m \
  -U lock:v:0x0F:m \
  -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h -U lock:r:-:h
```

Erwartet werden erfolgreiche Verifikationen und die Fuses `F7 / DE / FD`.
Die unbenutzten oberen Lock-Bits werden als Eins gelesen: `0x0F` erscheint
daher als `0xCF`, `0x3F` als `0xFF`. avrdude 6.3 kann dazu eine Warnung
ausgeben; entscheidend sind die implementierten unteren sechs Bits.

### Firmware über FTDI laden und prüfen

In [platformio.ini](platformio.ini) ist `pro8MHzatmega328` das Standard-
Environment: Protokoll `arduino`, Upload mit 38400 Baud und maximal
32256 Byte Anwendungsspeicher. Upload- und Monitor-Port zeigen auf den
FTDI-Adapter mit Seriennummer `A50285BI`; bei einem anderen Adapter beide
Einstellungen anhand von `/dev/serial/by-id/` anpassen.

Im Geräteordner ausführen:

```sh
~/.platformio/penv/bin/pio run -e pro8MHzatmega328 -t upload
~/.platformio/penv/bin/pio device monitor -e pro8MHzatmega328
```

Alternativ in VS Code unter PlatformIO → Project Tasks →
`pro8MHzatmega328` zuerst **Upload**, danach **Monitor** wählen. Ein
erfolgreicher Upload endet mit `bytes of flash verified` und `[SUCCESS]`.
Der Monitor verwendet **57600 Baud, 8-N-1**, unabhängig von den 38400 Baud
des Bootloaders. Für die Startausgabe bei geöffnetem Monitor das Board
zurücksetzen. Vor dem nächsten Upload den Monitor mit Strg+C schließen.

**Nicht `isp → Upload` oder `isp → Set Fuses` für diesen Ablauf verwenden.**
Das bestehende ISP-Environment ist für Betrieb ohne Bootloader eingerichtet:
Sein Upload löscht Optiboot, und seine Fuses `EC / D7 / FD` sind nicht die
hier dokumentierten Bootloader-Fuses. Normale FTDI-Uploads verwenden `-D`
und schreiben weder Fuses noch Lock-Bits; die EEPROM-Konfiguration bleibt
dabei erhalten.

Bei unlesbarer Konsole zuerst 57600 Baud und die UART-Verdrahtung prüfen.
Bleibt die Ausgabe falsch, den tatsächlichen CPU-Takt und CKDIV8 prüfen,
statt nur die Monitor-Baudrate zu ändern. Bei 57600 Baud beträgt eine
Bitdauer etwa 17,4 Mikrosekunden. Erst nach lesbarer Startausgabe mit dem
Anlernen und der CCU-Konfiguration fortfahren.

## Konfiguration in der CCU

Pro Kanal ist maximal ein direkter Peer vorgesehen: auf Kanal 1 ein HM-CC-TC,
auf Kanal 2 ein HM-CC-VD. Die Gerätebeschreibung begrenzt beide Kanäle mit
`max_link_peers="1"`, passend zu `PEERS_PER_CHANNEL 1` in der Firmware.

Auf Kanal 1:

- `ENABLE`: hydraulische Anpassung aktiv, Standard `true`. Bei `false` gilt ein
  effektiver Faktor von 100 %.
- `FACTOR`: Anteil des normalen TC-Stellwerts, der zum VD gelangt, 0–100 %,
  Standard 100 %. Faktor 80 bedeutet 20 % Reduktion.

Die letzte tatsächliche VD-Position wird für TC und CCU invers skaliert und auf
100 % begrenzt. Bei Faktor 0 meldet HY mit vorhandenem VD-Status im Normalbetrieb
100 %. Sonderbefehle werden nicht mit dem Faktor skaliert.
Änderungen wirken zum nächsten regulären VD-Sendezyklus.

Auf Geräteebene und Kanal 2 gibt es keine MASTER-Konfigurationsparameter mehr.
HY ist mit `rx_modes="ALWAYS"` als dauerhaft erreichbar beschrieben.

## Gerätebeschreibung und Paketbau

Die einzige bearbeitbare HY-Gerätebeschreibung ist `ccu/hb-sr-hy.xml`.
Modell-ID: `0xFE01`. Gerätetyp: `HB-SR-HY`.

```sh
sh ../../scripts/build-addon.sh
```

Dieser Befehl baut das gemeinsame Addon einschließlich aller weiteren Geräte.
Alternativ die VS-Code-Aufgabe **Build shared CCU addon from HY** verwenden.
Installationspaket: `../../dist/hb-sr-devices-addon.tgz`.

## Hardwarebedienung

- D4: AskSin++-Status-LED; D5 wird derzeit nicht angesteuert.
- D8 kurz drücken und loslassen: Anlernen starten.
- D8 im laufenden Betrieb etwa 8 Sekunden halten: Reset-Vorwarnung; weiter bis
  etwa 16 Sekunden halten: Werksreset. Die 8 Sekunden ergeben sich derzeit aus
  dem zweiten Konstruktorargument von `ConfigButton`.
- D8 beim Einschalten gedrückt halten: Werksreset nach der zusätzlichen
  3,5-Sekunden-Prüfung im Startablauf.

Ein Werksreset löscht die Geräte-Konfiguration und die Verknüpfungen.
Nach einem HY-Neustart kann der originale VD eine Resynchronisation benötigen.

## Dokumentation

- [TC-/VD-Protokoll und Messungen](docs/HM-CC-TC_HM-CC-VD_Protokoll.md)
- [CCU-Diagnose](docs/ccu_debug.md)
- `docs/reference/`: originale TC-/VD-Gerätebeschreibungen als Referenz
- `tools/convert_key.py`: bisheriges Hilfsskript
