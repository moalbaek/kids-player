#pragma once
#include "sdkconfig.h"

// Spotify app (Development Mode). The client ID is public; no client secret is used (PKCE).
#define KP_SPOTIFY_CLIENT_ID "18951081b4274207b0fa74ed95ede053"
#define KP_REDIRECT_URI "https://moalbaek.github.io/kids-player/callback.html"
#define KP_SPOTIFY_SCOPES "playlist-read-private playlist-read-collaborative user-library-read"

// Firmware updates are published on the GitHub Pages site.
#define KP_OTA_BASE "https://moalbaek.github.io/kids-player/fw/" CONFIG_KP_BOARD_NAME "/"

#define KP_DEFAULT_PREFIX "Kids:"
#define KP_MAX_HOME 100      // cards on the home screen (playlists, albums, Search, Liked Songs)
#define KP_MAX_TRACKS 120    // songs shown for one playlist/album
#define KP_MAX_RESULTS 30    // search: up to 10 songs, 10 albums, 10 playlists
#define KP_MAX_ROOMS 16
#define KP_LIKED_QUEUE 25    // Liked Songs: how many songs are queued from the one tapped

#if CONFIG_KP_BOARD_CUBE
#define KP_SCREEN_W 480
#define KP_SCREEN_H 480
#define KP_COVER 300        // cover size in pixels (decoded from Spotify's 300 px image)
#define KP_IMG_CACHE 14     // decoded covers kept in PSRAM (180 KB each)
#define KP_DEFAULT_FULL_LIBRARY 0  // the cube is the kids' player: only "Kids:" playlists
#define KP_DEFAULT_MAX_VOL 40
#else
#define KP_SCREEN_W 640
#define KP_SCREEN_H 172
#define KP_COVER 150        // 300 px image decoded at 1:2
#define KP_IMG_CACHE 30     // 45 KB each
#define KP_DEFAULT_FULL_LIBRARY 1  // the strip is the grown-up's player: whole library + search
#define KP_DEFAULT_MAX_VOL 100
#endif
