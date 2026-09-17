# CCU-Diagnose für HB-SR-HY

Diese Pfade gelten für das gemeinsame **HB-SR-Devices AddOn**.
Die folgenden Diagnosebefehle werden per SSH auf der CCU ausgeführt.

## Installierte Version und Gerätebeschreibung

```sh
cat /usr/local/addons/hb-sr-devices-addon/VERSION
ls -l /firmware/rftypes/hb-sr-hy.xml
cat /usr/local/addons/hb-sr-devices-addon/firmware/rftypes/hb-sr-hy.xml
```

Der gemeinsame Installer legt unter `/firmware/rftypes/` einen Link auf die
Gerätebeschreibung im Addon-Verzeichnis an. Die XML-Version beschreibt den Stand
der Gerätebeschreibung; sie ist unabhängig von Addon- und Firmware-Version.

Aktueller Gerätetyp: `HB-SR-HY`, Modell `FE01`, HY-Funkadresse im Sketch `FE0102`.
Die Konfiguration enthält `rx_modes="ALWAYS"` sowie `ENABLE` und `FACTOR` auf
Kanal 1. Geräteebene und Kanal 2 haben leere MASTER-Parametersätze.

## Addon-Installationslogs

```sh
cat /usr/local/addons/hb-sr-devices-addon/log/inst.log
cat /usr/local/addons/hb-sr-devices-addon/log/inst.err
```

## Serielle Diagnose am HY

Den PlatformIO-Monitor mit 57600 Baud öffnen. Nach einer Faktoränderung sollte
`ConfigChanged CH1` den neuen Wert zeigen. Der reduzierte Stellwert wird erst
im nächsten regulären Link-B-Slot gesendet.

Bei einem kurzen Tastendruck erscheinen `pressed`, `released` und anschließend
die gesendete Geräteinformation. Die Antwort der CCU bestätigt zunächst nur
den Empfang; sie ist noch kein Nachweis für einen geschriebenen Faktor.

## Paket neu erstellen

Auf dem Entwicklungsrechner vom Repository-Hauptverzeichnis aus:

```sh
sh scripts/build-addon.sh
```

Das Ergebnis `dist/hb-sr-devices-addon.tgz` unter **Zusatzsoftware** installieren.
XML-Dateien müssen nicht manuell auf die CCU kopiert werden.
