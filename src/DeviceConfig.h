#pragma once

#include <Arduino.h>

// ── WiFi + machine (machine_id) allocation ──────────────────────────────────
// Everything needed to figure out "who is this board" (resolve MACHINE_CODE
// to its machines.id UUID) and "how does it get online" (WiFi creds, with
// remote-config support from the machine_wifi table) lives here.

#define LED_PIN 2

// Fallback used only when NVS has never received a remote config (i.e. a
// brand-new board, or one that's never been assigned WiFi from the
// Settings page). Once a remote config is applied via machine_wifi, THAT is
// what persists across reboots from then on - see
// loadWifiCreds()/checkRemoteWifiConfig() in DeviceConfig.cpp.
#define WIFI_SSID "Prem"
#define WIFI_PASS "987654321"

// Bump this string any time you need this firmware to forcibly wipe
// whatever WiFi config is currently saved in NVS and go back to using
// WIFI_SSID/WIFI_PASS above - e.g. after re-provisioning the router. Once
// applied, remote config from the Supabase machine_wifi table can still take
// over normally again; this only forces a one-time reset per unique tag.
#define WIFI_FORCE_RESET_TAG "reset-1"

// The machine_code of the row already created in the `machines` table
// (id, customer_id, machine_name, machine_code, ...) for THIS physical unit.
// There's no self-registration RPC in this schema - a matching row must
// already exist - so firmware resolves this code to the machine's real UUID
// (g_machineId) via the resolve_machine_id RPC at boot. Every other table
// (sensor_data, machine_wifi, machine_commands, ...) is keyed off that UUID,
// never off this code directly.
//
// PLACEHOLDER: "AQM-001" - insert a matching row in `machines` before
// flashing, or change this to whatever machine_code was actually assigned.
#define MACHINE_CODE "AQM-002"

extern char g_machineId[40];   // UUID resolved from MACHINE_CODE; empty until resolveMachineId() succeeds
extern char g_wifiSsid[33];
extern char g_wifiPass[65];

// Loads whatever WiFi config is saved in NVS (or the hardcoded default if
// none has ever been applied) into g_wifiSsid/g_wifiPass.
void loadWifiCreds();

// Blocking connect attempt with LED heartbeat; returns true on success.
bool connectWifi(const char* ssid, const char* pass, uint32_t timeoutMs);

// Resolves MACHINE_CODE to its machines.id UUID via the resolve_machine_id
// RPC and stores it in g_machineId. Requires WiFi to already be connected.
// Returns true on success; safe to call again later if it failed before.
bool resolveMachineId();

// Boot-once: checks Supabase's machine_wifi table (filtered by g_machineId)
// for a config newer than what we last applied, and switches to it
// (persisting to NVS) only if it actually connects. Requires g_machineId to
// already be resolved. See DeviceConfig.cpp for the full rationale.
void checkRemoteWifiConfig();
