# CCU3 Debug-Anleitung für HB-SR-HY

## 1. SSH-Verbindung zur CCU3 herstellen

```bash
ssh root@<ccu-ip>
```

## 2. Überprüfen, ob die XML-Datei installiert ist

```bash
ls -la /usr/local/etc/config/firmware/rftypes/ | grep hb-sr-hy
```

Erwartete Ausgabe: `-rw-r--r-- ... hb-sr-hy.xml`

## 3. XML-Datei anzeigen

```bash
cat /usr/local/etc/config/firmware/rftypes/hb-sr-hy.xml
```

## 4. RFD Log in Echtzeit beobachten

```bash
tail -f /var/log/rfd.log
```

Dann Anlernen-Button am Gerät drücken und Log beobachten.

Suche nach:
- Zeilen mit `FE01` (Device Model)
- Zeilen mit `019901` (Device ID)
- Fehlermeldungen wie "Unknown device type"

## 5. RFD neu starten

```bash
/etc/init.d/S61rfd restart
```

## 6. Alle bekannten Gerätetypen auflisten

```bash
cd /usr/local/etc/config/firmware/rftypes/
ls -1 *.xml | head -20
```

## 7. Test: Manuelles Hinzufügen der Gerätedefinition

Falls die XML nicht installiert wurde, manuell kopieren:

```bash
# Auf deinem PC das Addon entpacken und XML per SCP kopieren:
scp addon/firmware/rftypes/hb-sr-hy.xml root@<ccu-ip>:/usr/local/etc/config/firmware/rftypes/

# Dann auf der CCU:
chmod 644 /usr/local/etc/config/firmware/rftypes/hb-sr-hy.xml
/etc/init.d/S61rfd restart
```

## 8. Weitere wichtige Logs

```bash
# System-Log
cat /var/log/messages | grep -i hb-sr

# ReGaHss Log (falls vorhanden)
cat /var/log/rega.log | tail -50
```

## 9. Bekannte Gerätetypen im RFD prüfen

```bash
# BidCos-RF Dienst Status
/etc/init.d/S61rfd status
```

## Typische Fehler und Lösungen

### Fehler: "Unknown device type"
- XML-Datei fehlt oder wurde nicht geladen
- RFD muss neu gestartet werden
- Device Model in XML stimmt nicht mit Firmware überein

### Fehler: Gerät wird erkannt aber nicht angelernt
- AES-Schlüssel prüfen
- Signalstärke/RSSI zu schwach
- Gerät bereits in CCU vorhanden (vorher löschen)

### Fehler: Addon wird nicht installiert
- Prüfe ob Addon-Format korrekt ist (tar.gz mit richtiger Struktur)
- Prüfe Schreibrechte auf CCU
