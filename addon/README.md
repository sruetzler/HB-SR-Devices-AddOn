# Gemeinsames CCU-Addon

`src/` enthält die Installations- und Startskripte sowie die Update-Abfrage des
bisherigen HB-SR-Devices-AddOn-Projekts. Der installierte Name bleibt unverändert.
Die Update-Abfrage verwendet das neueste veröffentlichte GitHub-Release und
lädt dessen Asset `hb-sr-devices-addon.tgz` herunter. Release-Tags müssen der
Version in `VERSION` entsprechen, optional mit `v` davor (z. B. `v0.04`).
Entwürfe und Vorabversionen werden nicht angeboten. Fehlt das Paket oder
schlägt die Abfrage fehl, meldet die Versionsabfrage `n/a`.

Die bearbeitbare Versionsnummer steht ausschließlich in `VERSION`.
Gerätespezifische Dateien gehören nach `../devices/<Gerät>/ccu/`.

Bauen aus diesem Ordner:

```sh
sh ../scripts/build-addon.sh
```

Das Paket liegt anschließend unter `../dist/hb-sr-devices-addon.tgz`.
Die Firmware des Geräts wird durch die Addon-Installation nicht aktualisiert.
