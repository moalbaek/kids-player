// Sonos UPnP message building and parsing (pure C, unit-testable on a PC).
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char uid[48];        // RINCON_xxxx of this room's visible speaker
    char name[64];       // room name, e.g. "Bathroom"
    char ip[16];         // this speaker's IP
    char coord_uid[48];  // group coordinator (leader) for playback commands
    char coord_ip[16];
} sonos_room_t;

typedef struct {
    const char *key;
    const char *value;  // escaped automatically
} sonos_arg_t;

// Build a SOAP envelope. Returns body length, or 0 if it did not fit.
size_t sonos_build_envelope(char *out, size_t outlen, const char *service, const char *action,
                            const sonos_arg_t *args, int nargs);

// Parse the *unescaped* ZoneGroupState XML into rooms (invisible bonded speakers skipped, names de-duplicated).
int sonos_parse_topology(const char *zgs, sonos_room_t *rooms, int max);

// Find a Spotify service id (sid=N) in unescaped DIDL text containing x-sonos-spotify URIs. Returns -1 if none.
int sonos_parse_spotify_sid(const char *didl);

// Extract the 22-char Spotify track id from a Sonos TrackURI. Returns false if not a Spotify track.
bool sonos_track_id_from_uri(const char *uri, char out[23]);

// Account serial (sn=N) used with that sid in unescaped DIDL text. Returns -1 if none.
int sonos_parse_spotify_sn(const char *didl, int sid);

typedef enum { SONOS_SP_PLAYLIST = 0, SONOS_SP_ALBUM, SONOS_SP_TRACK } sonos_sp_kind_t;

// Enqueue URI for a Spotify playlist, album or song. Songs use the x-sonos-spotify form when sid and sn are
// known (> 0), otherwise the bare "spotify%3atrack%3a<id>" form that Sonos resolves from the metadata.
size_t sonos_spotify_uri(char *out, size_t outlen, sonos_sp_kind_t kind, const char *id, int sid, int sn);

// DIDL-Lite metadata for the same (service type e.g. 2311 or 3079).
size_t sonos_spotify_didl(char *out, size_t outlen, sonos_sp_kind_t kind, const char *id, int service_type);

// Parse a UPnP fault and return the errorCode (e.g. 800), or -1.
int sonos_fault_code(const char *xml);

// Position (1-based track number) of a Spotify song in an unescaped DIDL-Lite listing of the queue (Browse Q:0),
// using the item ids "Q:0/<n>". Returns -1 if the song is not in the listing.
int sonos_queue_position(const char *didl, const char *track_id);

#ifdef __cplusplus
}
#endif
