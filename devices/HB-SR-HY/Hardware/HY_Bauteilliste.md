# HY – Bauteilliste

Stand: 26.09.2026

## Elektronik

| Menge | Bauteil | Typ / Wert | Hinweis |
|---:|---|---|---|
| 1 | Mikrocontroller | **ATmega328P-PU**, PDIP-28 | Betrieb mit 3,3 V / 8 MHz |
| 1 | IC-Sockel | DIP-28, 2,54 mm | Für den ATmega328P |
| 1 | Funkmodul | **RadioControlli RC-CC1101-SPI-868** | 868,3 MHz, THT |
| 1 | Spannungsregler | **MCP1700-3302E/TO** | 3,3 V, TO-92 |
| 1 | Quarz | **8,000 MHz, HC49S, CL = 20 pF, THT** | Für den ATmega328P |
| 2 | Keramikkondensator | **20 pF** (`200`) | Lastkondensatoren für den 8-MHz-Quarz |
| 3 | Keramikkondensator | **100 nF** (`104`) | ATmega VCC, ATmega AVCC, CC1101 |
| 1 | Keramikkondensator | **100 nF** (`104`) | AREF nach GND |
| 2 | Elektrolytkondensator | **1 µF** | Ein- und Ausgang MCP1700 |
| 2 | Elektrolytkondensator | **10 µF** | 5-V-Eingangspuffer und CC1101-Puffer |
| 1 | Widerstand | **10 kΩ** | RESET-Pull-up |
| 1 | Widerstand | **1 kΩ** | Vorwiderstand Status-LED |
| 1 | LED | **3 mm THT** | Status-LED |
| 2 | Taster | **Omron B3F-1000S** | Config/Pairing und Reset |

## Anschlüsse

| Menge | Bauteil | Typ / Raster | Verwendung |
|---:|---|---|---|
| 1 | Micro-USB-B Breakout | **Pololu 2586** | 5-V-Versorgung; D+, D− und ID unbeschaltet |
| 1 | ISP-Wannenstecker | **Würth 61200621621**, 2×3, 2,54 mm, THT | AVR-ISP |
| 1 | Buchsenleiste | **1×6, 2,54 mm, gerade** | Direkter Anschluss des vorhandenen FT232RL |
| 1 | Streifenrasterplatine | 2,54 mm | Durchgehende Kupferstreifen |

### ISP-Belegung

| Pin | Signal |
|---:|---|
| 1 | MISO |
| 2 | +3,3 V |
| 3 | SCK |
| 4 | MOSI |
| 5 | RESET |
| 6 | GND |

### FT232RL-Buchsenleiste

Die 1×6-Buchsenleiste wird passend zum vorhandenen FT232RL angeordnet.

| FT232RL-Pin | Verbindung auf der HY |
|---|---|
| DTR | NC |
| RXD | ATmega TX / D1 / Pin 3 |
| TXD | ATmega RX / D0 / Pin 2 |
| VCC | NC |
| CTS | NC |
| GND | GND |

Die HY wird **nicht über den FT232RL versorgt**. Die Versorgung erfolgt über Micro-USB.

## Funk / Antenne

| Menge | Bauteil | Ausführung | Hinweis |
|---:|---|---|---|
| 1 | Antennendraht | ca. **86 mm** | Gerade Drahtantenne für 868 MHz |
| nach Bedarf | Anschlussdraht | isoliert, dünn | Für Drahtbrücken auf Streifenraster |

Die 86-mm-Antenne wird im Kunststoffgehäuse gerade entlang einer Gehäuseseite **neben der Platine** geführt, nicht über der Platinenfläche.

## Mechanik

| Menge | Bauteil | Hinweis |
|---:|---|---|
| 4 | M3-Schrauben | Für Platinenbefestigung |
| 4 | Abstandshalter | ca. 5–8 mm; im Antennenbereich bevorzugt Kunststoff/Nylon |
| 4 | Muttern | passend zu M3 |
| nach Bedarf | Gehäuse | Wird später passend zur fertigen Platine konstruiert |

Für die Platine sind vier freie Befestigungsbereiche vorgesehen. Die Streifenrasterlöcher können dort auf ca. **3,2 mm** für M3 aufgebohrt werden.

## Programmierung und Debugging

Diese Teile gehören nicht zwingend auf die HY-Platine, werden aber für Entwicklung und Inbetriebnahme benötigt.

| Menge | Gerät | Verwendung |
|---:|---|---|
| 1 | **Pololu USB AVR Programmer v2.1** | Programmieren des ATmega328P über ISP |
| 1 | 6-poliges AVR-ISP-Kabel | Verbindung Programmer ↔ HY |
| 1 | vorhandener **FT232RL USB-UART-Adapter** | Serielle Debug-Konsole |
| 1 | Micro-USB-Kabel / 5-V-Netzteil | Versorgung der HY |

## Feste ATmega-Pinbelegung

| Funktion | ATmega / Arduino-Pin |
|---|---|
| CC1101 GDO0 | D2 / Pin 4 |
| Status-LED | D4 / Pin 6 |
| Config-Taster | D8 / Pin 14 |
| CC1101 CSn | D10 / Pin 16 |
| CC1101 MOSI / SI | D11 / Pin 17 |
| CC1101 MISO / SO | D12 / Pin 18 |
| CC1101 SCK | D13 / Pin 19 |
| UART RX | D0 / Pin 2 |
| UART TX | D1 / Pin 3 |

## Bereits festgelegte mechanische Daten

- Streifenraster: **2,54 mm**
- Kupferstreifen der vorhandenen Platte: durchgehend
- Omron B3F-1000S passt direkt ins Raster:
  - ca. **7,62 mm × 5,08 mm**
  - entsprechend **3 × 2 Rasterabständen**
- CC1101:
  - Pinabstand innerhalb einer Reihe: **2,54 mm**
  - Abstand der beiden Pinreihen: ca. **20,32 mm**
- 1-µF- und 10-µF-Elkos:
  - Beinabstand ca. **2,54 mm**
  - Gehäusedurchmesser ca. **5 mm**
- FT232RL-Anschluss:
  - **1×6-Buchsenleiste, 2,54 mm**
- Vorgesehene Platinenfläche:
  - ungefähr **80 × 60 mm**
  - inklusive vier Befestigungspunkten
