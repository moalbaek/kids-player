#pragma once
#include <stdbool.h>

void wifi_init(void);

// Connect as a station. Keeps retrying in the background after the timeout. Returns true once connected.
bool wifi_connect(const char *ssid, const char *pass, int timeout_ms);
bool wifi_is_connected(void);
void wifi_ip(char *out, int len);  // "" if not connected

// Setup access point with captive-portal DNS (station keeps running if configured).
void wifi_start_ap(char *ssid_out, int ssid_len, char *pass_out, int pass_len);
void wifi_stop_ap(void);
bool wifi_ap_running(void);

// Scan nearby networks (unique SSIDs, strongest first).
int wifi_scan(char ssids[][33], int max);
