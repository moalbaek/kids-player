// Spotify Web API JSON parsing (pure C + cJSON, unit-testable on a PC).
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_ID_LEN 23
#define SP_NAME_LEN 96
#define SP_ARTIST_LEN 64
#define SP_IMG_LEN 256

typedef enum {
    SP_KIND_PLAYLIST = 0,
    SP_KIND_ALBUM,
    SP_KIND_TRACK,
    SP_KIND_LIKED,   // "Liked Songs" card (full library mode)
    SP_KIND_SEARCH,  // "Search" card (full library mode)
} sp_kind_t;

// One card on screen: a playlist, album, song, or one of the special cards.
typedef struct {
    uint8_t kind;               // sp_kind_t
    bool can_list;              // playlists: Spotify lets us read the songs (owned or collaborative)
    int16_t track_no;           // songs: track number on its album (1-based, per disc)
    char id[SP_ID_LEN];
    char album_id[SP_ID_LEN];   // songs: the album it is on
    char name[SP_NAME_LEN];     // playlists in kids mode: with the prefix removed
    char artist[SP_ARTIST_LEN]; // songs and albums: first artist
    char img[SP_IMG_LEN];       // image closest to 300 px
} sp_entry_t;

typedef sp_entry_t sp_playlist_t;
typedef sp_entry_t sp_track_t;

// Page of GET /me/playlists. If prefix is non-empty, keeps only names starting with it (case-insensitive) and
// removes it; an empty prefix keeps everything. me_id (may be empty) decides can_list: owned or collaborative.
// Appends to out[*count..max). Copies the "next" URL (or "") into next. Returns false on malformed JSON.
bool sp_parse_playlists(const char *json, const char *prefix, const char *me_id, sp_entry_t *out, int *count,
                        int max, char *next, size_t nextlen);

// Page of GET /playlists/{id}/items or GET /me/tracks (accepts the new "item" and the old "track" field).
// Skips local files, episodes and unavailable entries. *raw_items receives the number of entries in the page.
bool sp_parse_tracks(const char *json, sp_entry_t *out, int *count, int max, char *next, size_t nextlen,
                     int *raw_items);

// Page of GET /albums/{id}/tracks (simplified tracks without images: album_img is used for all of them).
bool sp_parse_album_tracks(const char *json, const char *album_id, const char *album_img, sp_entry_t *out,
                           int *count, int max, char *next, size_t nextlen);

// Page of GET /me/albums.
bool sp_parse_saved_albums(const char *json, sp_entry_t *out, int *count, int max, char *next, size_t nextlen);

// GET /search?type=track,album,playlist: songs first, then albums, then playlists.
bool sp_parse_search(const char *json, const char *me_id, sp_entry_t *out, int *count, int max);

// Parse a token response. Returns false if access_token is missing. refresh may be left empty.
bool sp_parse_token(const char *json, char *access, size_t alen, char *refresh, size_t rlen, int *expires_in);

// Parse an error response {"error":"invalid_grant",...} or {"error":{"status":401,"message":...}}.
void sp_parse_error(const char *json, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
