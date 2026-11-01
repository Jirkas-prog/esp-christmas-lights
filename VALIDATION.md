# Ověření 0.6.1

Datum: 1. 11. 2026.

- Nativní testy MSVC: oba způsoby připojení výstupů, obě polarity, nesymetrická napětí LED, otevřený výstup, jednotlivé i společné zkraty, spodní omezení ADC, saturace a nepozorovatelný signál.
- Nezávislá simulace integruje proudy skutečného děliče do kondenzátoru. Zachovává jeho napětí přes změny PWM, přidává šum a ověřuje rychlé i pomalé RC během změn jasu, prolínání a střídání. Model testovaného pozorovatele negeneruje vstupní data tohoto testu.
- Ověřeny jsou také identifikace RC pro oba výstupy, validace nastavení a uložené kalibrace, časování poruchy, tepelný výpočet, 8181 kombinací PWM, efekty, síťová obnova a časový plán.
- `node --check web/app.js` a 4 testy `tests/device_api_guard.py` prošly. Zápisy vyžadují ověřenou identitu cílové desky.
- PlatformIO sestavení `tdisplay` (16 MB) i `esp32wroom32` (4 MB) prošlo. Prošlo i čisté veřejné sestavení bez místních přístupů. Obrazy se vejdou do obou OTA partition. Veřejné výchozí souřadnice zůstávají 0,0; časové testy používají Greenwich.

Na ESP32-D0WDQ6-V3 / 16 MB proběhla instalace a ověřené OTA přepnutí do druhé aplikační partition. Nová verze byla ověřena se skutečně prohozenými výstupy. Nastavení AOUT2 i úplná kalibrace přežily restart vyvolaný přes MQTT. Discovery selektoru, jeho šablony, odmítnutí neplatného příkazu a opakování stejné volby bez ztráty kalibrace prošly živým testem. Wi-Fi, MQTT, vypnutí záložního AP a webové soubory fungují; soubory odpovídají zdrojům po sjednocení konců řádků.

Automatická kalibrace dokončila všechny kroky. Živý test provedl 18 kombinací první/druhá/obě poloviny a jasů 2, 5, 10, 20, 50 a 100 %. Pasivní záznam GPIO při jednotlivých polovinách nezachytil vysokou úroveň opačného řídicího GPIO. Během 30 s opakovaných HTTP požadavků při konstantním svícení nepřibyly zápisy PWM, přepnutí můstku ani chyby počtu hran. Dále prošlo 36 kombinací efektů, jasů a intervalů bez restartu či vypnutí ochranou při automaticky určených hranicích.

**Omezení konkrétního osazení:** velmi silně vyhlazený signál dal automatické hranice 8 % / 7 % / 97 % pro první, druhou a obě poloviny. Při společném svitu to znamená možné téměř 4,9 W na rezistoru při zkratu bez zásahu ochrany; web a MQTT hlásí překročení předpokladu 2 W. Prolínání má odvozenou hranici mimo svůj rozsah, takže při tomto automatickém nastavení se napěťově nehlídá. Úspěšný test efektů proto není důkazem jejich plné ochrany. Při dočasném snížení hranice byly změřeny i společný svit a prolínání s aktivním vyhodnocováním; následně byly obnoveny automatické hranice. Krátká diagnostika neopravňuje k jejich trvalému snížení.

LED řetěz při těchto zkouškách nebyl připojený. Kalibrace otevřeného výstupu je přípustná, ale po připojení skutečného řetězu je potřeba nová kalibrace. Fyzický zkrat ani tepelná zkouška rezistoru nebyly provedeny. Numerické testy tyto zkoušky nenahrazují.
