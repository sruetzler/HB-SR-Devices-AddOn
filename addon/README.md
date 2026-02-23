# HB-SR-HY CCU3 Addon

Dieses Addon fügt Unterstützung für das HB-SR-HY Gerät (Hydraulic Balancing Thermostat) zur CCU3 hinzu.

## Installation

1. Erstelle ein tar.gz Archiv:
   ```bash
   cd addon
   tar -czf hb-sr-hy-addon.tar.gz *
   ```

2. Gehe zur CCU3 WebUI:
   - Einstellungen → Systemsteuerung → Zusatzsoftware
   - Wähle die erstellte .tar.gz Datei aus
   - Klicke auf "Installieren"

3. Nach der Installation wird das RFD automatisch neu gestartet

4. Das Gerät kann nun über die Anlernfunktion der CCU3 eingelernt werden

## Geräteparameter

Das Gerät verfügt über folgende Konfigurationsparameter:

- **ENABLE**: Aktiviert/Deaktiviert die Funktion (Boolean)
- **LEARN**: Lern-Modus aktivieren (Boolean)
- **NEWFACTOR**: Neuer Faktor (0-255)
- **ACTFACTOR**: Aktueller Faktor (0-255)

## Deinstallation

Die Deinstallation erfolgt über die CCU3 WebUI unter Zusatzsoftware.
