#pragma once
#include <stdbool.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char refresh_token[512];
    char speaker_uid[48];    // chosen Sonos room (its visible speaker)
    char speaker_name[64];
    char speaker_ip[16];     // last known IP; also used when discovery is blocked between VLANs
    int service_type;        // Sonos Spotify service type (sid*256+7); 0 = detect automatically
    char prefix[32];         // playlists starting with this are shown
    int max_vol;             // volume cap for the buttons
    int flip;                // strip: rotate the picture 180 degrees
    int swap_buttons;        // cube: swap volume up/down buttons
    int full_library;        // 1 = all playlists, saved albums, Liked Songs and search; 0 = prefix playlists only
} kp_settings_t;

extern kp_settings_t g_set;

void settings_init(void);   // nvs_flash_init + load
void settings_save(void);
void settings_lock(void);
void settings_unlock(void);
