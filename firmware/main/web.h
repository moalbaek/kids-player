#pragma once
#include <stdbool.h>
#include <stddef.h>

// Start the HTTP server on port 80 (settings page, Spotify login relay, Wi-Fi setup portal).
void web_start(void);

// Networks shown on the Wi-Fi setup page.
void web_set_scan_results(char ssids[][33], int n);

// While true, every unknown URL redirects to the setup page (captive portal).
void web_set_portal_mode(bool on);

// Create a settings session or a Spotify-only login link for the QR code on screen.
void web_open_settings(char *url, size_t len);
void web_close_settings(void);
bool web_settings_open(void);  // a settings link exists and has not expired
void web_open_login(char *url, size_t len);
void web_close_login(void);
