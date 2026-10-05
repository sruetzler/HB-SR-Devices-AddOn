# HY – Streifenrasterlayout Rev. 16

Rev. 16 ersetzt Rev. 15.

## Quarzbereich: Masse eine Zeile höher
- Der lokale GND-Bus liegt jetzt auf **Reihe L, Spalten 1 bis 15**.
- Damit enden die Masse-Drahtbrücken eine Rasterzeile weiter vom 8-MHz-Quarzgehäuse entfernt.

### Änderungen
- **J34:** `C11 → L11` statt `C11 → M11`.
- **J45:** `J12 → L12` statt `J12 → M12`.
- **J39 entfällt**. Der 20-pF-Kondensator C1 liegt mit `L15` direkt auf GND.
- **J46 neu:** `L3 → M3`. Damit wird der angehobene L-GND-Bus weit links vom Quarz mit dem Haupt-GND verbunden.
- Auf Reihe L bleibt nur der Schnitt **`L15–L16`**: links davon GND, rechts davon VCC.
- **J01 bleibt `A16 → L16`**; `L16…L20` ist weiterhin VCC bis ATmega Pin 7.

## Unverändert
- Quarz: `N10 / N14`.
- LED `K14 → K20 / Pin 6` bleibt durchgehend.
- VCC-100 nF: `J19 ↔ L19`.
- AVCC-100 nF: `L24 ↔ N24`.
- AREF-100 nF: `M26 ↔ M28`.
- Radio 3,3 V: `J06 A41 → I41`.
- Radio 10 µF: `H43 ↔ I43`.
- Radio 100 nF: `C49 ↔ E49`.
- M3: `C3 / C33 / X3 / X33`.

## Prüfstatus
- elektrische Netzprüfung: **OK**
- L-GND-Bus an Haupt-GND: **OK**
- L15/L16 GND-VCC-Trennung: **OK**
- VCC L16 → Pin 7: **OK**
- C1 L15 direkt GND: **OK**
- modellierte Bauteilkollisionen: **keine**
- modellierte Draht-/Bauteilkollisionen: **keine**
- überlappende Drahtbrücken: **keine**

## Drahtbrücken

| ID | Punkte | Funktion |
|---|---|---|
| J01 | A16 → L16 | MCU VCC → +3,3 V |
| J02 | M7 → R7 | MCU GND Pin 8 → GND |
| J03 | L29 → T29 | MCU GND Pin 22 → GND |
| J04 | A36 → N36 | AVCC → +3,3 V |
| J05 | A35 → V35 | MCP VOUT / +3,3-V-Bus |
| J06 | A41 → I41 | Radio +3,3 V lokal |
| J07 | H37 → T37 | Radio GND lokal |
| J08 | N5 → P5 | USB VBUS → +5 V |
| J36 | P19 → U19 | +5 V → MCP VIN |
| J09 | R3 → T3 | USB GND |
| J10 | H18 → V18 | ATmega TX/D1 → FT232 RXD |
| J11 | G8 → W8 | ATmega RX/D0 ← FT232 TXD |
| J12 | T12 → Z12 | FT232 GND |
| J13 | P25 → W25 | ISP MISO |
| J14 | O24 → X24 | ISP SCK |
| J15 | Q31 → X31 | ISP MOSI |
| J16 | F17 → Y17 | ISP RESET |
| J17 | V34 → W34 | ISP +3,3 V |
| J18 | T36 → Y36 | ISP GND |
| J19 | E12 → F12 | RESET-Taster → RESET |
| J34 | C11 → L11 | RESET-GND → GND-Bus L |
| J45 | J12 → L12 | VCC-100nF GND → GND-Bus L |
| J21 | Q5 → R5 | CONFIG-GND |
| J22 | N9 → O9 | Quartz → XTAL2 |
| J23 | E13 → I13 | D2 |
| J24 | B38 → Q38 | CC1101 SI → MOSI |
| J25 | C39 → O39 | CC1101 SCLK → SCK |
| J26 | D40 → P40 | CC1101 SO → MISO |
| J27 | E28 → F28 | CC1101 GDO0 → D2 |
| J28 | G32 → R32 | CC1101 CSn → D10 |
| J29 | A45 → B45 | CC1101 VCC |
| J30 | E45 → H45 | CC1101 GND9 |
| J31 | F46 → H46 | CC1101 GND8 |
| J32 | G47 → H47 | CC1101 GND7 |
| J40 | L31 → M31 | AREF 100nF GND |
| J37 | B47 → C47 | CC1101 100nF VCC |
| J38 | U37 → V37 | MCP Cout VOUT |
| J46 | L3 → M3 | GND-Bus L → Haupt-GND |

## Kupferschnitte

- `B35–B36`
- `B43–B44`
- `C2–C3`
- `C3–C4`
- `C32–C33`
- `C33–C34`
- `C35–C36`
- `C43–C44`
- `C50–C51`
- `D35–D36`
- `D43–D44`
- `D50–D51`
- `E12–E13`
- `E40–E41`
- `E43–E44`
- `F21–F22`
- `F25–F26`
- `F43–F44`
- `G21–G22`
- `G25–G26`
- `G43–G44`
- `H21–H22`
- `H24–H25`
- `I21–I22`
- `I24–I25`
- `J19–J20`
- `J21–J22`
- `K13–K14`
- `K21–K22`
- `L15–L16`
- `L21–L22`
- `M21–M22`
- `M26–M27`
- `N7–N8`
- `N12–N13`
- `N21–N22`
- `O3–O4`
- `O21–O22`
- `P4–P5`
- `P19–P20`
- `P21–P22`
- `Q4–Q5`
- `Q18–Q19`
- `Q21–Q22`
- `R13–R14`
- `R21–R22`
- `S21–S22`
- `U10–U11`
- `U11–U12`
- `U36–U37`
- `V18–V19`
- `W16–W17`
- `W28–W29`
- `X2–X3`
- `X3–X4`
- `X10–X11`
- `X11–X12`
- `X28–X29`
- `X32–X33`
- `X33–X34`
- `Y10–Y11`
- `Y11–Y12`
- `Y28–Y29`
- `Z13–Z14`