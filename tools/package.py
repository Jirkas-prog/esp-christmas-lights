"""Package verified ESP32 build locally. Credentials may be embedded: do not share."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('-e', '--environment', choices=('tdisplay', 'esp32wroom32'), default='tdisplay')
args = parser.parse_args()
config = configparser.ConfigParser()
config.read(root/'platformio.ini', encoding='utf-8')
profile = config[f'env:{args.environment}']
flash_size = profile['board_upload.flash_size']
label = ('Původní LILYGO T-Display, ESP32-D0WDQ6, bez obsluhy displeje'
         if args.environment == 'tdisplay' else 'ESP32-WROOM-32 / ESP32 DevKit, bez displeje')
build = root/'.pio/build'/args.environment
version = re.search(r'Version\[\] = "([0-9.]+)"', (root/'src/main.cpp').read_text(encoding='utf-8')).group(1)
out = root/'releases'/version/args.environment
out.mkdir(parents=True, exist_ok=True)
partitions = {}
data = (build/'partitions.bin').read_bytes()
for i in range(0, len(data), 32):
    magic, kind, sub, offset, size, label, flags = struct.unpack('<HBBII16sI', data[i:i+32])
    if magic != 0x50aa: break
    partitions[label.rstrip(b'\0').decode()] = (offset, size)
assert partitions['app0'] == (0x10000, 0x1e0000)
assert partitions['app1'] == (0x1f0000, 0x1e0000)
assert (build/'firmware.bin').stat().st_size < 0x1e0000
for name in ('bootloader.bin', 'firmware.bin'):
    header = (build/name).read_bytes()[:4]
    assert header[0] == 0xe9 and header[3] >> 4 == {'4MB': 2, '16MB': 4}[flash_size], (name, 'wrong flash size')
assert version.encode() + b'\0' in (build/'firmware.bin').read_bytes(), 'Rebuild this environment before packaging'
images = {0x1000:build/'bootloader.bin', 0x8000:build/'partitions.bin',
          0xe000:Path.home()/'.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin',
          0x10000:build/'firmware.bin'}
merged = bytearray(b'\xff'*(0x10000+(build/'firmware.bin').stat().st_size))
manifest = dict(version=version, environment=args.environment, board=profile['board'], flash_size=flash_size,
                contains_private_credentials=(root/'.local/bootstrap.h').exists(), files={})
for offset, source in images.items():
    content = source.read_bytes(); merged[offset:offset+len(content)] = content
    shutil.copy2(source, out/source.name)
    manifest['files'][source.name] = dict(offset=hex(offset), bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
(out/'factory.bin').write_bytes(merged)
manifest['files']['factory.bin'] = dict(offset='0x0',bytes=len(merged),sha256=hashlib.sha256(merged).hexdigest())
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
(out/'CTI-ME.txt').write_text(f'{label}, {flash_size} flash; DRV8833. Firmware {version}.\n'
    'GPIO25 = AIN1, GPIO26 = AIN2, GPIO27 = nSLEEP, společná zem GND.\n'
    'GPIO32 = snímání ochrany. GPIO37 se neřídí, může být fyzicky spojen s GND.\n'
    'Ochranu nastav ve webu Světlo > Ochrana proti zkratu. První start čeká na kalibraci.\n'
    'Výchozí reakce na závadu: jen vypnout. Volitelné trvalé blokování se nastavuje na webu.\n'
    'První USB instalace: factory.bin od adresy 0x0, pouze na určenou novou desku.\n'
    'Další aktualizace přes web: firmware.bin.\n'
    'Web i OTA jsou bez přihlášení a hesla.\n'
    'Výchozí alias ledpasek01 lze změnit ve webu: Systém > Název zařízení. Web: http://ledpasek01.local/.\n'
    'MQTT základ wifi2mqtt/ledpasek01; po změně aliasu wifi2mqtt/<alias>.\n'
    'Otevřené AP má jméno aliasu: po 30 s bez domácí Wi-Fi, http://192.168.4.1/. Po připojení se ihned vypne.\n'
    'Obsahuje místní Wi-Fi/MQTT přístupy, nesdílet.\n'
    'Napětí řetězu a omezení proudu musí být ověřeny před připojením. Viz README.md.\n',encoding='utf-8')
print(f'Packaged {version}/{args.environment}: {len(merged)} bytes, {flash_size}, two OTA slots verified.')
