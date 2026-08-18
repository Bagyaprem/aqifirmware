#pragma once

#include <Arduino.h>

// ── Offline recovery portal ──────────────────────────────────────────────────
// Closes the one hole that could strand a unit permanently: WiFi credentials
// are normally pushed FROM the dashboard (machine_wifi -> checkRemoteWifiConfig),
// which requires the device to already be online. A customer who swaps their
// router, or changes their SSID/password without updating the dashboard first,
// therefore takes the device offline with no way back - the new config can only
// be delivered over the very network that just disappeared. Recovery was a USB
// reflash, i.e. a site visit.
//
// After PORTAL_TRIGGER_MS with no connection, the device publishes its own
// WPA2 access point and serves a small captive-portal page on which the
// customer types the new network name and password directly. On success the
// credentials are persisted to NVS and the device reboots into them; the
// dashboard is re-synced from the device by the regular reportWifiStatus()
// heartbeat within 30s of coming back.
//
// The portal closes itself the moment the saved network becomes reachable
// again, so a router that was merely rebooting never needs anyone's attention.

// How long the device must be continuously offline before it opens the portal.
// Long enough not to react to a router reboot, short enough that a customer
// standing in front of a dead unit doesn't give up first.
#define PORTAL_TRIGGER_MS (3UL * 60UL * 1000UL)

// How often, while the portal is open and nobody is using it, to take the AP
// down and check whether the saved network has come back. Each attempt makes
// the setup network vanish for up to 12s, so this can't be aggressive - the
// plain 30s station retry is suspended while the portal is up precisely
// because it made the AP unfindable.
//
// Started at 5 minutes, which was too slow to live with: switch a router or
// hotspot back on and the device ignores it for up to five minutes, looking
// broken. 2 minutes keeps the AP present ~90% of the time while making
// recovery feel automatic.
#define PORTAL_RETRY_INTERVAL_MS (2UL * 60UL * 1000UL)

// WPA2 password for the setup network itself. Must be >= 8 characters. This
// is printed on the device label and in the manual.
//
// It is deliberately the same on every unit, which is the simple choice, not
// the strongest one: anyone within radio range of an OFFLINE unit could join
// its setup AP and point it at another network. They gain no access to data
// (the portal never displays the stored password and exposes no readings), so
// the worst case is a nuisance. For a per-device secret instead, derive it
// from g_chipId here and print that on each unit's label.
#define AP_SETUP_PASSWORD "zygreen-setup"

// Brings up the AP + DNS + web server. Safe to call when already active.
void portalBegin();

// Tears everything down and returns the radio to plain station mode.
void portalEnd();

bool portalIsActive();

// True while at least one client is associated with the setup AP - i.e.
// somebody is probably mid-way through the form and must not be interrupted
// by a blocking reconnect attempt.
bool portalHasClient();

// Services DNS + HTTP and drives the connect attempt. Call frequently.
void portalHandle();

// Non-blocking double-blink, visually distinct from the plain "searching"
// blink, so "needs setup" is distinguishable from "still trying".
void portalLedTick();
