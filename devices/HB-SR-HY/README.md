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

## Konfiguration in der CCU

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
