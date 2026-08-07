#include "DeviceConfig.h"
#include "Secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>

char g_machineId[40] = {0};
char g_chipId[13]    = {0};
char g_wifiSsid[33];
char g_wifiPass[65];

static Preferences wifiPrefs;
static String      g_wifiApplied;

// ── Chip identity ────────────────────────────────────────────────────────────
// WiFi.macAddress() reads the base MAC burned into the ESP32's efuse at the
// factory - unique per chip and immutable, so it works as a hardware serial
// number without needing a compile-time constant that would differ per
// board. Colons are stripped only for a cleaner/URL-safer string; nothing
// about the value itself changes.
void initChipId() {
    String mac = WiFi.macAddress();   // e.g. "AA:BB:CC:DD:EE:FF"
    int j = 0;
    for (int i = 0; i < (int)mac.length() && j < 12; i++) {
        if (mac.charAt(i) != ':') g_chipId[j++] = mac.charAt(i);
    }
    g_chipId[j] = '\0';
}

// ── Machine identity (chip_id -> machines.id UUID) ───────────────────────────
// resolve_machine_id_by_chip(p_chip_id) is a Postgres function exposed over
// PostgREST as a GET-able RPC; a scalar-returning RPC responds with the bare
// JSON value (a quoted string here), not an object, so we just strip the
// quotes.
bool resolveMachineId() {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    String url = String(SUPABASE_URL) + "/rest/v1/rpc/resolve_machine_id_by_chip?p_chip_id=" + g_chipId;
    http.begin(client, url);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[MACHINE] resolve_machine_id_by_chip FAILED (HTTP %d) — is chip_id \"%s\" set on a row in the machines table?\n",
            code, g_chipId);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    body.trim();
    if (body.length() < 2 || body == "null") {
        Serial.printf("[MACHINE] resolve_machine_id_by_chip returned no match for chip_id \"%s\"\n", g_chipId);
        return false;
    }
    if (body.charAt(0) == '"') body = body.substring(1, body.length() - 1);

    snprintf(g_machineId, sizeof(g_machineId), "%s", body.c_str());
    Serial.printf("[MACHINE] Resolved chip_id \"%s\" -> %s\n", g_chipId, g_machineId);
    return true;
}

// ── Remote WiFi config (Settings page -> machine_wifi table) ────────────────
// NVS-backed so a credential change survives power loss. `applied` stores the
// updated_at of whichever config we last successfully connected with, so the
// boot-once check below can tell "nothing new" from "there's a pending
// change" without ever polling more than once per boot.

void loadWifiCreds() {
    wifiPrefs.begin("wifi", false);
    String forceTag = wifiPrefs.getString("force_tag", "");
    if (forceTag != WIFI_FORCE_RESET_TAG) {
        wifiPrefs.remove("ssid");
        wifiPrefs.remove("pass");
        wifiPrefs.remove("applied");
        wifiPrefs.putString("force_tag", WIFI_FORCE_RESET_TAG);
        Serial.println("[WIFI] Forced reset tag changed — cleared saved WiFi config, using hardcoded default.");
    }
    String storedSsid = wifiPrefs.getString("ssid", "");
    String storedPass = wifiPrefs.getString("pass", "");
    g_wifiApplied      = wifiPrefs.getString("applied", "");
    wifiPrefs.end();

    if (storedSsid.length() > 0) {
        snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", storedSsid.c_str());
        snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", storedPass.c_str());
    } else {
        snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", WIFI_SSID);
        snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", WIFI_PASS);
    }
}

static void saveWifiCreds(const char* ssid, const char* pass, const char* appliedAt) {
    wifiPrefs.begin("wifi", false);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
    wifiPrefs.putString("applied", appliedAt);
    wifiPrefs.end();
}

bool connectWifi(const char* ssid, const char* pass, uint32_t timeoutMs) {
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid, pass);
    bool     ledState  = false;
    uint32_t lastBlink = 0;
    uint32_t deadline  = millis() + timeoutMs;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        if (millis() - lastBlink >= 300) {
            lastBlink = millis();
            ledState  = !ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
        delay(50);
        Serial.print(".");
    }
    return WiFi.status() == WL_CONNECTED;
}

// Boot-once: does Supabase have a WiFi config for THIS machine that we
// haven't already applied? If so, connect with it, and only keep it (save to
// NVS) if that connection actually succeeds - a typo'd password from the
// website can't strand the device offline forever, it just falls back to
// whatever it was already using for the rest of this session.
void checkRemoteWifiConfig() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return; // no resolved machine_id to look up yet

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    // anon has no direct SELECT access to machine_wifi (RLS is authenticated-only,
    // by design - see get_machine_wifi in Supabase), so this goes through a
    // SECURITY DEFINER RPC scoped to just this machine's own row instead.
    String url = String(SUPABASE_URL) + "/rest/v1/rpc/get_machine_wifi?mid=" + g_machineId;
    http.begin(client, url);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);

    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    if (body.indexOf("\"ssid\"") < 0) return; // no config set for this machine yet

    auto extract = [&](const char* key) -> String {
        String k = String("\"") + key + "\":\"";
        int i = body.indexOf(k);
        if (i < 0) return "";
        i += k.length();
        int j = body.indexOf('"', i);
        return body.substring(i, j);
    };
    String newSsid      = extract("ssid");
    String newPass      = extract("password");
    String newUpdatedAt = extract("updated_at");
    if (newSsid.length() == 0 || newUpdatedAt == g_wifiApplied) return; // nothing new

    Serial.printf("\n[WIFI] Remote config change found (SSID: %s) - applying...\n", newSsid.c_str());
    if (connectWifi(newSsid.c_str(), newPass.c_str(), 15000)) {
        saveWifiCreds(newSsid.c_str(), newPass.c_str(), newUpdatedAt.c_str());
        snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", newSsid.c_str());
        snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", newPass.c_str());
        g_wifiApplied = newUpdatedAt;
        Serial.printf("\n[WIFI] New credentials applied and saved — IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WIFI] New credentials FAILED to connect — reverting to previous network for this session.");
        connectWifi(g_wifiSsid, g_wifiPass, 15000);
    }
}
