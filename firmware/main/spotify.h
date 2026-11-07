#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "spotify_parse.h"

typedef enum {
    SP_OK = 0,
    SP_ERR_NETWORK,     // could not reach Spotify
    SP_ERR_LOGGED_OUT,  // no or expired login: show the login QR
    SP_ERR_OTHER,
} sp_result_t;

// Call once at start-up, before other tasks use this module.
void spotify_init(void);

bool spotify_logged_in(void);
void spotify_logout(void);

// Login (PKCE). Builds the authorize URL for a phone that opened http://<device_ip>/login.
// Returns false if the URL does not fit.
bool spotify_begin_login(const char *device_ip, char *url, size_t urllen);

// Called from /callback. Verifies state and exchanges the code for tokens.
sp_result_t spotify_finish_login(const char *code, const char *state);

// Kids mode: playlists whose name starts with the configured prefix (prefix removed).
// Full library mode: all of the user's own and followed playlists.
sp_result_t spotify_get_playlists(sp_playlist_t *out, int max, int *count);

// Saved albums, Liked Songs, songs of an album, and search (songs, albums, playlists).
sp_result_t spotify_get_saved_albums(sp_entry_t *out, int max, int *count);
sp_result_t spotify_get_liked(sp_entry_t *out, int max, int *count);
sp_result_t spotify_get_album_tracks(const char *album_id, const char *album_img, sp_entry_t *out, int max,
                                     int *count);
sp_result_t spotify_search(const char *query, sp_entry_t *out, int max, int *count);

// Songs of a playlist (playable Spotify tracks only, in playlist order).
sp_result_t spotify_get_tracks(const char *playlist_id, sp_track_t *out, int max, int *count);

// Current user's display name (for the settings page).
sp_result_t spotify_get_display_name(char *out, size_t len);
