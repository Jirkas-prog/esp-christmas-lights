# Validation record

## English/Czech interface — 0.6.2

Date: 2026-11-03.

- Eight Chromium scenarios passed with Playwright 1.58.2: every page in both languages, drafts and file selection, literal names and raw examples, unchanged command payloads, live calibration messages, rejected commands and OTA errors, saved preferences, blocked storage and mobile layout. All device requests used a local mock.
- The original native MSVC suite passed, including the independent RC simulations and 8,181 PWM combinations. Four device-identity guard tests and two embedded-asset / HTTP-capacity checks passed. Both JavaScript files passed syntax checks.
- Both PlatformIO profiles built successfully. RAM use is 67,564 / 327,680 bytes. Reported flash use is 1,103,381 / 1,966,080 bytes for `tdisplay` and 1,103,341 / 1,966,080 bytes for `esp32wroom32`.
- Local packages passed the existing flash-header, version, partition and size checks for 16 MB and 4 MB boards. Both retain two OTA slots. No local bootstrap credentials were included.
- The firmware change adds the language asset, its HTTP route and sufficient route capacity; control algorithms, command parsers, persistent settings and calibration formats are unchanged. Browser language selection issues no device command.
- This update was not flashed to a physical board. OTA failure testing used a mock endpoint; it does not establish physical OTA or thermal behaviour. The historical hardware limitations below still apply.

## Historical hardware and model checks — 0.6.1

Date: 2026-11-01. These measurements describe 0.6.1 and have not been repeated on physical hardware for the language update.

- Native MSVC tests covered both output wirings and polarities, asymmetric LED voltages, open output, individual and combined shorts, the ADC lower limit, saturation and unobservable signals.
- An independent simulation integrates actual divider currents into a capacitor. It preserves capacitor voltage across PWM changes, adds noise and checks fast and slow RC circuits during brightness changes, crossfades and alternating effects. The observer being tested does not generate this test's input data.
- Tests also covered RC identification for both outputs, validation of settings and saved calibration, fault timing, thermal calculations, 8,181 PWM combinations, effects, network recovery and scheduling.
- `node --check web/app.js` and four `tests/device_api_guard.py` tests passed. Writes require a verified target-board identity.
- Both PlatformIO profiles, `tdisplay` (16 MB) and `esp32wroom32` (4 MB), built successfully. A clean public build without local credentials also passed. Images fit both OTA partitions. Public default coordinates remain 0,0; time tests use Greenwich.

An ESP32-D0WDQ6-V3 / 16 MB received an installation and a verified OTA switch to the second application partition. The new version was checked with physically swapped outputs. AOUT2 settings and complete calibration survived an MQTT-triggered restart. Live tests passed for selector discovery and its templates, rejection of an invalid command, and repetition of the same selection without losing calibration. Wi-Fi, MQTT, fallback-AP shutdown and web assets worked; web assets matched the sources after line-ending normalization.

Automatic calibration completed every step. A live test exercised 18 combinations of first half, second half and both halves at 2, 5, 10, 20, 50 and 100% brightness. Passive GPIO capture during single-half operation detected no high level on the opposite control GPIO. During 30 seconds of repeated HTTP requests at constant lighting, there were no additional PWM writes, bridge switches or edge-count errors. Another 36 combinations of effects, brightness and intervals passed without restarts or protection shutdowns at the automatically selected limits.

**Limitation of this hardware setup:** a heavily smoothed signal produced automatic limits of 8% / 7% / 97% for the first half, second half and both halves. With both halves lit, a short circuit could therefore dissipate almost 4.9 W in the resistor without protection intervening; the web interface and MQTT report that the 2 W assumption is exceeded. Crossfade's derived limit lies outside its operating range, so voltage protection does not monitor crossfades at these automatic settings. Successful effect tests are therefore not evidence of complete protection. Both-half operation and crossfades were also measured with active evaluation after temporarily reducing the limit; automatic limits were restored afterwards. This short diagnostic does not justify permanently reducing them.

No LED string was connected during these tests. Open-output calibration is permitted, but a real string requires a new calibration after connection. No physical short circuit or resistor thermal test was performed. Numerical tests do not replace those tests.
