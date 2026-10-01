// Copy this file to wifi_secrets.h and fill in your network. wifi_secrets.h is
// git-ignored, so the password never ends up in the repository.
// Without wifi_secrets.h the board skips the router and opens its own hotspot.
#pragma once

#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"

// Optional: a fixed address, so the dashboard URL never changes.
// #define WIFI_STATIC_IP 192,168,1,50
// #define WIFI_GATEWAY   192,168,1,1
// #define WIFI_SUBNET    255,255,255,0

// Optional: password for the board's own hotspot (8 characters or more).
// #define HOTSPOT_PASS "cuffless-bp"
