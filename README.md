# ESP Christmas Lights · ledpasek02

Firmware 0.6.2 for an ESP32, a DRV8833 bridge and a two-wire LED string with opposite polarities. It provides an English/Czech web interface, MQTT / Home Assistant integration, solar scheduling and passive short-circuit detection. The default alias is `ledpasek02`; changing it also changes DHCP, mDNS, fallback Wi-Fi and MQTT naming.

## English and Czech

Choose **EN** or **CZ** in the header. The first visit follows the browser language (Czech for `cs`, otherwise English), and the choice is remembered in that browser. Switching preserves unsaved settings, selected files and open sections. It sends no command to the board. Device aliases, network names, MQTT topics, input values and raw JSON examples remain literal. Both languages cover protection, calibration, diagnostics, scheduling and update messages.

The language preference belongs to the browser. Firmware settings, MQTT codes, Home Assistant discovery identities and stored calibration are unchanged. Home Assistant retains its existing bilingual effect names.

## Wiring

Supply the bridge with **5 V** and connect a real common ground between ESP32 and DRV8833. Do not power the string through the ESP32 3.3 V regulator. GPIO37 is input-only and is not driven by the firmware; any connection to GND must be physical.

| ESP32 | DRV8833 / connection |
|---|---|
| GPIO25 | AIN1 |
| GPIO26 | AIN2 |
| GPIO27 | nSLEEP / SLEEP / EEP |
| GND | GND |
| GPIO32 / ADC1_CH4 | Sense-divider midpoint |

```text
DRV8833 AOUT1 ── 5.6 Ω / 5 W ── A ── LED string ── AOUT2
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

The diagram places the resistor on AOUT1. **AOUT1 and AOUT2 may be swapped.** Select the bridge output connected to the 5.6 Ω resistor in the web interface. This choice is saved and is also available through MQTT / Home Assistant. Changing it turns off the light and invalidates calibration; run calibration again. Calibration does not change the selection automatically because reversed wiring and a short circuit can produce similar signals.

The recommended ceramic capacitor C is 100 nF. Calibration measures the actual RC time constant and checks signal quality; an arbitrary capacitance may not produce a measurable waveform. Both 3 kΩ resistors and the 5 V supply are part of the model. Sensing connects **after the power resistor**. Firmware enables the weak internal ADC pull-up and includes its effect in the calculation.

GPIO32 belongs to ADC1, so it can measure while Wi-Fi is active. References: [Espressif ADC](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/peripherals/adc.html) and [TI DRV8833 pinout and input table](https://www.ti.com/lit/ds/symlink/drv8833.pdf).

## Protection and calibration

Open **Light → Short-circuit protection → Start automatic calibration**. Calibration measures OFF, RC response and 1–100% brightness for the first half, second half and full string, including the shortest crossfade pulses. It takes several minutes; weaker signals are sampled longer. If it fails, the previous valid calibration remains saved.

- With positive supply through the resistor, a short circuit pulls point A toward ground.
- With opposite polarity, a short circuit pulls point A toward the supply. The AOUT1/AOUT2 setting determines which half corresponds to each direction.
- Measurement reads the phase of actual PWM and compares both parts of the waveform with calibration. Averaging both polarities alone cannot reliably distinguish a short circuit.
- For slower RC circuits, each short measurement separates the remaining capacitor response from the previous voltage. Crossfades need not wait for settling. Calibration adapts its settling time and accuracy assessment to the measured time constant; signals that remain too weak are rejected.
- An 80% setting means a lower limit of 80% of calibrated LED voltage. Opposite polarity uses the difference from supply voltage, so its ADC threshold is an upper limit. Excessive deviation in the opposite direction and invalid measurements are also monitored.
- Calibration permits near-zero current and an open output. Recalibrate after replacing the string to capture its actual voltage characteristics.
- Protection injects no test pulses and does not light an inactive half. With protection enabled, output is blocked until the first valid calibration.
- Fault confirmation time is adjustable from 0.1–10 s. The default response turns the light off; a new manual ON permits another attempt. Persistent latching is optional.
- Signal quality determines the ignored low-brightness limits for each half and the full string. Manual overrides and restoration of automatic limits are available through the web interface and MQTT.

**Thermal assumption: 2 W in the resistor.** During a short circuit, `P = U²/R × (dutyA+dutyB)/1024`. With conservative values of 5.25 V and 5.32 Ω, the maximum is 39% for the full string or 79% for one half. Protection is not automatically disabled below these brightness values: the actual limit depends on measurement quality. Settings above this thermal assumption are permitted, but the web interface and MQTT show a prominent warning. The actual temperature of the encapsulated resistor is not measured; software does not replace a thermal fuse.

ADC accuracy is limited near the upper end of its range. Calibration checks saturation, repeatability and whether a healthy signal can be distinguished from a short circuit. A numerical test is neither a temperature measurement nor confirmation of a particular board's behaviour.

## Control and networking

Open `http://ledpasek02.local/` or the IP address assigned by the router. The web interface and OTA have no password. An open fallback AP starts after loss of the main Wi-Fi network and stops when the connection recovers; its address is `http://192.168.4.1/`. Use a trusted local network.

Effects include the full string, either half, alternating, linear crossfade, soft crossfade, breathing, tide and candlelight. Brightness and cycle duration are adjustable. A separate preset determines brightness and effect for an ordinary ON command. **1% = OFF** switches off both the reported state and output; increasing brightness turns the light back on, and ON restores the preset.

The board obtains time from the internet and calculates sunrise/sunset from a time zone and coordinates, with configurable offsets. Public default coordinates are 0,0: configure your own location. Evening, night and morning windows have separate brightness and effects. Automation can be disabled independently of manual control; a manual change takes priority until the next schedule boundary.

MQTT base: `wifi2mqtt/ledpasek02`. Home Assistant discovery creates the light and settings. Retained configuration is in `/config`, state in `/state`, calibration in `/calibration`, light state in `/light/state` and availability in `/availability`.

| Command topic under the base | Purpose / example |
|---|---|
| `/set` | `{"state":"ON","brightness":35,"effect":"all"}` |
| `/preset/set` | Default ON brightness and effect |
| `/schedule/set` | Automation, location, time zone, times and offsets |
| `/schedule/window/0/set`, `/schedule/window/1/set` | Schedule windows |
| `/schedule/presets/set` | Evening and morning presets |
| `/schedule/night/set` | Night brightness and effect |
| `/schedule/action` | `{"action":"resume"}` — resume automation |
| `/protection/set` | `{"enabled":true,"threshold_percent":80,"delay_ms":500,"latch_fault":false}` |
| `/protection/polarity/set` | `{"output":"AOUT1"}` or `{"output":"AOUT2"}` — resistor wiring; changing it requires recalibration |
| `/protection/limits/set` | `{"first":5,"second":5,"all":5}` or `{"automatic":true}` |
| `/protection/action` | `{"action":"calibrate"}`, `{"action":"cancel"}`, `{"action":"clear"}` |
| `/network/set` | Network configuration |
| `/alias/set` | `{"alias":"ledpasek02"}` |
| `/system/action` | `{"action":"restart"}` |
| `/get` | `{}` — request current state |

Send commands without retain. Responses appear in `/result`. The web interface uses the same command validation as MQTT.

## Build, install and verify

PlatformIO, Arduino ESP32 2.0.17 / ESP-IDF 4.4.7. Profiles: `tdisplay` (LILYGO T-Display, 16 MB, display unused) and `esp32wroom32` (ESP32-WROOM-32 / DevKit, 4 MB). The pinout is identical; select the profile matching the board's actual flash capacity.

```powershell
pio run -e tdisplay -e esp32wroom32
./tools/test.ps1
python tests/device_api_guard.py
python tests/language_assets.py
npm --prefix tests/browser ci
npm --prefix tests/browser exec -- playwright install chromium --only-shell
npm --prefix tests/browser test
python tools/package.py -e tdisplay
```

Browser tests use a local mock device. They check both languages, drafts, file selection, literal user text, protocol payloads, calibration messages, persistence and mobile layout. They do not connect to a physical board.

The first USB installation uses `factory.bin` at address 0x0. Web OTA uses only `firmware.bin`; two application partitions permit normal updates without changing saved settings. No device flash backup is created. Builds made with a local `.local/bootstrap.h` may contain credentials and must not be shared.

For writes, `tools/device_api.py` requires `LIGHT_HOST` and `LIGHT_DEVICE_ID` matching the verified target board. The upload helper checks identity and flash size. Reading state does not require an identifier.

Hardware LEDC generates PWM at 1 kHz. A dedicated task owns effect updates, separately from HTTP, MQTT, ADC and settings writes. Diagnostics count real PWM edges. Native tests cover the single-divider physical model, both output wirings, fast and slow RC responses including transients during brightness changes and crossfades, open output, individual and combined shorts, low brightness, protection timing, thermal calculations, effects and scheduling. Live tests are run separately on an explicitly selected board.

The live check `python tests/single_point_device.py --calibrate` calibrates and changes brightness on a previously verified board, then leaves the output off. It requires `LIGHT_HOST` and `LIGHT_DEVICE_ID`. It creates no physical short circuit.

See [VALIDATION.md](VALIDATION.md) for recorded measurements and hardware limitations.
