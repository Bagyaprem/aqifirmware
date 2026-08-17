#pragma once

#include <Arduino.h>

// ── WiFi + machine (machine_id) allocation ──────────────────────────────────
// Everything needed to figure out "who is this board" (resolve its chip_id
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
#define WIFI_FORCE_RESET_TAG "reset-2"

extern char g_machineId[40];   // UUID resolved from g_chipId; empty until resolveMachineId() succeeds
extern char g_wifiSsid[33];
extern char g_wifiPass[65];

// Stable per-board identity derived from the ESP32's own hardware MAC (see
// initChipId() in DeviceConfig.cpp) - NOT a compile-time constant, so the
// same compiled binary is correct on every physical board. This is what
// resolveMachineId() sends to resolve_machine_id_by_chip(); the matching
// `machines.chip_id` row must already exist (one-time provisioning: flash
// once over USB, read this value off Serial Monitor, set it in Supabase).
extern char g_chipId[13];

// Populates g_chipId from WiFi.macAddress(). Requires WiFi.mode() to have
// already been called (doesn't require an actual connection).
void initChipId();

// Loads whatever WiFi config is saved in NVS (or the hardcoded default if
// none has ever been applied) into g_wifiSsid/g_wifiPass.
void loadWifiCreds();

// Blocking connect attempt with LED heartbeat; returns true on success.
bool connectWifi(const char* ssid, const char* pass, uint32_t timeoutMs);

// Call this instead of connectWifi(g_wifiSsid, g_wifiPass, ...) at boot.
// Handles a pending WIFI_FORCE_RESET_TAG change safely: tests the new
// hardcoded default first, only commits to wiping the saved network if that
// default actually connects, and otherwise falls back to the still-intact
// saved config instead of stranding the device. See DeviceConfig.cpp for
// the incident that made this necessary.
bool connectWifiAtBoot(uint32_t timeoutMs);

// Resolves g_chipId to its machines.id UUID via the resolve_machine_id_by_chip
// RPC and stores it in g_machineId. Requires WiFi to already be connected.
// Returns true on success; safe to call again later if it failed before.
bool resolveMachineId();

// Boot-once: checks Supabase's machine_wifi table (filtered by g_machineId)
// for a config newer than what we last applied, and switches to it
// (persisting to NVS) only if it actually connects. Requires g_machineId to
// already be resolved. See DeviceConfig.cpp for the full rationale.
void checkRemoteWifiConfig();

// Persists a WiFi config to NVS so it survives power loss. `appliedAt` is the
// machine_wifi.updated_at the config came from, or "" when it came from the
// on-device setup portal (which has no dashboard row behind it).
void saveWifiCreds(const char* ssid, const char* pass, const char* appliedAt);

// Sets a one-shot NVS flag telling the NEXT boot to skip checkRemoteWifiConfig().
// Needed after the setup portal provisions the device: machine_wifi still holds
// the stale credentials that caused the outage, and applying them again would
// waste a 15s failed connect on every boot until the dashboard catches up.
// The regular reportWifiStatus() heartbeat re-syncs the dashboard within 30s.
void markPortalProvisioned();
