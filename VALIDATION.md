# Ověření zdrojového snímku

Datum: 1. 11. 2026.

- tools/test.ps1: hostitelské testy plánů, řízení LED, ADC, ochrany a sítí prošly.
- Gitleaks 8.30.1: kontrola exportovaných zdrojů prošla. Příklady obsahují pouze zástupné hodnoty.
- Testy běžely na Windows / MSVC 2022; potřebné knihovny byly dostupné místně.

Úplné sestavení firmwaru, instalace na desku a testy připojené k reálnému zařízení nebyly při tomto importu provedeny. Tento snímek není nové binární vydání.

Před veřejným vydáním byla odstraněna výchozí poloha instalace. Hostitelské testy znovu ověřují neutrální výchozí hodnoty a výpočty vůči veřejnému referenčnímu bodu Greenwich.
