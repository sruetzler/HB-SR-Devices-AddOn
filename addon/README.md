# Gemeinsames CCU-Addon

`src/` enthält die Installations- und Startskripte sowie die Update-Abfrage des
bisherigen HB-SR-Devices-AddOn-Projekts. Der installierte Name und die
GitHub-Update-Adressen bleiben unverändert.

Die bearbeitbare Versionsnummer steht ausschließlich in `VERSION`.
Gerätespezifische Dateien gehören nach `../devices/<Gerät>/ccu/`.

Bauen aus diesem Ordner:

```sh
sh ../scripts/build-addon.sh
```

Das Paket liegt anschließend unter `../dist/hb-sr-devices-addon.tgz`.
Die Firmware des Geräts wird durch die Addon-Installation nicht aktualisiert.
