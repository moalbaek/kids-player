#pragma once
#include <stdbool.h>

// Start the HTTP server on port 80 (settings page, Spotify login relay, Wi-Fi setup portal).
void web_start(void);

// Networks shown on the Wi-Fi setup page.
void web_set_scan_results(char ssids[][33], int n);

// While true, every unknown URL redirects to the setup page (captive portal).
void web_set_portal_mode(bool on);
