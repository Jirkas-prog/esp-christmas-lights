"""Offline checks for the embedded language asset and HTTP registration capacity."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LanguageAssets(unittest.TestCase):
    def test_embedded_asset_is_served_and_loaded_before_application(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        config = (ROOT / 'platformio.ini').read_text(encoding='utf-8')
        html = (ROOT / 'web/index.html').read_text(encoding='utf-8')
        self.assertIn('web/i18n.js', config)
        self.assertIn('_binary_web_i18n_js_start', source)
        self.assertIn('path == "/i18n.js" ? languageStart', source)
        self.assertLess(html.index('src="/i18n.js"'), html.index('src="/app.js"'))

    def test_http_server_can_register_all_routes_including_reliability_build(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        capacity = int(re.search(r'config.max_uri_handlers\s*=\s*(\d+)', source)[1])
        groups = re.findall(r'for \(const char \*path : \{([^}]+)\}\)', source)
        self.assertEqual(len(groups), 2)
        routes = sum(len(re.findall(r'"[^\"]+"', group)) for group in groups)
        self.assertGreaterEqual(capacity, routes + 1, 'Allow the optional /api/test route too')
        self.assertIn('"/i18n.js"', groups[0])


if __name__ == '__main__':
    unittest.main()
