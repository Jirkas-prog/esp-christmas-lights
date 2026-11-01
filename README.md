# ESP Christmas Lights

Řízení dvouvodičového LED řetězu přes ESP32, lokální web a MQTT. Režimy, stmívání, časové plány a ochranné funkce.

## Sestavení a nastavení

PlatformIO: pio run -e tdisplay. Host testy: tools/test.ps1. Použijte správně dimenzovaný H-můstek a napájení; před provozem je nutná kalibrace. Firmware není náhradou elektrického jištění. Wi-Fi a MQTT se zadávají do webového nastavení.

Výchozí zeměpisná poloha `0,0` je pouze zástupná. Před zapnutím slunečního plánu nastavte vlastní polohu v zařízení; automatický plán je ve výchozím stavu vypnutý.

## Obsah repozitáře

Zdrojové kódy, konfigurace sestavení a dostupné testy. Soukromé přístupy, záznamy instalací, zálohy zařízení a dříve sestavené binární soubory sem nepatří. Příklady vyplňte vlastními údaji až místně.

Toto je zdrojový snímek uložený 1. 11. 2026. Stav konkrétních kontrol je uveden v `VALIDATION.md`; nahrání zdrojů neznamená nové ověření na fyzickém zařízení.

## Další projekty

[Přehled projektů Jirkas-prog](https://github.com/Jirkas-prog) · [Home Assistant doplňky](https://github.com/Jirkas-prog/home-assistant-addons)
