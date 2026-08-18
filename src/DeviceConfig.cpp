#include "DeviceConfig.h"
#include "Secrets.h"
#include "WifiPortal.h"

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

static bool g_hadStoredWifiCreds = false;

void loadWifiCreds() {
    wifiPrefs.begin("wifi", false);
    String storedSsid = wifiPrefs.getString("ssid", "");
    String storedPass = wifiPrefs.getString("pass", "");
    g_wifiApplied      = wifiPrefs.getString("applied", "");
    wifiPrefs.end();

    g_hadStoredWifiCreds = storedSsid.length() > 0;
    if (g_hadStoredWifiCreds) {
        snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", storedSsid.c_str());
        snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", storedPass.c_str());
    } else {
        snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", WIFI_SSID);
        snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", WIFI_PASS);
    }
}

void saveWifiCreds(const char* ssid, const char* pass, const char* appliedAt) {
    wifiPrefs.begin("wifi", false);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
    wifiPrefs.putString("applied", appliedAt);
    wifiPrefs.end();
}

void markPortalProvisioned() {
    wifiPrefs.begin("wifi", false);
    wifiPrefs.putBool("portal_ok", true);
    wifiPrefs.end();
}

// Boot-time connect with a SAFE forced-reset path. Previously, bumping
// WIFI_FORCE_RESET_TAG wiped the saved network unconditionally, before ever
// testing whether WIFI_SSID/WIFI_PASS is actually reachable from this
// device's physical location - fine for the single-router-got-reprovisioned
// case this was designed for, but on a multi-site fleet it permanently
// stranded a device whose deployed location was never near that hardcoded
// network at all (confirmed live 2026-08-07: force-reset fired, "Prem" was
// unreachable, NVS was already wiped, and nothing else in this firmware ever
// calls checkRemoteWifiConfig() unless the initial boot connection already
// succeeded - the device had no path back online without a USB reflash).
// Now the reset is provisional: the new default has to actually prove
// reachable before we give up the working saved config, and if it doesn't,
// we keep using what we had and just try again next boot.
bool connectWifiAtBoot(uint32_t timeoutMs) {
    wifiPrefs.begin("wifi", false);
    String forceTag = wifiPrefs.getString("force_tag", "");
    wifiPrefs.end();

    bool resetPending = (forceTag != WIFI_FORCE_RESET_TAG);

    if (resetPending && g_hadStoredWifiCreds) {
        Serial.printf("\n[WIFI] Force-reset pending — testing default \"%s\" before giving up saved config...", WIFI_SSID);
        if (connectWifi(WIFI_SSID, WIFI_PASS, timeoutMs)) {
            wifiPrefs.begin("wifi", false);
            wifiPrefs.remove("ssid");
            wifiPrefs.remove("pass");
            wifiPrefs.remove("applied");
            wifiPrefs.putString("force_tag", WIFI_FORCE_RESET_TAG);
            wifiPrefs.end();
            snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", WIFI_SSID);
            snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", WIFI_PASS);
            g_wifiApplied = "";
            Serial.println("\n[WIFI] Default reachable — force reset applied.");
            return true;
        }
        Serial.println("\n[WIFI] Default unreachable — keeping existing saved config, will retry the reset next boot.");
        // g_wifiSsid/g_wifiPass still hold the untouched saved credentials
        // from loadWifiCreds() - fall through and try those instead.
    } else if (resetPending) {
        // Nothing saved to protect (brand-new board) - safe to just adopt
        // the new tag now, no separate attempt needed.
        wifiPrefs.begin("wifi", false);
        wifiPrefs.putString("force_tag", WIFI_FORCE_RESET_TAG);
        wifiPrefs.end();
    }

    return connectWifi(g_wifiSsid, g_wifiPass, timeoutMs);
}

bool connectWifi(const char* ssid, const char* pass, uint32_t timeoutMs) {
    // disconnect(true) powers the radio down, which also takes the setup
    // portal's access point with it - and this is called *from* the portal to
    // test what the customer typed. Only tear the radio down when there's no
    // AP to protect.
    WiFi.disconnect(!portalIsActive());
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

// Does Supabase have a WiFi config for THIS machine that we haven't already
// applied? If so, connect with it, and only keep it (save to NVS) if that
// connection actually succeeds - a typo'd password from the website can't
// strand the device offline forever, it just falls back to whatever it was
// already using.
//
// Called at boot and then polled from loop() every WIFI_CONFIG_POLL_MS. It
// was boot-only until 2026-08-18: a customer could save a new network on the
// dashboard and watch nothing happen, because the device would not look again
// until it was physically restarted.
void checkRemoteWifiConfig() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return; // no resolved machine_id to look up yet

    // If the setup portal provisioned this device, machine_wifi is by
    // definition still holding the credentials that stopped working - that's
    // what forced someone to walk up to the unit. Applying them here would
    // burn a guaranteed-to-fail 15s connect attempt on every boot until the
    // dashboard catches up, so skip exactly once. reportWifiStatus() pushes
    // the real credentials up within 30s, after which this resumes normally.
    wifiPrefs.begin("wifi", false);
    bool justProvisioned = wifiPrefs.getBool("portal_ok", false);
    if (justProvisioned) wifiPrefs.remove("portal_ok");
    wifiPrefs.end();
    if (justProvisioned) {
        Serial.println("[WIFI] Credentials came from the setup portal — skipping remote config once.");
        return;
    }

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
