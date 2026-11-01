# Ověření 0.6.0

Datum: 1. 11. 2026.

- MSVC / `tools/test.ps1`: nativní testy prošly. Nezávislá simulace integruje proudy dvou rezistorů do kondenzátoru, včetně spodního omezení ADC. Ověřuje bod A, obě polarity, nesymetrické napětí LED, otevřený výstup, nízké střídy, jednotlivé i společné zkraty a nulovou změnu signálu opačné polarity proti OFF.
- Ověřeny jsou také RC identifikace, saturace a výpadky měření, limity kvality signálu, časování poruchy, tepelný výpočet, 8181 kombinací PWM, efekty, síťová obnova a časový plán.
- `node --check web/app.js`: prošlo jako součást `tools/test.ps1`.
- `tests/device_api_guard.py`: 4 testy prošly. Bez cílového ID nebo při jeho neshodě se neodesílá žádný zápis.
- PlatformIO: úplné sestavení `tdisplay` (16 MB) i `esp32wroom32` (4 MB) prošlo, včetně čistého veřejného sestavení bez místních přístupů. Obrazy se vejdou do obou OTA partition.
- Veřejné výchozí souřadnice zůstávají 0,0; časové testy používají Greenwich.

Instalace této verze, kalibrace a živé testy na konkrétní desce zatím nejsou ověřené. Skript `tests/single_point_device.py` je připraven pro výslovně vybraný cíl; není součástí automatických offline testů. Fyzický zkrat ani tepelná zkouška zalitého rezistoru nebyly provedeny. Numerické testy tyto zkoušky nenahrazují.
