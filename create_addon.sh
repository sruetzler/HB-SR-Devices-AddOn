#!/bin/bash
# Script zum Erstellen des CCU3-Addon Pakets

cd "$(dirname "$0")/addon"
tar -czf ../hb-sr-hy-addon.tar.gz *
cd ..

echo "Addon-Paket wurde erstellt: hb-sr-hy-addon.tar.gz"
echo ""
echo "Installation auf der CCU3:"
echo "1. Gehe zu: Einstellungen → Systemsteuerung → Zusatzsoftware"
echo "2. Wähle die Datei hb-sr-hy-addon.tar.gz aus"
echo "3. Klicke auf 'Installieren'"
