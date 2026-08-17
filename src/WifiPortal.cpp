#include "WifiPortal.h"
#include "DeviceConfig.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WebServer server(80);
static DNSServer dns;

static bool g_active   = false;
static char g_apSsid[24] = {0};

// Cached scan taken once at portalBegin(), BEFORE the AP is up. Scanning
// while clients are associated briefly knocks them off, so the customer gets
// this snapshot plus an explicit "Rescan" button rather than a scan on every
// page load.
#define MAX_SCAN 20
static String g_nets[MAX_SCAN];
static int    g_netCount = 0;

enum PortalState : uint8_t { P_IDLE, P_CONNECTING, P_OK, P_FAILED };
static PortalState g_state = P_IDLE;
static String      g_pendSsid;
static String      g_pendPass;
static uint32_t    g_okAt = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────
static String esc(const String& in) {
    String out;
    out.reserve(in.length() + 12);
    for (unsigned int i = 0; i < in.length(); i++) {
        char c = in[i];
        if      (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '>')  out += "&gt;";
        else if (c == '"')  out += "&quot;";
        else                out += c;
    }
    return out;
}

static void runScan() {
    g_netCount = 0;
    int n = WiFi.scanNetworks(false, false);
    for (int i = 0; i < n && g_netCount < MAX_SCAN; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        bool dup = false;
        for (int j = 0; j < g_netCount; j++) {
            if (g_nets[j] == s) { dup = true; break; }
        }
        if (!dup) g_nets[g_netCount++] = s;
    }
    WiFi.scanDelete();
    Serial.printf("[PORTAL] Scan found %d network(s).\n", g_netCount);
}

static String pageOpen(const char* title) {
    String h = F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                 "<title>");
    h += title;
    h += F("</title><style>"
           "body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;"
           "background:#f7f8f6;color:#14201b;margin:0;padding:26px 18px;line-height:1.5}"
           ".w{max-width:420px;margin:0 auto}"
           "h1{font-size:21px;margin:0 0 2px;letter-spacing:-.02em}"
           ".sub{color:#5c6b64;font-size:13px;margin:0 0 20px}"
           ".card{background:#fff;border:1px solid #dde3df;border-radius:12px;padding:20px}"
           "label{display:block;font-size:11px;font-weight:700;text-transform:uppercase;"
           "letter-spacing:.09em;color:#5c6b64;margin:16px 0 6px}"
           "label:first-of-type{margin-top:0}"
           /* 16px keeps iOS from zooming the page when the field is focused */
           "input{width:100%;box-sizing:border-box;font-size:16px;padding:11px 12px;"
           "border:1px solid #c3cec8;border-radius:8px;background:#fff;color:#14201b}"
           "input:focus{outline:2px solid #1f6b4a;outline-offset:-1px;border-color:#1f6b4a}"
           "button{width:100%;margin-top:22px;font-size:16px;font-weight:600;padding:13px;"
           "border:0;border-radius:8px;background:#1f6b4a;color:#fff;cursor:pointer}"
           ".msg{padding:12px 14px;border-radius:8px;font-size:14px;margin-bottom:18px}"
           ".ok{background:#e4efe8;color:#16543a}"
           ".err{background:#fdecea;color:#8c2f27}"
           ".note{font-size:12.5px;color:#5c6b64;margin-top:16px}"
           "a{color:#16543a}"
           "code{font-family:ui-monospace,Consolas,monospace;font-size:12.5px}"
           "</style></head><body><div class=\"w\">"
           "<h1>ZyGreen</h1>");
    return h;
}

static String pageClose() {
    return F("</div></body></html>");
}

// ── Handlers ─────────────────────────────────────────────────────────────────
static void handleRoot() {
    String h = pageOpen("ZyGreen Wi-Fi Setup");
    h += F("<p class=\"sub\">Wi-Fi setup &middot; <code>");
    h += g_chipId;
    h += F("</code></p><div class=\"card\">");

    if (g_state == P_FAILED) {
        h += F("<div class=\"msg err\">Could not connect to <b>");
        h += esc(g_pendSsid);
        h += F("</b>. Check the password and that it is a 2.4 GHz network, then try again.</div>");
    }

    h += F("<form method=\"POST\" action=\"/save\">"
           "<label for=\"ssid\">Network name</label>"
           "<input id=\"ssid\" name=\"ssid\" list=\"nets\" maxlength=\"32\" required "
           "autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" "
           "placeholder=\"Your Wi-Fi name\">"
           "<datalist id=\"nets\">");
    for (int i = 0; i < g_netCount; i++) {
        h += F("<option value=\"");
        h += esc(g_nets[i]);
        h += F("\">");
    }
    h += F("</datalist>"
           "<label for=\"pass\">Password</label>"
           "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"64\" "
           "autocomplete=\"off\" placeholder=\"Network password\">"
           "<button type=\"submit\">Save &amp; Connect</button>"
           "</form>");

    h += F("<p class=\"note\">Currently trying: <b>");
    h += esc(String(g_wifiSsid));
    h += F("</b><br>2.4 GHz networks only &middot; <a href=\"/rescan\">Rescan</a></p>");
    h += F("</div>");
    h += pageClose();
    server.send(200, "text/html", h);
}

static void handleRescan() {
    runScan();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

static void handleSave() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    ssid.trim();

    if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
        String h = pageOpen("ZyGreen Wi-Fi Setup");
        h += F("<p class=\"sub\">Wi-Fi setup</p><div class=\"card\">"
               "<div class=\"msg err\">Enter a network name of 1&ndash;32 characters "
               "and a password of at most 64.</div>"
               "<p><a href=\"/\">Back</a></p></div>");
        h += pageClose();
        server.send(200, "text/html", h);
        return;
    }

    g_pendSsid = ssid;
    g_pendPass = pass;
    g_state    = P_CONNECTING;

    // The attempt itself blocks for up to 12s in portalHandle(), during which
    // nothing is served. Rather than let the phone's captive-portal browser
    // show a connection error and look broken, this page waits it out and
    // refreshes once, after the attempt has certainly finished.
    String h = pageOpen("Connecting");
    h += F("<meta http-equiv=\"refresh\" content=\"16;url=/status\">");
    h += F("<p class=\"sub\">Wi-Fi setup</p><div class=\"card\">"
           "<div class=\"msg ok\">Connecting to <b>");
    h += esc(ssid);
    h += F("</b>&hellip;</div><p class=\"note\">This takes about 15 seconds. "
           "Keep this page open &mdash; it will update by itself.</p></div>");
    h += pageClose();
    server.send(200, "text/html", h);
}

static void handleStatus() {
    if (g_state == P_CONNECTING) {
        String h = pageOpen("Connecting");
        h += F("<meta http-equiv=\"refresh\" content=\"5;url=/status\">");
        h += F("<p class=\"sub\">Wi-Fi setup</p><div class=\"card\">"
               "<div class=\"msg ok\">Still connecting&hellip;</div></div>");
        h += pageClose();
        server.send(200, "text/html", h);
        return;
    }

    if (g_state == P_OK) {
        String h = pageOpen("Connected");
        h += F("<p class=\"sub\">Wi-Fi setup</p><div class=\"card\">"
               "<div class=\"msg ok\"><b>Connected.</b></div>"
               "<p>The monitor is restarting and will be back on your dashboard "
               "in about a minute. This setup network is closing now &mdash; your "
               "phone will rejoin your normal Wi-Fi on its own.</p>"
               "<p class=\"note\">The indicator light will stop blinking and stay "
               "on once it is running.</p></div>");
        h += pageClose();
        server.send(200, "text/html", h);
        return;
    }

    // P_FAILED / P_IDLE - send them back to the form, which renders the error.
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

// Everything else (including the OS connectivity-check URLs that make a phone
// pop up the "Sign in to network" notification) is bounced to the form.
static void handleNotFound() {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
void portalBegin() {
    if (g_active) return;

    // g_chipId is the 12-char MAC; its last 6 are enough to tell two units
    // apart on a shelf and match the label.
    snprintf(g_apSsid, sizeof(g_apSsid), "ZyGreen-%s", g_chipId + 6);

    Serial.printf("\n[PORTAL] Offline for %lus and \"%s\" is unreachable.\n",
        (unsigned long)(PORTAL_TRIGGER_MS / 1000), g_wifiSsid);

    WiFi.mode(WIFI_AP_STA);
    runScan();                       // before softAP() - no clients to disturb yet
    WiFi.softAP(g_apSsid, AP_SETUP_PASSWORD);
    delay(200);                      // softAP needs a moment before softAPIP() is valid

    IPAddress ip = WiFi.softAPIP();
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", ip);          // wildcard: every lookup resolves to us

    server.on("/",       HTTP_GET,  handleRoot);
    server.on("/save",   HTTP_POST, handleSave);
    server.on("/status", HTTP_GET,  handleStatus);
    server.on("/rescan", HTTP_GET,  handleRescan);
    server.onNotFound(handleNotFound);
    server.begin();

    g_active = true;
    g_state  = P_IDLE;

    Serial.printf("[PORTAL] Setup network \"%s\" is open (password: %s).\n",
        g_apSsid, AP_SETUP_PASSWORD);
    Serial.printf("[PORTAL] Join it, then open http://%s/\n", ip.toString().c_str());
}

void portalEnd() {
    if (!g_active) return;
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_active = false;
    g_state  = P_IDLE;
    Serial.println("[PORTAL] Setup network closed.");
}

bool portalIsActive() { return g_active; }

bool portalHasClient() { return g_active && WiFi.softAPgetStationNum() > 0; }

void portalHandle() {
    if (!g_active) return;

    dns.processNextRequest();
    server.handleClient();

    if (g_state == P_CONNECTING) {
        Serial.printf("[PORTAL] Trying \"%s\" from the setup form...\n", g_pendSsid.c_str());
        if (connectWifi(g_pendSsid.c_str(), g_pendPass.c_str(), 12000)) {
            // appliedAt is deliberately empty: these credentials came from the
            // device, not from a machine_wifi row, so there is no updated_at to
            // record. markPortalProvisioned() then stops the NEXT boot's
            // checkRemoteWifiConfig() from overwriting them with the stale
            // dashboard entry that caused this outage in the first place.
            saveWifiCreds(g_pendSsid.c_str(), g_pendPass.c_str(), "");
            markPortalProvisioned();
            snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", g_pendSsid.c_str());
            snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", g_pendPass.c_str());
            g_state = P_OK;
            g_okAt  = millis();
            Serial.printf("[PORTAL] Connected to \"%s\" — saved. Restarting shortly.\n",
                g_pendSsid.c_str());
        } else {
            g_state = P_FAILED;
            Serial.printf("[PORTAL] Could not connect to \"%s\".\n", g_pendSsid.c_str());
        }
    }

    // Reboot rather than carry on in place: a clean boot re-runs NTP, the
    // machine_id resolve and the firmware-version report, and drops the AP.
    // The delay is only so the success page is definitely delivered first.
    if (g_state == P_OK && millis() - g_okAt >= 4000) {
        Serial.println("[PORTAL] Restarting into the new network.");
        Serial.flush();
        portalEnd();
        delay(100);
        ESP.restart();
    }
}

void portalLedTick() {
    // Double-blink every 2s: distinct from connectWifi()'s even 300ms blink,
    // so "waiting for someone to set me up" doesn't look like "still trying".
    uint32_t t = millis() % 2000;
    bool on = (t < 110) || (t >= 300 && t < 410);
    digitalWrite(LED_PIN, on ? HIGH : LOW);
}
