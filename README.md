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

Die Version des gemeinsamen **HB-SR-Devices AddOns** steht in
[`addon/VERSION`](addon/VERSION); der aktuelle Quellstand ist **1.0**.
Der Paketname bleibt `hb-sr-devices-addon.tgz`.
Die Firmware-Version jedes Geräts wird unabhängig davon gepflegt. Eine
Addon-Installation aktualisiert die CCU-Gerätebeschreibungen und die
Addon-Skripte, aber nicht die Firmware auf den Geräten.

Für eine neue Addon-Version:

1. Die gewünschte Version in `addon/VERSION` eintragen.
2. `python3 -m unittest discover -s scripts/tests -v` und
   `sh scripts/build-addon.sh` ausführen und das erzeugte Paket prüfen.
3. Die Quelländerungen einschließlich der erzeugten Versionsdatei
   `CCU_RM/src/addon/VERSION` committen und nach `main` pushen.
4. Im GitHub-Repository `sruetzler/HB-SR-Devices-AddOn` ein Release für diesen
   Commit erstellen. Der Tag muss zur Addon-Version passen: für `1.0` also
   `v1.0` oder `1.0`.
5. `dist/hb-sr-devices-addon.tgz` und optional die zugehörige
   `dist/hb-sr-devices-addon.tgz.sha256` als Release-Assets anhängen.
   Das Release veröffentlichen und als neuestes Release markieren.
   Entwürfe und Vorabversionen werden bei der Update-Abfrage nicht angeboten.

Die aktuelle Update-Abfrage verwendet die GitHub-API `releases/latest`.
Der Download-Button lädt das Paket aus diesem Release. Fehlt das passende
Paket oder schlägt die Abfrage fehl, wird `n/a` gemeldet.
Der lokale Build veröffentlicht nichts; ein Push allein stellt das Paket
für diesen Update-Ablauf noch nicht bereit.

Ältere installierte Addons verwenden stattdessen die bisherigen GitHub-Pfade
unter `CCU_RM/`. Dafür erzeugt der Build zusätzlich
`CCU_RM/src/addon/VERSION` und `CCU_RM/hb-sr-devices-addon.tgz`.
Diese Dateien nicht von Hand bearbeiten. Die Versionsdatei ist in Git erfasst,
das Paket wird durch `.gitignore` ausgeschlossen. Sollen auch diese älteren
Addons das Update erhalten, muss das erzeugte Paket vor dem Commit mit
`git add -f CCU_RM/hb-sr-devices-addon.tgz` aufgenommen und zusammen mit der
Versionsdatei nach `main` gepusht werden. Ein Release-Asset allein bedient die
alten Download-Pfade nicht.

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
