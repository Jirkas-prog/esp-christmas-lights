#pragma once
#include <ArduinoJson.h>
#include <string.h>

constexpr char DefaultAlias[] = "ledpasek02";
inline bool validAlias(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > 32 || s[0] == '-' || s[n - 1] == '-') return false;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
    return true;
}

struct Settings {
    uint32_t version = 1;
    char ssid[33] = {}, wifiPassword[65] = {};
    char mqttHost[97] = {}, mqttUser[65] = {}, mqttPassword[97] = {};
    uint16_t mqttPort = 1883;
    bool mqttEnabled = true;
};
template<size_t N> inline bool copyText(JsonVariantConst value, char (&out)[N]) {
    if (!value.is<const char *>()) return false;
    const char *s = value.as<const char *>(); size_t n = value.as<JsonString>().size();
    if (n >= N || n != strlen(s)) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)s[i] < 32 || s[i] == 127) return false;
    memcpy(out, s, n + 1); return true;
}
inline const char *parseAlias(JsonObjectConst doc, char (&out)[33]) {
    char next[33];
    if (doc.isNull() || doc.size() != 1 || !copyText(doc["alias"], next) || !validAlias(next))
        return "Alias: 1–32 malých písmen a–z, číslic nebo pomlček; pomlčka nesmí být na kraji.";
    strcpy(out, next); return nullptr;
}
inline bool hostValid(const char *s) {
    if (!*s) return true;
    unsigned label = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '.') { if (!label || p[-1] == '-') return false; label = 0; continue; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
        if ((!label && *p == '-') || ++label > 63) return false;
    }
    return label && s[strlen(s) - 1] != '-';
}
inline bool validSettings(const Settings &s) {
    if (s.version != 1 || !memchr(s.ssid, 0, sizeof(s.ssid)) || !memchr(s.wifiPassword, 0, sizeof(s.wifiPassword)) ||
        !memchr(s.mqttHost, 0, sizeof(s.mqttHost)) || !memchr(s.mqttUser, 0, sizeof(s.mqttUser)) ||
        !memchr(s.mqttPassword, 0, sizeof(s.mqttPassword))) return false;
    size_t n = strlen(s.wifiPassword);
    return (!n || (n >= 8 && n <= 63)) && s.mqttPort && hostValid(s.mqttHost);
}
inline const char *parseSettings(JsonObjectConst doc, Settings &s) {
    if (doc.isNull() || !doc.size()) return "Očekávám nastavení.";
    Settings next = s;
    for (JsonPairConst p : doc) {
        const char *k = p.key().c_str(); bool ok = true;
        if (!strcmp(k, "ssid")) ok = copyText(p.value(), next.ssid);
        else if (!strcmp(k, "mqtt_host")) ok = copyText(p.value(), next.mqttHost);
        else if (!strcmp(k, "mqtt_user")) ok = copyText(p.value(), next.mqttUser);
        else if (!strcmp(k, "wifi_password")) { char v[65]; ok = copyText(p.value(), v); if (ok && *v) strcpy(next.wifiPassword, v); }
        else if (!strcmp(k, "mqtt_password")) { char v[97]; ok = copyText(p.value(), v); if (ok && *v) strcpy(next.mqttPassword, v); }
        else if (!strcmp(k, "clear_wifi_password") || !strcmp(k, "clear_mqtt_password")) {
            ok = p.value().is<bool>();
            if (ok && p.value().as<bool>()) (!strcmp(k, "clear_wifi_password") ? next.wifiPassword : next.mqttPassword)[0] = 0;
        } else if (!strcmp(k, "mqtt_port")) {
            ok = p.value().is<unsigned>() && p.value().as<unsigned>() >= 1 && p.value().as<unsigned>() <= 65535;
            if (ok) next.mqttPort = p.value();
        } else if (!strcmp(k, "mqtt_enabled")) { ok = p.value().is<bool>(); if (ok) next.mqttEnabled = p.value(); }
        else return "Neznámé pole nastavení.";
        if (!ok) return "Neplatný typ nebo příliš dlouhá hodnota.";
    }
    if (!validSettings(next)) return "Zkontrolujte adresu brokeru, port a délku hesla Wi-Fi (8–63 bajtů).";
    s = next; return nullptr;
}
