#include <Arduino.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSps30.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include <math.h>
#include <string.h>
#include <time.h>

// ── OTA rollback safety net ──────────────────────────────────────────────────
// The bootloader is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, so a
// freshly-OTA'd image boots in ESP_OTA_IMG_PENDING_VERIFY and the bootloader
// will revert to the previous partition if the device reboots before that
// image is confirmed good.
//
// The catch: Arduino's initArduino() runs BEFORE setup() and, by default,
// confirms the image unconditionally the moment it sees PENDING_VERIFY. So
// the safety net was compiled in but permanently disarmed - a CI build that
// panicked at boot would have bricked every device that pulled it, with USB
// reflash the only recovery.
//
// Overriding this weak hook (declared in the core's esp32-hal-misc.c) to
// return true tells the core to skip that auto-confirm and leave the
// decision to us - see confirmFirmwareIfHealthy() below.
extern "C" bool verifyRollbackLater() { return true; }

#include "Secrets.h"
#include "DeviceConfig.h"
#include "WifiPortal.h"

#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

#define NUM_SAMPLES       5
#define WARMUP_SECONDS    15
#define I2C_CLOCK_HZ      100000
#define MAX_READY_WAIT_MS 2500
#define EMA_ALPHA         0.25f
#define TEMP_OFFSET_C     3.4f
#define UPLOAD_INTERVAL_MS 5000   // sensors keep sampling into memory every loop; only upload this often
#define STATUS_REPORT_INTERVAL_MS 30000

// How often to re-check the dashboard for a WiFi config change. This used to
// happen ONCE, in setup(), which meant a customer who changed the network on
// the website saw nothing happen until somebody physically power-cycled the
// unit - a site visit for anything wall-mounted. checkRemoteWifiConfig() is a
// no-op unless machine_wifi.updated_at differs from what we last applied, so
// polling costs one small request a minute and normally does nothing at all.
#define WIFI_CONFIG_POLL_MS 60000

// CI injects the real version via a -D FIRMWARE_VERSION build flag on every push to
// main (see .github/workflows/ota.yml) - this exact string is what's compared against
// machine_firmware.latest_version. The fallback below only applies to local manual
// builds outside CI.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif
#define OTA_CHECK_INTERVAL_MS (10UL * 60UL * 1000UL)

#define SPS30_STATUS_FAN_ERROR    (1UL << 3)
#define SPS30_STATUS_LASER_ERROR  (1UL << 4)

SensirionI2cSps30 sps30;
SensirionI2cScd4x scd4x;

static int16_t err;
static uint8_t statusCheckCounter = 0;
static float   ema[4] = {-1.f, -1.f, -1.f, -1.f};

static int      g_pm[4] = {0, 0, 0, 0};
static uint16_t g_co2   = 0;
static float    g_temp  = 0;
static float    g_humid = 0;

// ── Array utilities ───────────────────────────────────────────────────────────
static void insertionSort(float* a, int n) {
    for (int i = 1; i < n; i++) {
        float key = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
}

static int tukeyClean(float* sorted, int n, float* clean) {
    float iqr = sorted[(3*n)/4] - sorted[n/4];
    float lo  = sorted[n/4]     - 1.5f * iqr;
    float hi  = sorted[(3*n)/4] + 1.5f * iqr;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (sorted[i] >= lo && sorted[i] <= hi) clean[k++] = sorted[i];
    return k;
}

// ── Supabase upload ───────────────────────────────────────────────────────────
static void pushToSupabase() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DB] WiFi offline — skipped");
        return;
    }
    if (g_machineId[0] == '\0') {
        Serial.println("[DB] machine_id not resolved yet — skipped");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.begin(client, SUPABASE_URL "/rest/v1/sensor_data");
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    http.addHeader("Prefer",        "return=minimal");

    char json[320];
    snprintf(json, sizeof(json),
        "{\"machine_id\":\"%s\",\"pm1_0\":%d,\"pm2_5\":%d,\"pm4_0\":%d,\"pm10\":%d,"
        "\"co2\":%u,\"temperature\":%.1f,\"humidity\":%.1f}",
        g_machineId, g_pm[0], g_pm[1], g_pm[2], g_pm[3],
        g_co2, g_temp, g_humid);

    Serial.printf("[DB] Sending: %s\n", json);

    int code = http.POST(json);
    if (code == 201) {
        Serial.println("[DB] Saved OK");
    } else if (code < 0) {
        Serial.printf("[DB] Connection failed: %s\n", http.errorToString(code).c_str());
    } else {
        Serial.printf("[DB] HTTP %d  Body: %s\n", code, http.getString().c_str());
    }
    http.end();
}

// millis() is a uint32_t of milliseconds, so it silently wraps back to 0
// after ~49.7 days of continuous uptime. The scheduling comparisons in
// loop() are all of the form (millis() - last >= interval), which stays
// correct across a wrap thanks to unsigned arithmetic - but reporting
// millis()/1000 as an absolute uptime does NOT: it would drop from ~4.3M
// seconds straight back to 0, indistinguishable from a device that just
// rebooted. Counting the wraps keeps the reported total honest.
static uint64_t uptimeSeconds() {
    static uint32_t lastMillis = 0;
    static uint32_t wraps      = 0;
    uint32_t now = millis();
    if (now < lastMillis) wraps++;   // only possible via a 32-bit overflow
    lastMillis = now;
    return ((uint64_t)wraps << 32 | now) / 1000ULL;
}

// Building JSON with snprintf and a raw user-supplied string breaks the moment
// that string contains a double quote or a backslash - both perfectly legal in
// a WiFi name or password, and common in generated passwords. The result is
// malformed JSON, a rejected RPC, and WiFi status that silently stops updating
// with nothing anywhere explaining why. Escape properly instead.
static void jsonEscape(const char* in, char* out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= outSize) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (o + 6 >= outSize) break;      // control chars must be \u00XX
            o += snprintf(out + o, outSize - o, "\\u%04x", (unsigned)(unsigned char)c);
        } else {
            if (o + 1 >= outSize) break;
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

// ── Machine status heartbeat ─────────────────────────────────────────────────
// Reports liveness to the website's "Quick Status" panel via a SECURITY
// DEFINER RPC (anon has no direct write access to machine_status - see
// report_machine_heartbeat in Supabase). Sets is_online=true and last_seen=now()
// every call, so this must be called periodically, not just once at boot.
static void reportMachineHeartbeat(const char* sensorStatus) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.begin(client, SUPABASE_URL "/rest/v1/rpc/report_machine_heartbeat");
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    http.addHeader("Prefer",        "return=minimal");

    char json[192];
    snprintf(json, sizeof(json),
        "{\"mid\":\"%s\",\"p_uptime_seconds\":%llu,\"p_sensor_status\":\"%s\"}",
        g_machineId, (unsigned long long)uptimeSeconds(), sensorStatus);

    int code = http.POST(json);
    if (code != 204 && code != 200) {
        Serial.printf("[STATUS] Heartbeat failed (HTTP %d): %s\n", code, http.getString().c_str());
    }
    http.end();
}

// Reports WiFi connection status to the website's "Wi-Fi Status" field, via
// the same SECURITY-DEFINER-RPC pattern (anon has no direct write access to
// machine_wifi). Uses this device's own known credentials/IP - see
// report_wifi_status in Supabase.
static void reportWifiStatus() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.begin(client, SUPABASE_URL "/rest/v1/rpc/report_wifi_status");
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    http.addHeader("Prefer",        "return=minimal");

    // Worst case every character needs escaping, so allow 2x the source buffers.
    char ssidEsc[sizeof(g_wifiSsid) * 2];
    char passEsc[sizeof(g_wifiPass) * 2];
    jsonEscape(g_wifiSsid, ssidEsc, sizeof(ssidEsc));
    jsonEscape(g_wifiPass, passEsc, sizeof(passEsc));

    char json[520];
    snprintf(json, sizeof(json),
        "{\"mid\":\"%s\",\"p_ssid\":\"%s\",\"p_password\":\"%s\",\"p_ip\":\"%s\"}",
        g_machineId, ssidEsc, passEsc, WiFi.localIP().toString().c_str());

    int code = http.POST(json);
    if (code != 204 && code != 200) {
        Serial.printf("[STATUS] WiFi status report failed (HTTP %d): %s\n", code, http.getString().c_str());
    }
    http.end();
}

// ── OTA firmware updates ─────────────────────────────────────────────────────
// Reports this device's own compiled-in FIRMWARE_VERSION as its current
// version. Called once at boot - CI-published versions only ever change
// latest_version, so there's nothing new to report until a real OTA happens
// and the device reboots into a new build (with a new FIRMWARE_VERSION).
static void reportFirmwareVersion() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.begin(client, SUPABASE_URL "/rest/v1/rpc/report_firmware_version");
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    http.addHeader("Prefer",        "return=minimal");

    char json[128];
    snprintf(json, sizeof(json), "{\"mid\":\"%s\",\"p_version\":\"%s\"}", g_machineId, FIRMWARE_VERSION);

    int code = http.POST(json);
    if (code != 204 && code != 200) {
        Serial.printf("[OTA] Version report failed (HTTP %d): %s\n", code, http.getString().c_str());
    }
    http.end();
}

// Polls this machine's latest_version via a SECURITY DEFINER RPC (anon has no
// direct read access to machine_firmware - see get_latest_firmware in
// Supabase). If it differs from our own FIRMWARE_VERSION, downloads and
// flashes that build from the public "firmware" Storage bucket, then
// reboots into it - httpUpdate.update() only returns on failure/no-update.
static void checkForOtaUpdate() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    String url = String(SUPABASE_URL) + "/rest/v1/rpc/get_latest_firmware?mid=" + g_machineId;
    http.begin(client, url);
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);

    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    body.trim();
    if (body.length() < 2 || body == "null") return; // this machine has never reported a version yet
    if (body.charAt(0) == '"') body = body.substring(1, body.length() - 1);
    if (body == FIRMWARE_VERSION) return; // already up to date

    Serial.printf("[OTA] New firmware available: %s (current: %s). Downloading...\n", body.c_str(), FIRMWARE_VERSION);

    httpUpdate.rebootOnUpdate(true);

    // Authenticated storage path FIRST (note: no "/public/"), so the firmware
    // bucket can be switched to private. While the bucket was public, every
    // build - and therefore the anon key, the AP setup password and the
    // fallback WiFi credentials baked into it - could be downloaded by anyone
    // who knew the URL. Reading it with the apikey header instead means an
    // attacker needs the key to get a binary, and needs a binary to get the
    // key, which breaks that bootstrap for anyone without physical access.
    //
    // The public path is kept as a fallback purely so this build works either
    // way: it has to be delivered over the CURRENTLY public bucket, and must
    // keep working if the private-bucket policy isn't in place yet. Once every
    // device reports a version >= 2.4, the fallback can be deleted.
    WiFiClientSecure otaClient;
    otaClient.setInsecure();

    HTTPClient otaHttp;
    otaHttp.setTimeout(20000);
    otaHttp.begin(otaClient, String(SUPABASE_URL) + "/storage/v1/object/firmware/" + body + ".bin");
    otaHttp.addHeader("apikey",        SUPABASE_KEY);
    otaHttp.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    t_httpUpdate_return ret = httpUpdate.update(otaHttp, FIRMWARE_VERSION);

    if (ret == HTTP_UPDATE_FAILED) {
        Serial.printf("[OTA] Authenticated fetch failed (%d): %s — trying the public path.\n",
            httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());

        WiFiClientSecure pubClient;
        pubClient.setInsecure();
        String publicUrl = String(SUPABASE_URL) + "/storage/v1/object/public/firmware/" + body + ".bin";
        ret = httpUpdate.update(pubClient, publicUrl);
    }

    if (ret == HTTP_UPDATE_FAILED) {
        Serial.printf("[OTA] Update failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    } else if (ret == HTTP_UPDATE_NO_UPDATES) {
        Serial.println("[OTA] Server reported no update available.");
    }
    // HTTP_UPDATE_OK reboots immediately (rebootOnUpdate(true)) - no code after it runs.
}

// ── OTA image confirmation ───────────────────────────────────────────────────
// Confirms the running image only once it has proven it can still be
// REACHED - WiFi up and machine_id resolved against Supabase. That's the
// criterion that actually matters: an image which can talk to the backend
// can always be replaced by pushing another OTA, so it's recoverable. One
// that can't is exactly what rollback exists to undo.
//
// Sensor health is deliberately NOT part of this. A dead I2C sensor is a
// hardware fault that reverting firmware won't fix, and rolling back over it
// would just churn versions while the real problem persists.
//
// Not confirming is safe and non-destructive: the image simply stays pending,
// and this retries every loop. Rollback only happens if the device REBOOTS
// while still unconfirmed - i.e. the crash-loop case this is meant to catch.
static void confirmFirmwareIfHealthy() {
    static bool settled = false;
    if (settled) return;

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        settled = true;   // can't determine state (e.g. factory/USB-flashed) - nothing to confirm
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        settled = true;   // already valid, or not an OTA image at all
        return;
    }

    // Health gate. Both must hold before we commit to this build.
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        settled = true;
        Serial.printf("[OTA] Image %s confirmed healthy — rollback cancelled.\n", FIRMWARE_VERSION);
    } else {
        Serial.println("[OTA] esp_ota_mark_app_valid_cancel_rollback() FAILED — image stays pending.");
    }
}

// ── Cloud-triggered FRC calibration ─────────────────────────────────────────────
// Polls machine_commands for this machine's oldest Pending frc_calibration
// row. payload is {"target": <ppm of the known fresh air the sensor is in>}.
// Runs a Forced Recalibration against that target and marks the command
// Done/Failed with executed_at so it runs exactly once.
static void checkCalibrationRequest() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_machineId[0] == '\0') return;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    String url = String(SUPABASE_URL) + "/rest/v1/machine_commands?select=id,payload"
                 "&machine_id=eq." + g_machineId +
                 "&command_type=eq.frc_calibration&status=eq.Pending"
                 "&order=created_at.asc&limit=1";
    http.begin(client, url);
    http.addHeader("apikey",        SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_KEY);

    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    if (body.length() < 3) return; // "[]" — no request pending

    // Dependency-free parse (response is a tiny single-row JSON array)
    String commandId;
    int idPos = body.indexOf("\"id\":\"");
    if (idPos < 0) return;
    idPos += 6;
    int idEnd = body.indexOf('"', idPos);
    commandId = body.substring(idPos, idEnd);

    uint16_t target = 420;
    int tp = body.indexOf("\"target\":");
    if (tp >= 0) target = (uint16_t)atoi(body.c_str() + tp + 9);

    Serial.printf("[CAL] FRC request %s received — target %u ppm. Running FRC...\n",
        commandId.c_str(), target);

    int  correction = 0;
    bool ok         = false;

    // atoi() returns 0 for a missing, malformed or non-numeric payload, and an
    // FRC against 0 ppm would write a wildly wrong baseline into the sensor's
    // EEPROM - permanently, and not undoable from the cloud. Only accept a
    // figure that could plausibly be real ambient air.
    if (target < 300 || target > 2000) {
        Serial.printf("[CAL] Refusing FRC: target %u ppm is outside the plausible ambient "
                      "range (300-2000). Check the command payload.\n", target);
    } else {
        scd4x.stopPeriodicMeasurement();
        delay(500);
        uint16_t raw = 0;
        int16_t  e   = scd4x.performForcedRecalibration(target, raw);
        if (e == NO_ERROR && raw != 0xFFFF) {
            correction = (int)raw - 32768;   // datasheet: applied correction = word - 0x8000
            ok = true;
            Serial.printf("[CAL] FRC OK. Correction = %+d ppm (baseline was %s).\n",
                correction, correction < 0 ? "HIGH" : "LOW");
        } else {
            Serial.printf("[CAL] FRC FAILED (err=%d raw=0x%04X) — air not stable/known.\n", e, raw);
        }
        scd4x.persistSettings();
        delay(1000);
        scd4x.startPeriodicMeasurement();
    }

    // Report back: mark this command Done/Failed so it runs exactly once
    char patch[96];
    snprintf(patch, sizeof(patch),
        "{\"status\":\"%s\",\"executed_at\":\"now()\"}", ok ? "Done" : "Failed");

    HTTPClient ph;
    ph.setTimeout(10000);
    String patchUrl = String(SUPABASE_URL) + "/rest/v1/machine_commands?id=eq." + commandId;
    ph.begin(client, patchUrl);
    ph.addHeader("Content-Type",  "application/json");
    ph.addHeader("apikey",        SUPABASE_KEY);
    ph.addHeader("Authorization", "Bearer " SUPABASE_KEY);
    ph.addHeader("Prefer",        "return=minimal");
    int pc = ph.sendRequest("PATCH", (uint8_t*)patch, strlen(patch));
    Serial.printf("[CAL] Result reported (HTTP %d).\n", pc);
    ph.end();
}

// ── SPS30 helpers ─────────────────────────────────────────────────────────────
static bool sps30CheckStatus() {
    uint32_t st = 0;
    if (sps30.readDeviceStatusRegister(st) != NO_ERROR) return true;
    if (st & SPS30_STATUS_LASER_ERROR) return false;
    if (st & SPS30_STATUS_FAN_ERROR)   return false;
    if (st != 0) sps30.clearDeviceStatusRegister();
    return true;
}

static bool sps30WaitReady() {
    uint16_t flag = 0;
    uint32_t deadline = millis() + MAX_READY_WAIT_MS;
    while (millis() < deadline) {
        if (sps30.readDataReadyFlag(flag) != NO_ERROR) return false;
        if (flag) return true;
        delay(50);
    }
    return false;
}

static bool sps30ReadMedian(float& pm1, float& pm25, float& pm4, float& pm10) {
    float raw[4][NUM_SAMPLES];
    float nc0p5, nc1, nc25, nc4, nc10, sz;

    if (++statusCheckCounter >= 5) {
        statusCheckCounter = 0;
        if (!sps30CheckStatus()) return false;
    }

    for (int i = 0; i < NUM_SAMPLES; i++) {
        if (!sps30WaitReady()) return false;
        err = sps30.readMeasurementValuesFloat(
            raw[0][i], raw[1][i], raw[2][i], raw[3][i],
            nc0p5, nc1, nc25, nc4, nc10, sz);
        if (err != NO_ERROR) return false;
        for (int c = 0; c < 4; c++) if (raw[c][i] < 0) raw[c][i] = 0;
    }

    float results[4];
    for (int c = 0; c < 4; c++) {
        float sorted[NUM_SAMPLES];
        memcpy(sorted, raw[c], NUM_SAMPLES * sizeof(float));
        insertionSort(sorted, NUM_SAMPLES);
        float clean[NUM_SAMPLES];
        int nClean = tukeyClean(sorted, NUM_SAMPLES, clean);
        results[c] = (nClean >= 3) ? clean[nClean/2] : sorted[NUM_SAMPLES/2];
    }

    pm1  = results[0]; pm25 = results[1];
    pm4  = results[2]; pm10 = results[3];
    return true;
}

// ── SCD40 helper ──────────────────────────────────────────────────────────────
static bool scd4xWaitReady() {
    bool flag = false;
    uint32_t deadline = millis() + 6000;
    while (millis() < deadline) {
        if (scd4x.getDataReadyStatus(flag) != NO_ERROR) return false;
        if (flag) return true;
        delay(100);
    }
    return false;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial) delay(100);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    WiFi.mode(WIFI_STA);   // connectWifi() below handles disconnect+begin itself
    WiFi.setSleep(false);  // modem-sleep power saving is what causes the ESP32 to silently
                            // drop off WiFi after long uptimes on some routers; disabling it
                            // trades a bit of power draw for actually staying connected

    initChipId();
    Serial.printf("Chip ID: %s (must have a matching chip_id set on its row in the machines table)\n", g_chipId);
    Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

    // Announce whether this boot is a freshly-OTA'd image still on probation.
    // If it is and the device reboots before confirmFirmwareIfHealthy()
    // succeeds, the bootloader reverts to the previous version.
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            Serial.println("[OTA] Running a PENDING_VERIFY image — will confirm once WiFi + machine_id are up.");
        }
    }

    loadWifiCreds();  // NVS-stored remote config if one was ever applied, else the fallback default
    Serial.printf("Connecting to \"%s\"", g_wifiSsid);
    bool wifiOk = connectWifiAtBoot(15000);

    if (wifiOk) {
        digitalWrite(LED_PIN, HIGH);
        Serial.printf("\nConnected — IP: %s\n", WiFi.localIP().toString().c_str());

        configTime(19800, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
        Serial.print("NTP sync");
        struct tm t;
        uint32_t ntpDeadline = millis() + 8000;
        while (!getLocalTime(&t, 1000) && millis() < ntpDeadline) Serial.print(".");
        if (getLocalTime(&t, 0))
            Serial.printf("\nTime: %04d-%02d-%02d %02d:%02d:%02d IST\n",
                t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
        else
            Serial.println("\nNTP unavailable — Supabase uses server time, continuing...");

        // Must resolve machine_id before checking machine_wifi, since that
        // table is looked up by machine_id, not by machine_code.
        for (int attempt = 0; attempt < 5 && g_machineId[0] == '\0'; attempt++) {
            if (resolveMachineId()) break;
            delay(2000);
        }

        // One-time-per-boot check: has the admin/customer pushed a new WiFi
        // config from the website since we last applied one? See
        // DeviceConfig.cpp for why this can never brick the device on a bad
        // password.
        checkRemoteWifiConfig();

        reportFirmwareVersion();
    } else {
        digitalWrite(LED_PIN, LOW);
        Serial.printf("\nWiFi FAILED (status=%d)\n", WiFi.status());
    }

    // Standard ESP32 I2C pinout: SDA=21, SCL=22. Both boards are wired to
    // this same pinout, which is required for one shared binary (chip-ID
    // identity + no per-board pin override) to be OTA-safe for the fleet.
    Wire.begin(21, 22);
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setTimeOut(1000);   // caps any single I2C transaction at 1s instead of
                             // letting an unresponsive sensor hang forever - without
                             // this, sps30WaitReady()'s own timeout loop never gets
                             // a chance to run because it's stuck inside a Wire call
                             // that never returns

    sps30.begin(Wire, SPS30_I2C_ADDR_69);
    sps30.deviceReset();
    delay(500);   // datasheet requires >=100ms after reset before next command; kept a safe margin
    sps30.writeAutoCleaningInterval(0);
    sps30.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);

    // Every call here can fail if the SCD40 isn't actually responding on the
    // bus (no power, bad connector) - none of these were checked before, so
    // a dead sensor failed completely silently and just left co2/temp/
    // humidity at 0 forever with no indication why. Now it's loud about it.
    scd4x.begin(Wire, SCD40_I2C_ADDR_62);
    int16_t scdErr = scd4x.stopPeriodicMeasurement();
    if (scdErr != NO_ERROR) {
        Serial.printf("[SCD40] stopPeriodicMeasurement FAILED (err=%d) — sensor not responding on I2C. Check its power/GND/connector.\n", scdErr);
    }
    delay(500);
    scd4x.setTemperatureOffset(TEMP_OFFSET_C);
    scd4x.setAutomaticSelfCalibrationEnabled(0);  // ASC off — indoor use, never sees 400ppm fresh air
    scd4x.persistSettings();               // save to EEPROM (~800ms)
    delay(1000);
    scdErr = scd4x.startPeriodicMeasurement();
    if (scdErr != NO_ERROR) {
        Serial.printf("[SCD40] startPeriodicMeasurement FAILED (err=%d) — CO2/temp/humidity will read 0 until this is fixed.\n", scdErr);
    } else {
        Serial.println("[SCD40] Periodic measurement started OK.");
    }

    Serial.print("Warming up");
    for (int i = 0; i < WARMUP_SECONDS; i++) { delay(1000); Serial.print("."); }
    Serial.println(" READY");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    // No reconnect logic existed before this - once WiFi dropped (router
    // reboot, DHCP lease expiry, AP idle-kick), the device stayed offline
    // silently until physically power-cycled. Retry on a cooldown so a
    // transient drop self-heals instead of needing manual intervention.
    //
    // Suspended entirely while the setup portal is open. This guard used to be
    // !portalHasClient() - "don't interrupt someone mid-form" - which was a
    // deadlock: nobody can connect to the AP until they can SEE it, so the
    // guard never fired, and this ran every 30s while the portal was up.
    // connectWifi() disconnects the radio and blocks for 15s, so the soft AP
    // was being torn down and channel-hopped for 15 out of every 30 seconds.
    // Confirmed live on 2026-08-18: a genuinely stranded board never showed a
    // usable "ZyGreen-XXXXXX" network for 20+ minutes. Retrying the saved
    // network is now the portal's own job, on its own schedule - see below.
    static unsigned long lastWifiRetry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= 30000 &&
        !portalIsActive()) {
        lastWifiRetry = millis();
        Serial.println("[WIFI] Disconnected — attempting reconnect...");
        if (connectWifi(g_wifiSsid, g_wifiPass, 15000)) {
            Serial.printf("[WIFI] Reconnected — IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[WIFI] Reconnect attempt failed — will retry in 30s.");
        }
    }

    // ── Offline recovery ─────────────────────────────────────────────────────
    // The retry above can only ever try the credentials we already have, and
    // new ones normally arrive FROM the dashboard - over the very network that
    // just changed. A customer who swaps their router or password without
    // updating the dashboard first was therefore unrecoverable without a USB
    // reflash. Publishing our own AP after a sustained outage gives them a way
    // in. It closes itself the moment the saved network works again, so a
    // rebooting router never needs anyone's attention. See WifiPortal.h.
    static unsigned long offlineSince = 0;
    if (WiFi.status() == WL_CONNECTED) {
        offlineSince = 0;
        if (portalIsActive()) {
            Serial.println("[PORTAL] Back online — closing the setup network.");
            portalEnd();
        }
    } else if (offlineSince == 0) {
        offlineSince = millis();
    } else if (!portalIsActive() && millis() - offlineSince >= PORTAL_TRIGGER_MS) {
        portalBegin();
    }

    if (portalIsActive()) {
        // Because the 30s retry above is suspended, the portal owns the job of
        // noticing that the saved network came back. It can't just call
        // connectWifi() with the AP still up - that's the bug above - so it
        // takes the AP down, tries once, and puts it back only if that failed.
        // Skipped while a client is associated: someone is mid-form and pulling
        // the network out from under them is worse than a slower recovery.
        static unsigned long lastPortalRetry = 0;
        if (!portalHasClient() && millis() - lastPortalRetry >= PORTAL_RETRY_INTERVAL_MS) {
            lastPortalRetry = millis();
            Serial.println("[PORTAL] Pausing the setup network to retry the saved one...");
            portalEnd();
            if (connectWifi(g_wifiSsid, g_wifiPass, 12000)) {
                Serial.printf("[PORTAL] Saved network is back — IP: %s\n",
                    WiFi.localIP().toString().c_str());
                offlineSince = 0;
            } else {
                Serial.println("[PORTAL] Still unreachable — reopening the setup network.");
                portalBegin();
            }
        }

        // Sensor reads block for seconds at a time (sps30WaitReady /
        // scd4xWaitReady), long enough that a phone's captive-portal browser
        // gives up on the page. Nothing is buffered while offline, so pausing
        // sampling until we're back online costs no data that would have
        // survived anyway.
        if (portalIsActive()) {
            portalHandle();
            portalLedTick();
            delay(2);
        }
        return;
    }

    float    pm[4] = {0, 0, 0, 0};
    uint16_t co2   = 0;
    float    temperature = 0, humidity = 0;

    // A failed/dead PM sensor used to `return` here, which skipped everything
    // below for the rest of loop() too - heartbeat, WiFi status, Supabase
    // upload, calibration poll, and the OTA check. That made one broken
    // sensor take the whole device off-grid (no heartbeat, never eligible
    // for an OTA fix) instead of just reporting degraded PM readings.
    bool pmOk = sps30ReadMedian(pm[0], pm[1], pm[2], pm[3]);
    if (!pmOk) {
        Serial.println("[SPS30] Read failed/timed out");
    } else {
        for (int c = 0; c < 4; c++)
            ema[c] = (ema[c] < 0.f) ? pm[c] : EMA_ALPHA * pm[c] + (1.0f - EMA_ALPHA) * ema[c];
    }

    static unsigned long lastScdErrLog = 0;
    if (scd4xWaitReady()) {
        err = scd4x.readMeasurement(co2, temperature, humidity);
        if (err != NO_ERROR) {
            co2 = 0;
            if (millis() - lastScdErrLog >= 30000) {
                lastScdErrLog = millis();
                Serial.printf("[SCD40] readMeasurement FAILED (err=%d)\n", err);
            }
        }
    } else if (millis() - lastScdErrLog >= 30000) {
        lastScdErrLog = millis();
        Serial.println("[SCD40] Not ready within timeout - check power/connector");
    }

    if (pmOk) for (int c = 0; c < 4; c++) g_pm[c] = (int)roundf(pm[c]);
    g_co2   = co2;
    g_temp  = temperature;
    g_humid = humidity;

    static unsigned long lastResolveAttempt = 0;
    if (g_machineId[0] == '\0' && WiFi.status() == WL_CONNECTED &&
        millis() - lastResolveAttempt >= 30000) {
        lastResolveAttempt = millis();
        resolveMachineId();
    }

    // Apply a WiFi change made on the dashboard WITHOUT needing a power-cycle.
    // Same safety as at boot: the new credentials are tried, and kept only if
    // they actually connect - a typo on the website falls back to the network
    // we're already on rather than stranding the unit.
    static unsigned long lastWifiConfigPoll = 0;
    if (WiFi.status() == WL_CONNECTED && g_machineId[0] != '\0' &&
        millis() - lastWifiConfigPoll >= WIFI_CONFIG_POLL_MS) {
        lastWifiConfigPoll = millis();
        checkRemoteWifiConfig();
    }

    static unsigned long lastStatusReport = 0;
    if (lastStatusReport == 0 || millis() - lastStatusReport >= STATUS_REPORT_INTERVAL_MS) {
        lastStatusReport = millis();
        // Reflects BOTH sensors. This used to be `co2 > 0 ? "OK" : "Degraded"`,
        // which ignored the PM sensor entirely - so a dead/unplugged SPS30
        // reported "OK" indefinitely while its PM readings sat frozen, with
        // nothing on the dashboard hinting anything was wrong.
        const char* sensorStatus;
        if (co2 > 0 && pmOk)        sensorStatus = "OK";
        else if (co2 > 0 || pmOk)   sensorStatus = "Degraded";   // exactly one sensor alive
        else                        sensorStatus = "Error";      // neither responding
        reportMachineHeartbeat(sensorStatus);
        reportWifiStatus();
    }

    static unsigned long lastUpload = 0;
    if (lastUpload == 0 || millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
        lastUpload = millis();
        pushToSupabase();
    }

    static unsigned long lastCalPoll = 0;
    if (lastCalPoll == 0 || millis() - lastCalPoll >= 5UL * 60UL * 1000UL) {
        lastCalPoll = millis();
        checkCalibrationRequest();
    }

    // Must run BEFORE checkForOtaUpdate(): confirming the current image is
    // what makes the previous partition free to be overwritten by the next
    // update, and we never want to stack a new OTA on top of an unverified one.
    confirmFirmwareIfHealthy();

    static unsigned long lastOtaCheck = 0;
    if (lastOtaCheck == 0 || millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
        lastOtaCheck = millis();
        checkForOtaUpdate();
    }

    digitalWrite(LED_PIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

    // Same backoff as before on a dead/failed PM sensor - just moved to the
    // end so it no longer skips heartbeat/upload/OTA on the way here.
    if (!pmOk) delay(3000);
}
