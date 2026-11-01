# ESP Christmas Lights · ledpasek02

Firmware 0.6.1 pro ESP32, DRV8833 a dvouvodičový LED řetěz se dvěma opačnými polaritami. Český web, MQTT / Home Assistant, automatika podle slunce a pasivní ochrana proti zkratu. Výchozí alias je `ledpasek02`; mění se na webu a používá se také pro DHCP, mDNS, záložní AP a MQTT.

## Zapojení

Napájení můstku **5 V**, společná skutečná zem ESP32 a DRV8833. Napájení řetězu neveďte přes 3,3V regulátor ESP32. GPIO37 je vstupní pin; firmware ho neřídí. Případný spoj na GND musí být fyzický.

| ESP32 | DRV8833 / připojení |
|---|---|
| GPIO25 | AIN1 |
| GPIO26 | AIN2 |
| GPIO27 | nSLEEP / SLEEP / EEP |
| GND | GND |
| GPIO32 / ADC1_CH4 | Střed snímacího děliče |

```text
DRV8833 AOUT1 ── 5,6 Ω / 5 W ── A ── LED řetěz ── AOUT2
                                │
                               3 kΩ
                                │
                                ├──────── GPIO32
                                │
                             ┌──┴──┐
                            3 kΩ   C
                             │     │
                             └──┬──┘
                               GND
```

Schéma ukazuje rezistor na AOUT1. **Výstupy AOUT1 a AOUT2 lze prohodit.** Ve webu zvolte výstup můstku, ke kterému je připojen rezistor 5,6 Ω. Volba se ukládá a je dostupná i v MQTT / Home Assistant. Její změna vypne světlo a zneplatní kalibraci; poté spusťte novou. Kalibrace tuto volbu sama nepřepíná, protože obrácené zapojení a zkrat mohou mít podobný signál.

Doporučený keramický kondenzátor C je 100 nF. Kalibrace změří časovou konstantu skutečného RC obvodu a ověří kvalitu signálu; libovolná kapacita nemusí dát měřitelný průběh. Hodnoty obou odporů 3 kΩ a napájení 5 V jsou součástí měřicího modelu. Snímání je připojené **za výkonovým rezistorem**. Firmware používá slabý interní pull-up ADC a zahrnuje jeho vliv do výpočtu.

GPIO32 patří do ADC1, takže může měřit i při aktivní Wi-Fi. [Espressif ADC](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/peripherals/adc.html). Pinout a vstupní tabulka můstku: [TI DRV8833](https://www.ti.com/lit/ds/symlink/drv8833.pdf).

## Ochrana a kalibrace

Na webu **Světlo → Ochrana proti zkratu → Automatická kalibrace**. Kalibrace změří OFF, odezvu RC obvodu a rozsah 1–100 % pro první polovinu, druhou polovinu i celý řetěz. Měří i nejkratší pulzy používané při prolínání. Trvá několik minut; slabé signály se měří déle. Při neúspěchu zůstane předchozí platná kalibrace zachovaná.

- Při kladném napájení přes rezistor napětí bodu A při zkratu klesá k zemi.
- Při opačné polaritě napětí bodu A při zkratu stoupá k napájení. Nastavení AOUT1/AOUT2 určuje, které polovině odpovídá každý směr.
- Měření čte fázi skutečného PWM a porovnává obě části průběhu s jejich kalibrací. Pouhý průměr obou polarit zkrat spolehlivě nerozliší.
- U pomalejšího RC obvodu se z každého krátkého měření oddělí doznívání předchozího napětí kondenzátoru. Prolínání tak nemusí čekat na ustálení. Kalibrace čekání a posouzení přesnosti přizpůsobí změřené časové konstantě; příliš slabý signál stále odmítne.
- Hodnota 80 % znamená spodní mez 80 % kalibrovaného napětí na LED. Pro opačnou polaritu se používá rozdíl vůči napájení; mez ADC je tedy horní. Hlídá se i odchylka opačným směrem a neplatné měření.
- Kalibrace dovoluje téměř nulový odběr i otevřený výstup. Po změně řetězu proveďte novou kalibraci pro jeho skutečnou napěťovou charakteristiku.
- Ochrana nevkládá testovací pulzy a nerozsvěcí vypnutou polovinu. Před první platnou kalibrací je výstup při zapnuté ochraně blokovaný.
- Doba potvrzení poruchy je nastavitelná 0,1–10 s. Výchozí reakce pouze vypne světlo; nové ruční ON dovolí další pokus. Trvalé blokování je volitelné.
- Kalibrace z kvality signálu určí hranice ignorovaných nízkých jasů pro jednotlivé poloviny a celý řetěz. Ruční úprava i návrat k automatice jsou dostupné ve webu a MQTT.

**Tepelný předpoklad: 2 W v rezistoru.** Při zkratu se počítá `P = U²/R × (dutyA+dutyB)/1024`. Pro konzervativní 5,25 V a 5,32 Ω vychází nejvýše 39 % pro celý řetěz nebo 79 % pro jedinou polovinu. Nejde o automatické vypnutí ochrany pod těmito jasy: skutečnou mez určuje kvalita měření. Nastavení nad tímto tepelným předpokladem je povolené, ale web i MQTT hlásí výrazné varování. Skutečná teplota zalitého rezistoru se neměří; software nenahrazuje tepelnou pojistku.

ADC má omezenou přesnost u horního konce rozsahu. Kalibrace kontroluje saturaci, opakovatelnost a rozlišení zdravého signálu od zkratu. Výsledek numerického testu není měřením teploty ani potvrzením chování konkrétní desky.

## Ovládání a síť

Web: `http://ledpasek02.local/`, případně IP přidělená routerem. Web a OTA nemají heslo. Záložní otevřené AP se zapne po výpadku hlavní Wi-Fi a po obnovení připojení se vypne; adresa AP je `http://192.168.4.1/`. Používejte důvěryhodnou lokální síť.

Režimy mají anglický i český název: celý řetěz, jednotlivé poloviny, střídání, prolínání, měkké prolínání, dýchání, příliv a světlo svíčky. Jas a délka cyklu jsou nastavitelné. Samostatná předvolba určuje jas a režim obyčejného ON. Volba **1 % = OFF** skutečně vypne stav i výstup; zvýšení jasu znovu zapne a ON obnoví předvolbu.

Deska získává čas přes internet, počítá východ/západ z časového pásma a zeměpisných souřadnic a podporuje časové posuny. Veřejné výchozí souřadnice jsou 0,0: nastavte vlastní místo. Večerní, noční i ranní interval mají samostatný jas a režim. Automatiku lze vypnout nezávisle na ručním ovládání; ruční změna má přednost do další hranice plánu.

MQTT základ: `wifi2mqtt/ledpasek02`. Home Assistant discovery vytvoří světlo a nastavení. Konfigurace je v retained `/config`, stav v `/state`, kalibrace v `/calibration`, stav světla v `/light/state` a dostupnost v `/availability`.

| Příkazové téma pod základem | Účel / příklad |
|---|---|
| `/set` | `{"state":"ON","brightness":35,"effect":"all"}` |
| `/preset/set` | Výchozí jas a režim ON |
| `/schedule/set` | Automatika, poloha, pásmo, časy a posuny |
| `/schedule/window/0/set`, `/schedule/window/1/set` | Časová okna |
| `/schedule/presets/set` | Večerní a ranní předvolba |
| `/schedule/night/set` | Noční jas a režim |
| `/schedule/action` | `{"action":"resume"}` — vrácení řízení automatice |
| `/protection/set` | `{"enabled":true,"threshold_percent":80,"delay_ms":500,"latch_fault":false}` |
| `/protection/polarity/set` | `{"output":"AOUT1"}` nebo `{"output":"AOUT2"}` — zapojení rezistoru; změna vyžaduje novou kalibraci |
| `/protection/limits/set` | `{"first":5,"second":5,"all":5}` nebo `{"automatic":true}` |
| `/protection/action` | `{"action":"calibrate"}`, `{"action":"cancel"}`, `{"action":"clear"}` |
| `/network/set` | Síťová konfigurace |
| `/alias/set` | `{"alias":"ledpasek02"}` |
| `/system/action` | `{"action":"restart"}` |
| `/get` | `{}` — vyžádání aktuálního stavu |

Příkazy posílejte bez retain. Odpovědi jsou v `/result`. Web používá stejnou validaci příkazů jako MQTT.

## Build, instalace a ověření

PlatformIO, Arduino ESP32 2.0.17 / ESP-IDF 4.4.7. Profily: `tdisplay` (LILYGO T-Display, 16 MB, bez displeje) a `esp32wroom32` (ESP32-WROOM-32 / DevKit, 4 MB). Shodný pinout; vyberte profil podle skutečné flash desky.

```powershell
pio run -e tdisplay -e esp32wroom32
./tools/test.ps1
python tests/device_api_guard.py
python tools/package.py -e tdisplay
```

První USB instalace používá `factory.bin` od adresy 0x0. Webové OTA používá pouze `firmware.bin`; dvě aplikační partition umožňují běžnou aktualizaci bez změny uložených nastavení. Nevytváří se záloha flash zařízení. Balíčky sestavené s místním `.local/bootstrap.h` mohou obsahovat přístupy a nesmějí se sdílet.

`tools/device_api.py` vyžaduje pro zápisy nastavit `LIGHT_HOST` a `LIGHT_DEVICE_ID` podle ověřené cílové desky. Nahrávací skript kontroluje identitu i velikost flash. Čtení stavu nevyžaduje identifikátor.

PWM generuje hardwarové LEDC na 1 kHz. Aktualizaci efektů vlastní samostatná úloha oddělená od HTTP, MQTT, ADC a zápisu nastavení. Diagnostika čítá skutečné hrany PWM. Nativní testy ověřují fyzikální model jednoho děliče, obě zapojení výstupů, rychlé i pomalé RC včetně doznívání při změnách jasu a prolínání, otevřený výstup, jednotlivé i společné zkraty, nízký jas, časování ochrany, tepelný výpočet, efekty a plánovač. Živé testy se spouštějí samostatně na výslovně vybrané desce.

Živá kontrola `python tests/single_point_device.py --calibrate` provede kalibraci a změny jasu na předem ověřené desce; po skončení ponechá výstup vypnutý. Vyžaduje `LIGHT_HOST` a `LIGHT_DEVICE_ID`. Žádný fyzický zkrat nevytváří.

Naměřené výsledky a omezení konkrétního osazení jsou v [VALIDATION.md](VALIDATION.md).
