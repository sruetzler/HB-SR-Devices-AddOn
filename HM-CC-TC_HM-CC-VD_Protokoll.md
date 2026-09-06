# HomeMatic HM-CC-TC ↔ HM-CC-VD – Protokolldokumentation

Stand: 2026-09-06

## 1. Scope

Dieses Dokument beschreibt die direkte Funkkommunikation zwischen:

- **HM-CC-TC** – HomeMatic Funk-Wandthermostat
- **HM-CC-VD** – HomeMatic Funk-Stellantrieb

Betrachtet wird die bereits **dekodierte BidCoS-Telegrammebene**:

```text
COUNTER CTRL TYPE SRC DST PAYLOAD
```

Nicht Bestandteil dieses Dokuments sind:

- physikalische 868-MHz-Funkebene
- Preamble / Sync / Whitening / CRC-Berechnung
- Anlernen / Peering
- Kommunikation mit einer CCU

Die Dokumentation basiert auf:

1. eQ-3-Gerätehandbüchern,
2. öffentlich dokumentiertem BidCoS-Reverse-Engineering,
3. FHEM-Implementierung für HM-CC-TC/HM-CC-VD,
4. realen HMLAN/FHEM-Funkmitschnitten.

Da das proprietäre BidCoS-Protokoll nicht vollständig offiziell veröffentlicht wurde, wird zwischen **belegt**, **sehr wahrscheinlich**, **rekonstruiert** und **offen** unterschieden.

---

# 2. Grundprinzip

Der HM-CC-TC ist der eigentliche Raumregler.

Er:

1. misst die Raumtemperatur,
2. kennt Solltemperatur und Zeitprogramm,
3. berechnet daraus eine gewünschte Ventilöffnung,
4. überträgt diese zyklisch an den HM-CC-VD.

Die Regelung benötigt im normalen Betrieb keine CCU.

eQ-3 beschreibt einen zyklischen Kommunikationsabstand von:

```text
120 ... 184 Sekunden
```

Der HM-CC-VD bestätigt den empfangenen Stellbefehl und meldet dabei seinen aktuellen Zustand zurück.

---

# 3. Allgemeiner dekodierter BidCoS-Frame

Auf der hier betrachteten Ebene besteht ein Telegramm aus:

```text
LEN CNT CTRL TYPE SRC SRC SRC DST DST DST PAYLOAD...
```

Dabei:

| Feld | Größe | Bedeutung |
|---|---:|---|
| LEN | 1 Byte | Länge ab `CNT` bis Ende Payload |
| CNT | 1 Byte | Message Counter / Sequence Number |
| CTRL | 1 Byte | Steuerflags |
| TYPE | 1 Byte | Nachrichtentyp |
| SRC | 3 Byte | 24-Bit-Senderadresse |
| DST | 3 Byte | 24-Bit-Empfängeradresse |
| PAYLOAD | variabel | typabhängige Nutzdaten |

Die 24-Bit-Adressen sind Geräteadressen.

---

# 4. Message Counter

Der Message Counter ist 8 Bit breit:

```text
00 ... FF
```

und läuft modulo 256:

```text
FC
FD
FE
FF
00
01
...
```

Er wird bei einer neuen Kommunikationssequenz erhöht.

Eine Anfrage und die zugehörige Antwort verwenden **denselben Counter**.

Beispiel:

```text
TC → VD
CNT=4A A2 58 ...

VD → TC
CNT=4A 82 02 ...
```

Der nächste reguläre Zyklus verwendet anschließend beispielsweise:

```text
CNT=4B
```

---

# 5. Control-Byte

Für die hier relevanten Telegramme sind besonders folgende Bits wichtig:

| Bit | Maske | Bedeutung |
|---:|---:|---|
| 0 | `0x01` | WakeUp |
| 1 | `0x02` | WakeMeUp |
| 2 | `0x04` | Broadcast |
| 4 | `0x10` | Burst |
| 5 | `0x20` | ACK Request |
| 6 | `0x40` | Repeated |
| 7 | `0x80` | Repeat Enable |

## 5.1 `CTRL = 0xA2`

Normale TC→VD-Stelltelegramme verwenden typischerweise:

```text
A2 = 1010 0010
```

gesetzt sind:

```text
0x80 Repeat Enable
0x20 ACK Request
0x02 WakeMeUp
```

Nicht gesetzt ist insbesondere:

```text
0x10 Burst
```

Das normale Stelltelegramm wird also **nicht als Burst** gesendet.

### Bedeutung von WakeMeUp

`WakeMeUp` bedeutet nicht:

> Wecke den Empfänger auf.

Sondern sinngemäß:

> Der Sender ist nach diesem Telegramm noch für kurze Zeit empfangsbereit.

Beim `A2 58` bezieht sich das daher auf den HM-CC-TC als Sender.

Der HM-CC-VD muss bereits aufgrund seines zyklischen Empfangstimings wach sein.

## 5.2 `CTRL = 0x82`

Reale HM-CC-VD-Antworten werden häufig beobachtet als:

```text
82 02
```

`0x82` enthält:

```text
Repeat Enable
WakeMeUp
```

FHEM kann bei einer virtuellen Aktorantwort auch `0x80 02` erzeugen. Für die originale VD-Kommunikation ist `0x82` der typische beobachtete Wert.

---

# 6. TC → VD: CLIMATE_EVENT `TYPE=0x58`

Das zentrale Stelltelegramm lautet:

```text
CNT A2 58 TC_ID VD_ID COMMAND TARGET
```

Mit Length-Byte:

```text
0B CNT A2 58 TC_ID VD_ID COMMAND TARGET
```

## 6.1 Felder

| Feld | Bedeutung |
|---|---|
| CNT | globaler Message Counter des TC |
| CTRL | normalerweise `A2` |
| TYPE | `58` = `CLIMATE_EVENT` |
| SRC | Adresse des HM-CC-TC |
| DST | Adresse des HM-CC-VD |
| Payload 0 | `ADJUSTING_COMMAND` |
| Payload 1 | `ADJUSTING_DATA` / Ventil-Sollwert |

FHEM kommentiert:

```text
adjust_command [0..4]
adj_data       [0..250]
```

## 6.2 Ventil-Sollwert

Der normale Ventil-Sollwert ist ungefähr:

```text
Sollposition [%] = TARGET_RAW / 2.56
```

bzw.

```text
TARGET_RAW ≈ Sollposition [%] × 2.56
```

Beispiele:

| Sollöffnung | Raw ungefähr |
|---:|---:|
| 0 % | `00` |
| 25 % | `40` |
| 50 % | `80` |
| 75 % | `C0` |

FHEM dekodiert den Wert mit:

```text
int(raw / 2.56 + 0.5)
```

Die Sonderfälle Voll-Auf/Voll-Zu können zusätzlich über `ADJUSTING_COMMAND` repräsentiert werden und sollten deshalb nicht ausschließlich aus dem Raw-Wert abgeleitet werden.

---

# 7. `ADJUSTING_COMMAND`

Der Wertebereich ist:

```text
00 ... 04
```

Die vollständige Enum-Bedeutung ist von eQ-3 öffentlich nicht sauber dokumentiert.

Der aktuelle Rekonstruktionsstand ist:

| Wert | vermutete Bedeutung | Sicherheit |
|---:|---|---|
| `00` | normaler zyklischer Stellwert / Refresh | sehr hoch |
| `01` | vermutlich `VENT_OPEN` | offen / Hypothese |
| `02` | `VENT_CLOSED` / OFF | hoch |
| `03` | neuer/geänderter Stellwert | sehr hoch |
| `04` | Entkalkungs-/Ventilfahrt | mittel bis hoch |

## 7.1 `00` – normaler zyklischer Stellwert

Bei unverändertem Sollwert werden reale Telegramme der Form:

```text
A2 58 ... 00 XX
```

beobachtet.

FHEMs TC-Emulation verwendet `00` für die folgenden zyklischen Wiederholungen eines bereits gesetzten Ventilwerts.

Interpretation:

```text
COMMAND = 00
DATA    = aktueller Ventilsollwert
```

## 7.2 `03` – neuer Stellwert

FHEM setzt bei einer Änderung der Ventilposition zunächst:

```text
COMMAND = 03
```

und danach wieder:

```text
COMMAND = 00
```

Ein typischer Ablauf ist daher:

```text
03 <neuer Sollwert>
00 <gleicher Sollwert>
00 <gleicher Sollwert>
00 <gleicher Sollwert>
```

Reale Funkmitschnitte passen zu diesem Verhalten.

## 7.3 `02` – VENT_CLOSED / OFF

Bei `desired-temp = off` wurden reale Telegramme beobachtet wie:

```text
A2 58 ... 02 00
```

Der VD schließt daraufhin das Ventil.

Daher ist sehr wahrscheinlich:

```text
COMMAND = 02
DATA    = 00
```

für den expliziten Sonderzustand `VENT_CLOSED`.

Dies ist semantisch von einem normalen:

```text
00 00
```

zu unterscheiden, auch wenn das mechanische Ergebnis ebenfalls ein geschlossenes Ventil sein kann.

## 7.4 `01` – vermutlich VENT_OPEN

Es gibt einen naheliegenden Zusammenhang mit dem HomeMatic-Sonderzustand `VENT_OPEN`.

Die Zuordnung:

```text
01 = VENT_OPEN
```

ist jedoch noch nicht durch einen ausreichend eindeutigen realen Mitschnitt bestätigt.

**Status: offen.**

## 7.5 `04` – Entkalkungsfahrt

`04` wurde im Zusammenhang mit der regelmäßigen Ventil-/Entkalkungsfahrt beobachtet.

Noch offen sind:

- genaue Bedeutung von `ADJUSTING_DATA`,
- vollständige Telegrammsequenz,
- Verhalten während der Adaptions-/Entkalkungsfahrt.

---

# 8. VD → TC: RESPONSE_ACK_STATUS

Der HM-CC-VD antwortet auf das `0x58` unmittelbar mit einem Response-Telegramm.

Typischer Aufbau:

```text
CNT 82 02 VD_ID TC_ID 01 01 POSITION STATUS RSSI
```

Mit Length:

```text
0E CNT 82 02 VD_ID TC_ID 01 01 POSITION STATUS RSSI
```

Dabei:

```text
TYPE    = 02 = RESPONSE
SUBTYPE = 01 = RESPONSE_ACK_STATUS
```

## 8.1 Felder

| Payload | Bedeutung |
|---:|---|
| Byte 0 | `01` = ACK_STATUS |
| Byte 1 | Kanal, normalerweise `01` |
| Byte 2 | aktuelle Ventilposition |
| Byte 3 | Status / Motor / Fehler |
| Byte 4 | RSSI-/Linkwert |

Der Antwort-Counter ist identisch mit dem Counter des auslösenden `0x58`.

---

# 9. Aktuelle Ventilposition des VD

Die VD-Rückmeldung verwendet **eine andere Skalierung als der TC-Sollwert**.

Es gilt:

```text
Istposition [%] = POSITION_RAW / 2
```

bzw.

```text
POSITION_RAW = Istposition [%] × 2
```

Beispiele:

| Istposition | Raw |
|---:|---:|
| 0 % | `00` |
| 10 % | `14` |
| 25 % | `32` |
| 37 % | `4A` |
| 50 % | `64` |
| 100 % | `C8` |

Damit gilt ausdrücklich:

```text
TC → VD Sollwert: raw / 2.56

VD → TC Istwert: raw / 2
```

Diese unterschiedlichen Skalierungen sind durch FHEM und reale Mitschnitte gut belegt.

---

# 10. VD-Statusbyte

Das Statusbyte wird bisher wie folgt verstanden:

```text
Bit:  7 6 5 4 3 2 1 0
      │ │ └─┬─┘ └─┬─┘ │
      │ │   │     │   └─ unbekannt
      │ │   │     └──── Fehlercode
      │ │   └────────── Motorzustand
      │ └────────────── unbekannt
      └──────────────── Low Battery
```

## 10.1 Motorzustand – Bits 5..4

Maske:

```text
STATUS & 0x30
```

Bekannt:

| Wert | Zustand |
|---:|---|
| `0x00` | Motor steht |
| `0x10` | Motor öffnet |
| `0x20` | Motor schließt |
| `0x30` | Bedeutung noch offen |

FHEM verwendet:

```text
0x00 → stop
0x10 → opening
0x20 → closing
```

## 10.2 Fehler – Bits 3..1

Dekodierung:

```text
error = (STATUS >> 1) & 0x07
```

Bekannte Codes:

| Code | Bedeutung |
|---:|---|
| 0 | kein Fehler |
| 1 | Ventil blockiert / schwergängig |
| 2 | Antrieb locker / nicht korrekt montiert bzw. Stellproblem |
| 3 | Stellbereich zu klein |
| 4 | Batterie kritisch |

Die Anzeigen des originalen VD entsprechen den bekannten Fehlern F1...F4.

## 10.3 Batterie

Zusätzlich wird:

```text
STATUS & 0x80
```

als `Low Battery` ausgewertet.

Damit gibt es mindestens eine Unterscheidung zwischen:

- Batterie niedrig (`Bit 7`)
- Batterie kritisch (`Errorcode 4`)

## 10.4 Noch unbekannte Statusbits

Offen:

```text
Bit 6
Bit 0
Motorcode 0x30
```

---

# 11. ACK-Timing

Reale Mitschnitte zeigen, dass der HM-CC-VD sehr schnell antwortet.

Beobachtete Größenordnung:

```text
TC A2 58
   ↓
ca. 10...15 ms
   ↓
VD 82 02
```

Dies ist die eigentliche Funkreaktionszeit.

Ein in FHEM verwendeter 10-Sekunden-Timer ist lediglich ein großzügiger Software-Watchdog zur Erkennung eines ausgebliebenen Status und **nicht** die erwartete Funk-ACK-Latenz.

---

# 12. Zyklisches Empfangs- und Sendetiming

Der originale HM-CC-VD ist batteriebetrieben und empfängt nicht dauerhaft.

TC und VD verwenden einen deterministischen pseudozufälligen Zeitplan.

eQ-3 dokumentiert einen Zyklus von:

```text
120 ... 184 s
```

Die reverse-engineerte und von FHEM verwendete Formel lautet:

```c
result =
    (((address << 8) | messageCounter)
     * 1103515245 + 12345) >> 16;

interval =
    (result & 0xff) / 4.0 + 120.0;
```

Damit:

```text
Minimum: 120.00 s
Maximum: 183.75 s
Raster:    0.25 s
```

Die ganzzahlige Form in Viertelsekunden ist:

```text
(result & 0xff) + 480
```

---

# 13. Welche Adresse geht in die Timingformel ein?

Verwendet wird die Adresse des **HM-CC-TC**, also des Senders der Stelltelegramme.

Formal:

```text
T = f(TC_ADDRESS, MESSAGE_COUNTER)
```

Interessante mathematische Eigenschaft:

Von der 24-Bit-TC-Adresse wirken für das letztlich verwendete Ergebnis effektiv nur die **unteren 16 Bit**.

Bei:

```text
Adresse = AA BB CC
```

beeinflussen effektiv:

```text
BB CC
```

das Intervall.

FHEMs optimierte Implementierung bildet entsprechend nur diese beiden Bytes in seinen vorberechneten Timingwerten ab.

---

# 14. Welcher Counter bestimmt das nächste Intervall?

Der Counter des **gerade gesendeten TC-Telegramms** bestimmt das Intervall bis zum nächsten globalen TC-Slot.

Beispiel:

```text
CNT=4A  A2 58
   ↓
   T(TC-ID, 4A)
   ↓
CNT=4B  A2 58
   ↓
   T(TC-ID, 4B)
   ↓
CNT=4C  A2 58
```

Dies lässt sich anhand realer Mitschnitte und der Timingformel nachvollziehen.

---

# 15. FHEM-Timingoffset

FHEMs virtuelle TC-Implementierung besitzt zusätzlich:

```text
cyclicMsgOffset
```

mit einem Default von ungefähr:

```text
+200 ms
```

zum berechneten Zeitpunkt.

Das ist ein praktischer Implementierungswert von FHEM.

Er beweist **nicht**, dass der originale VD exakt bei `T+200 ms` sein Empfangsfenster öffnet.

Mögliche Ursachen des Offsets sind unter anderem:

- tatsächliche Lage des originalen Empfangsfensters,
- Scheduling-Latenzen,
- HMLAN/CUL-Transportlatenzen,
- Kombination daraus.

---

# 16. Exakte Breite des VD-Empfangsfensters

Die exakte Lage und Dauer des normalen stromsparenden RX-Fensters sind derzeit **nicht belastbar bekannt**.

Bekannt ist:

```text
Zeitpunkt des Zyklus = berechenbar
```

Unbekannt:

```text
RX start = T - ?
RX end   = T + ?
```

Dieser Punkt bleibt bewusst offen.

---

# 17. Verlust einzelner Telegramme

Die Synchronisation ist so ausgelegt, dass nicht jedes Stelltelegramm zwingend empfangen werden muss.

FHEM kann bei seiner virtuellen TC-Implementierung gezielt zyklische Nachrichten auslassen (`msgReduce`).

Die FHEM-Dokumentation warnt:

```text
VD will lose connection with more then 5 skips
```

Dies ist ein starker Hinweis darauf, dass ein VD einige verpasste Zyklen toleriert.

Ein plausibles internes Modell ist:

```text
letzten Counter kennen
      ↓
nächste Counter selbst hochzählen
      ↓
für jeden Counter nächsten Slot berechnen
      ↓
nur bei zu vielen Ausfällen Sync verlieren
```

Die exakte originale interne Grenze sollte trotzdem als Implementierungsdetail betrachtet werden; FHEMs `>5` ist kein veröffentlichter Firmwarecode von eQ-3.

---

# 18. Verhalten bei fehlendem ACK

Für FHEMs TC-Emulation gilt:

1. ein `A2 58` wird gesendet,
2. FHEM wartet auf eine neue VD-Positionsmeldung,
3. nach 10 s wird ein `miss` gezählt,
4. nach mehr als fünf fehlenden Antworten wird der Zustand `lost`,
5. der normale zyklische Zeitplan läuft weiter.

In der FHEM-TC-Emulation ist kein sofortiges erneutes `A2 58` als Funk-Retry vorgesehen.

Für den **originalen HM-CC-TC** ist noch nicht vollständig belegt:

- ob es einen unmittelbaren Retry gibt,
- ob derselbe Counter verwendet würde,
- ob bei einem fehlenden VD der nächste reguläre Slot unverändert weiterläuft.

Ein sofortiger Retry ist derzeit **nicht belegt**.

Wichtig:

`Repeat Enable` (`CTRL Bit 7`) bedeutet nicht automatisch einen Retry durch den TC selbst. Es erlaubt insbesondere die Wiederholung durch einen BidCoS-Repeater.

---

# 19. Mehrere HM-CC-VD an einem TC

Reale Funkmitschnitte zeigen bei einem TC mit mehreren VDs:

```text
CNT=BA → VD1
CNT=BB → VD2
CNT=BC → VD1
CNT=BD → VD2
...
```

Damit ist für einen beobachteten Zwei-VD-Fall sehr gut belegt:

- ein **gemeinsamer globaler TC-Counter**,
- ein **gemeinsamer globaler Timingzyklus**,
- pro Slot wird ein VD adressiert,
- die VDs werden abwechselnd bedient.

Nicht beobachtet wurde:

```text
VD1 und VD2 direkt hintereinander mit demselben Slot/Counter
```

## 19.1 Round-Robin

Für zwei VDs ist ein alternierender Ablauf belegt:

```text
VD1 → VD2 → VD1 → VD2 → ...
```

Für drei oder vier VDs ist:

```text
VD1 → VD2 → VD3 → VD4 → VD1 ...
```

eine sehr plausible Rekonstruktion, aber bislang nicht durch einen entsprechenden vollständigen Mitschnitt bestätigt.

## 19.2 Timing bleibt global

Das nächste Intervall hängt weiterhin nur von:

```text
TC-Adresse
+
globalem Counter
```

ab.

Nicht von der Adresse des aktuell ausgewählten VD.

---

# 20. Synchronisation bei mehreren VDs

Hier ergibt sich eine wichtige Rekonstruktion.

Wenn mehrere VDs existieren, kann der Abstand zwischen zwei an denselben VD adressierten Stelltelegrammen deutlich größer als 184 s werden.

Gleichzeitig benötigt der VD laut Hersteller bei vollständigem Synchronisationsverlust maximal 184 s Dauerempfang, um wieder zu synchronisieren.

Das spricht stark dafür, dass ein VD auch Telegramme seines bekannten TC beobachten kann, die an **andere VDs** adressiert sind, um:

```text
TC-Counter
+
globalen Zyklus
```

mitzuverfolgen.

Beispiel:

```text
TC → VD1  CNT=80

TC → VD2  CNT=81
VD1 kann globalen TC-Zustand mithören

TC → VD3  CNT=82

TC → VD1  CNT=83
```

**Status:** starke Rekonstruktion, aber nicht durch veröffentlichten originalen Firmwarecode bestätigt.

---

# 21. `TYPE=0x70` WEATHER_EVENT

In realen HM-CC-TC-Mitschnitten erscheint zusätzlich:

```text
CTRL=86
TYPE=70
DST=000000
```

also ein Weather-/Broadcast-Telegramm.

Bei einigen Mitschnitten wurde beobachtet:

```text
t = -20 s
CNT=n  86 70 ...

t = 0
CNT=n  A2 58 ...
```

Das `0x70` und das nachfolgende `0x58` benutzen dabei denselben Counter.

Beispielstruktur:

```text
CNT=n 86 70 TC_ID 000000 ...
        |
        | ca. 20 s
        v
CNT=n A2 58 TC_ID VD_ID ...
```

Die `0x70`-Nachricht enthält unter anderem Temperatur-/Feuchteinformationen des TC.

Für die reine VD-Stellkommunikation ist bislang nicht belegt, dass der VD dieses Telegramm benötigt.

FHEMs virtuelle TC-Steuerung eines HM-CC-VD funktioniert grundsätzlich über die `0x58`-Kommunikation.

Daher:

**`0x70` ist Bestandteil des beobachteten TC-Funkverhaltens, aber seine direkte Relevanz für den VD bleibt offen.**

---

# 22. Resynchronisation des originalen HM-CC-VD

eQ-3 dokumentiert ausdrücklich:

Der VD ist zur Batterieschonung nicht dauerhaft auf Empfang.

Wenn die Funkverbindung längere Zeit gestört ist, versucht der VD:

```text
einmal pro Stunde
```

eine Neusynchronisation.

Dazu geht er:

```text
maximal 184 Sekunden
```

auf Dauerempfang.

Während des Kommunikationsfehlers fährt er die gespeicherte Störungsposition an.

Werkseinstellung laut Handbuch:

```text
15 %
```

Nach Batteriewechsel wird ebenfalls neu synchronisiert.

Die Synchronisation erfolgt:

- automatisch innerhalb einer Stunde,
- alternativ durch kurzen Tastendruck am VD sofort angestoßen.

---

# 23. Warum 184 Sekunden Resync plausibel sind

Das maximale reguläre TC-Intervall beträgt:

```text
183.75 s
```

Der VD hört bei Resync maximal:

```text
184 s
```

Damit umfasst das Dauerempfangsfenster mindestens einen vollständigen maximalen TC-Zyklus.

Dies ist eine sehr plausible Erklärung für die Wahl der 184 s.

Die Schlussfolgerung ist logisch aus den beiden Zeitwerten abgeleitet; eQ-3 erklärt diesen Zusammenhang im Handbuch nicht ausdrücklich.

---

# 24. Neustart / Batteriewechsel des VD

Nach einem normalen Batteriewechsel:

- die Peer-/Partnerbeziehung muss nicht neu angelernt werden,
- die Funk-Synchronisation muss neu aufgebaut werden.

Daraus folgt:

```text
VD Neustart
   ↓
Peer-Daten weiterhin vorhanden
   ↓
zyklischer Timingzustand nicht sicher vorhanden
   ↓
Resync
   ↓
normaler Betrieb
```

Für die Synchronisation benötigt der VD prinzipiell wieder:

```text
TC-Adresse
aktuellen Counter
aktuellen Empfangszeitpunkt
```

Ein gültiges Telegramm des bekannten TC reicht daher als plausible Grundlage, um den zukünftigen Zeitplan erneut zu bestimmen.

---

# 25. Neustart des TC

Beim Batteriewechsel des HM-CC-TC bleiben die angelernten Geräte grundsätzlich erhalten; Datum/Uhrzeit müssen nach dem Batteriewechsel wieder gesetzt bzw. synchronisiert werden.

Nicht belastbar geklärt ist:

```text
Bleibt der Message Counter über einen TC-Power-Cycle erhalten?
```

Möglichkeiten:

1. Counter persistent,
2. Counter startet neu,
3. anderer Initialwert.

**Status: offen.**

Falls der TC nach einem Neustart seinen Counter oder seine zeitliche Phase verliert, kann ein originaler VD zunächst die Synchronisation verlieren und später über seinen stündlichen 184-s-Resync wieder einsteigen.

---

# 26. FHEM-Verhalten bei Neustart eines virtuellen TC

FHEM speichert für seinen virtuellen TC:

```text
Message Counter
+
nächsten Sendetermin
```

in einem Reading `.next`.

Beim Wiederanlauf werden diese Werte wieder geladen.

Dies zeigt, wie ein TC-Nachbau den Zustand über einen Neustart erhalten kann.

Es beweist jedoch **nicht**, dass der originale HM-CC-TC intern genau dasselbe macht.

---

# 27. Reale Beispieltelegramme

## 27.1 Normaler Stellbefehl

```text
TC → VD
30 A2 58 1CE246 1CEB87 00 60
```

Interpretation:

```text
COMMAND = 00
TARGET  = 0x60 = 96
96 / 2.56 = 37.5 %
```

Antwort:

```text
VD → TC
30 82 02 1CEB87 1CE246 01 01 4A 00 36
```

Position:

```text
0x4A = 74
74 / 2 = 37 %
```

Damit stehen Soll und Ist praktisch auf derselben Position.

## 27.2 Motor schließt

Beispiel:

```text
TC → VD
AB A2 58 1BF7FA 1BFC82 03 16
```

Antwort:

```text
VD → TC
AB 82 02 1BFC82 1BF7FA 01 01 14 20 39
```

Position:

```text
0x14 / 2 = 10 %
```

Status:

```text
0x20 → Motor schließt
```

Der VD meldet also sofort seine aktuelle Position und den laufenden Motorzustand.

Er wartet nicht, bis das neue Ziel erreicht ist.

---

# 28. Wesentliche Zustandsmaschine im Normalbetrieb

Vereinfacht:

```text
HM-CC-TC                           HM-CC-VD
    |                                  |
    |                                  | sleep
    |                                  |
    |     berechneter globaler Slot    |
    |                                  |
    | A2 58                            |
    | Sollposition                     |
    |--------------------------------->|
    |                                  |
    |                            Soll übernehmen
    |                            ggf. Motor starten
    |                                  |
    |        82 02 ACK_STATUS          |
    |        Istposition + Status      |
    |<---------------------------------|
    |                                  |
    |                                  | sleep
    |                                  |
    | T(TC-ID, Counter)                |
    |----------------------------------|
    |                                  |
    | nächster Counter / Slot          |
```

---

# 29. Was als gesichert bzw. ausreichend belegt gilt

## Sehr gut belegt

- TC ist der eigentliche Regler.
- VD erhält zyklisch Ventilbefehle.
- Zyklus liegt bei 120...184 s.
- `TYPE=58` ist das zentrale TC→VD-Climate-Event.
- Payload enthält `ADJUSTING_COMMAND` + Stellwert.
- TC-Sollwert wird ungefähr mit `/2.56` dekodiert.
- VD antwortet mit `TYPE=02`, Subtype `01` ACK_STATUS.
- ACK verwendet denselben Counter.
- VD-Istposition wird mit `/2` dekodiert.
- Statusbits `opening`, `closing`, `stop`.
- Fehlercodes 0...4 wie dokumentiert.
- Timingformel 120.00...183.75 s.
- Timing ist abhängig von TC-Adresse + Message Counter.
- Counter läuft modulo 256.
- VD-Resync: einmal pro Stunde, max. 184 s Dauerempfang.
- FHEM kann als virtueller TC einen echten VD bedienen.

## Sehr wahrscheinlich / stark rekonstruiert

- `COMMAND=00` = normaler Refresh.
- `COMMAND=03` = neuer Stellwert.
- `COMMAND=02` = explizites VENT_CLOSED/OFF.
- globaler Counter bei mehreren VDs.
- ein VD pro globalem Slot.
- alternierende Bedienung bei zwei VDs.
- VDs können wahrscheinlich fremdadressierte `0x58` desselben TC für globale Synchronisation mithören.

---

# 30. Offene Punkte

Folgende Punkte sind noch nicht vollständig geklärt:

## Telegramme

- genaue Semantik `ADJUSTING_COMMAND=01`
- vollständige Semantik `ADJUSTING_COMMAND=04`
- Payload während Entkalkungs-/Adaptionsfahrt
- Bedeutung STATUS Bit 6
- Bedeutung STATUS Bit 0
- Bedeutung Motorcode `0x30`
- genaue Interpretation / physikalische Skalierung des letzten RSSI-Bytes

## Timing

- exakte Lage des normalen VD-RX-Fensters
- exakte Breite des normalen VD-RX-Fensters
- ob der FHEM-Defaultoffset von +200 ms ein originales Gerätetiming oder primär Software-/Gateway-Latenzen kompensiert

## Fehlerfälle

- exaktes Retry-Verhalten des originalen HM-CC-TC bei fehlendem ACK
- Verhalten des TC mit mehreren VDs, wenn ein einzelner VD dauerhaft nicht antwortet
- exakte Anzahl tolerierter Misses im originalen VD

## Neustart

- Message-Counter-Verhalten des originalen HM-CC-TC nach Power-Cycle
- Erhalt oder Neuanfang der exakten zeitlichen Phase nach TC-Neustart

## Mehrere VDs

- Round-Robin für drei oder vier VDs noch nicht direkt mit vollständigem Mitschnitt bestätigt

---

# 31. Bewusst nicht betrachtet

Für den aktuellen Anwendungsumfang nicht erforderlich:

- Pairing-/Teach-in-Sequenz
- CCU-Kommunikation
- AES-/Security-Handshakes
- rohe 868-MHz-Modulation
- Preamble
- Sync
- Whitening/Obfuscation
- CRC
- konkrete Funkchip-Konfiguration

---

# 32. Quellen

## S1 – BidCoS Reverse-Engineering

FKIE CAD – `bidcos-security-doc`

https://github.com/fkie-cad/bidcos-security-doc/blob/main/doc/1-bidcos.md

Relevant für:

- allgemeine BidCoS-Frame-Struktur
- Message Counter
- Control-Flags
- WakeMeUp
- Repeat Enable
- Message Types
- `0x02 RESPONSE`
- `0x01 RESPONSE_ACK_STATUS`
- `0x58 CLIMATE_EVENT`
- zyklische Empfangsstrategie

## S2 – FHEM HomeMatic Implementierung

`10_CUL_HM.pm`

https://raw.githubusercontent.com/mhop/fhem-mirror/master/fhem/FHEM/10_CUL_HM.pm

Relevant für:

- HM-CC-TC `0x58`-Dekodierung
- `adjust_command`
- Sollwert `/2.56`
- VD-Position `/2`
- VD-Statusbits
- Fehlercodes
- virtuelle TC-Implementierung
- Timingformel
- Message Counter
- Miss-Erkennung
- Persistieren von Counter und nächstem Termin
- `cyclicMsgOffset`

## S3 – HM-CC-TC Handbuch

HomeMatic HM-CC-TC Montage-/Bedienungsanleitung

Beispiel:

https://manualzz.com/doc/de/33500174/und-bedienungsanleitung-funk-wandthermostat-hm-cc-tc

Relevant für:

- TC ist Einzelraumregler
- autarke Regelung
- Ventilsteuerung
- Zykluszeit 120...184 s

## S4 – HM-CC-VD Handbuch

HomeMatic HM-CC-VD Installation and Operating Manual

https://manualzz.com/doc/64336738/homematic-hm-cc-vd-installation-and-operating-manual

Relevant für:

- VD nicht dauerhaft auf Empfang
- Synchronisationsverhalten
- einmal pro Stunde Resync
- max. 184 s Dauerempfang
- Störungsposition
- Fehleranzeigen
- Verhalten nach Batteriewechsel

## S5 – reale HMLAN/FHEM-Mitschnitte

Beispiele:

https://forum.fhem.de/index.php?topic=16926.0

https://forum.fhem.de/index.php?topic=17232.0

https://forum.fhem.de/index.php?topic=17232.15

Relevant für:

- reale `A2 58`-Frames
- reale `82 02`-Antworten
- `COMMAND=02`
- `COMMAND=03`
- Motorstatus
- zeitliche Abfolge
- `0x70` vor `0x58`
- mehrere VDs / Counterverhalten

---

# 33. Kurzreferenz

## TC → VD

```text
0B CNT A2 58 TC_ID VD_ID CMD TARGET
```

```text
TARGET[%] ≈ TARGET_RAW / 2.56
```

## VD → TC

```text
0E CNT 82 02 VD_ID TC_ID 01 01 POS STATUS RSSI
```

```text
POS[%] = POS_RAW / 2
```

## Status

```text
STATUS & 0x30:
00 stop
10 opening
20 closing
30 unknown
```

```text
ERROR = (STATUS >> 1) & 0x07

0 ok
1 blocked
2 loose / mount problem
3 adjusting range too small
4 battery critical
```

```text
STATUS & 0x80 → battery low
```

## Timing

```c
result =
    (((tcAddress << 8) | counter)
     * 1103515245 + 12345) >> 16;

interval =
    (result & 0xff) / 4.0 + 120.0;
```

```text
120.00 ... 183.75 s
```

## Counter

```text
00 ... FF → modulo 256
```

Request und zugehöriges ACK:

```text
gleicher Counter
```
