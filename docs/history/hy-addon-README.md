# HB-SR-HY CCU3 Addon

Dieses Addon fügt Unterstützung für das HB-SR-HY Gerät (Hydraulic Balancing Thermostat) zur CCU3 hinzu.

## Installation

1. Erstelle im Projektverzeichnis das Addon-Archiv:
   ```bash
   bash create_addon.sh
   ```
   Das Paket liegt anschließend als `hb-sr-hy-addon.tar.gz` im Projektverzeichnis.

2. Gehe zur CCU3 WebUI:
   - Einstellungen → Systemsteuerung → Zusatzsoftware
   - Wähle die erstellte .tar.gz Datei aus
   - Klicke auf "Installieren"

3. Nach der Installation wird das RFD automatisch neu gestartet

4. Das Gerät kann nun über die Anlernfunktion der CCU3 eingelernt werden

## Geräteparameter

Die hydraulische Anpassung wird auf **Kanal 1** (Verbindung zum originalen TC) konfiguriert:

- **ENABLE**: Aktiviert die hydraulische Anpassung (Boolean, Standard: `true`). Bei `false` arbeitet HY mit einem effektiven Faktor von 100 %; die Funkverbindungen bleiben aktiv.
- **FACTOR**: Anteil des normalen TC-Stellwerts, der an den VD weitergegeben wird (0–100 %, Standard: `100`). `100` bedeutet keine Reduktion, `80` bedeutet 20 % Reduktion, `0` bedeutet einen normalen Stellwert von 0 %.

Beispiel: Bei `FACTOR=80` werden aus 50 % TC-Stellwert 40 % VD-Stellwert. Die tatsächliche VD-Position wird für die Rückmeldung an den TC umgekehrt skaliert und auf 100 % begrenzt. Bei Faktor 0 meldet HY im Normalbetrieb mit vorhandenem VD-Status 100 % an den TC zurück, da eine Rückrechnung durch Division durch 0 nicht möglich ist.

Sonderbefehle wie explizites Öffnen/Schließen und Entkalkung werden nicht mit dem Faktor skaliert. Änderungen an `ENABLE` oder `FACTOR` werden beim nächsten regulären VD-Sendezyklus berücksichtigt.

Kanal 2 stellt die Verbindung zum originalen VD bereit. Ein Lernmodus und die früher dokumentierten Parameter `LEARN`, `NEWFACTOR` und `ACTFACTOR` sind in der aktuellen Firmware nicht implementiert.

Ab Addon-Version 1.0.4 hat Kanal 2 keine eigenen MASTER-Konfigurationsparameter
mehr. Die ungenutzte `GLOBAL_BUTTON_LOCK` und die doppelte `LOWBAT_LIMIT` wurden
entfernt. Ab Version 1.0.5 entfällt `LOWBAT_LIMIT` auch auf Geräteebene, da HY
netzversorgt ist. Für diese Bereinigung ist nur ein Addon-Update erforderlich.

Ab Version 1.0.6 entfallen auch `INTERNAL_KEYS_VISIBLE` und
`LOCAL_RESET_DISABLE` ("Lock reset via device button"). HY besitzt keine
internen Tastenkanäle; die Resetsperre war in der Firmware nicht implementiert.
Damit bleiben als MASTER-Konfigurationsparameter nur `ENABLE` und `FACTOR`
auf Kanal 1. Auch hierfür genügt ein Addon-Update; die Funktion des
Konfigurationstasters bleibt unverändert.

Der neue XML-Standardwert ändert keine bereits gespeicherten Faktoren im Gerät. Bei bestehenden Installationen den gewünschten Wert auf Kanal 1 prüfen.

## Konfiguration übertragen

HY ist netzversorgt und dauerhaft empfangsbereit. Ab Addon-Version 1.0.3 ist
das auch in der Gerätebeschreibung als `rx_modes="ALWAYS"` hinterlegt. Die CCU
kann Konfigurationsänderungen damit direkt übertragen; ein Tastendruck am HY
ist dafür nicht erforderlich. Die Firmware berücksichtigt für Antworten auf
CCU-Konfigurationsnachrichten ausdrücklich die normale AskSin++-Verzögerung
von 100 ms, unabhängig vom gesonderten TC-/VD-Timing.

Für diese Korrektur müssen sowohl die Firmware als auch das Addon aktualisiert
werden. Das Addon-Paket allein aktualisiert die HY-Firmware nicht.

## Deinstallation

Die Deinstallation erfolgt über die CCU3 WebUI unter Zusatzsoftware.
