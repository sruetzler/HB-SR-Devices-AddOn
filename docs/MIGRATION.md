# Zusammenführung zu HB-SR-Devices

Ausgangsprojekte:

- `MyDevice`: HY-Firmware, aktuelle HY-XML und Protokollnotizen.
- `HB-SR-Devices-AddOn`: gemeinsames Addon samt WebUI-Bildern, Installer,
  Deinstallation, CCU-Registrierung und GitHub-Update-Abfrage.

Das neue Repository führt beide Git-Historien in einem Merge zusammen. Die
GitHub-Remote-Adresse bleibt `git@github.com:sruetzler/HB-SR-Devices-AddOn.git`.
Es wurde nichts gepusht oder auf der CCU installiert.

## Zuordnung

| Bisher | Jetzt |
| --- | --- |
| `MyDevice/platformio.ini` | `devices/HB-SR-HY/platformio.ini` |
| `MyDevice/src/HB-SR-HY.ino` | `devices/HB-SR-HY/src/HB-SR-HY.ino` |
| `MyDevice/addon/firmware/rftypes/hb-sr-hy.xml` | `devices/HB-SR-HY/ccu/hb-sr-hy.xml` |
| Protokollnotizen und TC-/VD-Referenz-XML | `devices/HB-SR-HY/docs/` |
| `MyDevice/src/convert_key.py` | `devices/HB-SR-HY/tools/convert_key.py` |
| Gemeinsame `CCU_RM/src/`-Dateien | `addon/src/` |
| HY-Installer und Bilder aus `CCU_RM/src/addon/` | `devices/HB-SR-HY/ccu/` |
| Addon-Version | `addon/VERSION` |
| Paketbau | `scripts/build-addon.sh` |

Die alternative `MyDevice/src/platformio.ini` und der separate HY-Addon-Installer
sind nicht mehr Teil des aktiven Projekts. Sie bleiben in der alten Git-Historie
und in den ursprünglichen Ordnern erhalten. Historische READMEs und die bisher
unversionierten XML-Experimente liegen unter `docs/history/`; sie beschreiben
teilweise ältere Stände und sind keine Paketquellen.

Die Quellordner `MyDevice` und `HB-SR-Devices-AddOn` wurden nicht verändert.
Zusätzlich liegen lokale Dateisicherungen beider Arbeitsverzeichnisse unter
`.git/migration-snapshots/` im neuen Repository (ohne `.git` und `.pio`).
Diese Sicherungen werden nicht zu GitHub übertragen.

## Bestehende Update-URLs

Der Build erzeugt weiterhin:

- `CCU_RM/src/addon/VERSION`
- `CCU_RM/hb-sr-devices-addon.tgz`

So können bereits installierte Addons die neue Version unter den bisherigen
GitHub-Adressen finden, sobald die Änderungen auf `main` veröffentlicht werden.
`addon/VERSION` ist die Quelle; die Version unter `CCU_RM/` ist ein Build-Ergebnis.

Firmware, Geräteidentität und EEPROM-Layout bleiben beim Umbau unverändert.
Die aktuelle bereinigte XML aus MyDevice ersetzt die veraltete XML des bisherigen
Sammel-Addons. Der gemeinsame Installer wird ansonsten unverändert übernommen.
