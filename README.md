# HB-SR-Devices

Gemeinsames Repository für eigene Homematic-Geräte und das zugehörige
**HB-SR-Devices AddOn** für die CCU. Aktuell enthalten: **HB-SR-HY**,
ein Protokoll-Proxy zwischen HM-CC-TC und HM-CC-VD für den hydraulischen Abgleich.

## HY entwickeln

In VS Code `devices/HB-SR-HY` öffnen. Dort liegt die `platformio.ini`;
Build, Upload und serieller Monitor laufen wie bisher über PlatformIO.
Alternativ `HB-SR-Devices.code-workspace` öffnen: HY und CCU-Addon erscheinen
als separate Ordner im selben Fenster.

```sh
cd devices/HB-SR-HY
pio run
pio run --target upload
pio device monitor
```

## CCU-Addon bauen

Voraussetzungen: Python 3 und eine POSIX-Shell. Im Projektverzeichnis:

```sh
sh scripts/build-addon.sh
```

Ergebnis: `dist/hb-sr-devices-addon.tgz` mit einer SHA-256-Prüfsumme daneben.
Auch die VS-Code-Aufgabe **Build shared CCU addon** baut dieses Paket.
Das bisherige `sh CCU_RM/build.sh` funktioniert weiterhin.

Die Gerätebeschreibungen liegen ausschließlich unter `devices/*/ccu/*.xml`.
Der Build übernimmt sie automatisch zusammen mit den jeweiligen Installations-
und Deinstallationsskripten sowie den Bildern. XML-Dateien müssen nicht mehr
von Hand kopiert werden. Der Installer wird beim lokalen Build nicht ausgeführt.

## Struktur

```text
devices/HB-SR-HY/       PlatformIO-Projekt, CCU-Dateien und Gerätedokumentation
addon/VERSION          Version des gemeinsamen Addons
addon/src/             Gemeinsamer Installer und Update-Abfrage
scripts/               Paketbau und Tests
dist/                  Lokales Installationspaket (nicht in Git)
CCU_RM/                Kompatibilität mit bestehenden Update-URLs
docs/history/          Historische Notizen und frühere XML-Experimente
```

Für ein weiteres Gerät einen eigenen Ordner `devices/<Gerätename>` mit
`ccu/<dateiname>.xml` anlegen. Optional kommen `ccu/install_<name>`,
`ccu/uninstall_<name>` und Bilder unter `ccu/www/` hinzu. Dateinamen und
Gerätetyp-IDs müssen eindeutig sein. Der gemeinsame Build sammelt alle Geräte.

## Versionen und Veröffentlichung

Das gemeinsame Addon heißt weiterhin **hb-sr-devices-addon**. Version **0.03**
folgt auf dessen Version **0.02**. Die Versionen `1.0.x` bezeichneten das frühere,
separate HY-Paket; dessen aktuelle Gerätebeschreibung wurde hier übernommen.
Die Firmware-Version jedes Geräts bleibt unabhängig von der Addon-Version.

Für eine neue Addon-Version:

1. `addon/VERSION` ändern.
2. `sh scripts/build-addon.sh` ausführen und das Paket prüfen.
3. Die Quelländerungen und die erzeugten Dateien
   `CCU_RM/src/addon/VERSION` sowie `CCU_RM/hb-sr-devices-addon.tgz` committen.
4. Nach Freigabe nach `main` im bisherigen GitHub-Repository
   `sruetzler/HB-SR-Devices-AddOn` pushen.

Die beiden erzeugten Dateien unter `CCU_RM/` sind bewusst weiter in Git: Bereits
installierte Addons fragen genau diese GitHub-Pfade ab. Nicht von Hand ändern.
Der Build veröffentlicht nichts; erst ein Push stellt die neue Version bereit.

Das Paket wird wie bisher unter **Einstellungen → Systemsteuerung → Zusatzsoftware**
installiert. Die Geräteidentität von HY (`HB-SR-HY`, Modell `0xFE01`) bleibt gleich.
Für den Repository-Umbau ist kein neuer Firmware-Upload erforderlich.

## Prüfungen

```sh
python3 -m unittest discover -s scripts/tests -v
sh scripts/build-addon.sh
```

Die ursprünglichen Repository-Historien sind durch den Migrations-Merge erhalten.
`archive/hy-before-merge` zeigt zusätzlich auf den letzten HY-Stand vor dem Umbau.
Details: [Migration](docs/MIGRATION.md).

## Herkunft und Lizenz

Das gemeinsame Addon basiert auf
[TomMajors HB-TM-Devices-AddOn](https://github.com/TomMajor/SmartHome/tree/master/HB-TM-Devices-AddOn).
Die bestehenden Urheber- und Lizenzhinweise in den übernommenen Dateien gelten
weiter. Für das ursprüngliche Addon nennt die übernommene README
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/);
sie ist unter `docs/history/addon-README.md` erhalten.
