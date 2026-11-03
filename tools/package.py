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
label = ('LILYGO T-Display, ESP32-D0WDQ6, display unused'
         if args.environment == 'tdisplay' else 'ESP32-WROOM-32 / ESP32 DevKit, no display')
build = root/'.pio/build'/args.environment
version = re.search(r'Version\[\] = "([0-9.]+)"', (root/'src/main.cpp').read_text(encoding='utf-8')).group(1)
out = root/'releases'/version/args.environment
out.mkdir(parents=True, exist_ok=True)
partitions = {}
data = (build/'partitions.bin').read_bytes()
for i in range(0, len(data), 32):
    magic, kind, sub, offset, size, partition_label, flags = struct.unpack('<HBBII16sI', data[i:i+32])
    if magic != 0x50aa: break
    partitions[partition_label.rstrip(b'\0').decode()] = (offset, size)
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
(out/'README.txt').write_text(f'{label}, {flash_size} flash; DRV8833. Firmware {version}.\n'
    'GPIO25 = AIN1, GPIO26 = AIN2, GPIO27 = nSLEEP; common GND.\n'
    'GPIO32 = point A after 5.6 ohms, through a 3k/3k divider and capacitor to GND. GPIO37 is not driven; it may be physically connected to GND.\n'
    'Select AOUT1 or AOUT2 in the web interface to match the resistor connection. Changing it requires recalibration.\n'
    'Configure protection under Light > Short-circuit protection. The first start waits for calibration.\n'
    'Default fault response: turn off only. Optional persistent latching is configured in the web interface.\n'
    'First USB installation: factory.bin at address 0x0, only on the intended new board.\n'
    'Subsequent web updates: firmware.bin.\n'
    'Web and OTA require no login or password. Choose EN or CZ in the web header.\n'
    'Change the default alias ledpasek02 under System > Device name. Web: http://ledpasek02.local/.\n'
    'MQTT base: wifi2mqtt/ledpasek02; after an alias change: wifi2mqtt/<alias>.\n'
    'The open AP uses the alias: it starts after 30 s without home Wi-Fi, at http://192.168.4.1/, and stops when the connection recovers.\n'
    + ('Contains local Wi-Fi/MQTT credentials. Do not share.\n' if manifest['contains_private_credentials'] else 'Built without .local/bootstrap.h. Review configuration before sharing.\n')
    + 'Verify string voltage and current limiting before connection. See the repository README.md.\n',encoding='utf-8')
print(f'Packaged {version}/{args.environment}: {len(merged)} bytes, {flash_size}, two OTA slots verified.')
