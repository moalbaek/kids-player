#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "sonos_proto.h"

typedef struct {
    bool playing;
    char track_id[23];  // Spotify id of the current track, "" if none/other source
    int volume;         // -1 if unknown
} sonos_status_t;

// Find rooms: SSDP multicast, falling back to the saved speaker IP. Returns number of rooms.
int sonos_discover(sonos_room_t *rooms, int max);

// Replace the chosen room's queue with a Spotify playlist or album and play it from song index (0-based).
// If track_id is given, checks that this song is the one selected and looks it up in the queue if not.
esp_err_t sonos_play_container(sonos_sp_kind_t kind, const char *id, int index, const char *track_id);

// Replace the queue with these songs (ids[0] plays first; the rest are queued after it starts).
esp_err_t sonos_play_tracks(const char *const *ids, int n);

esp_err_t sonos_toggle_play(void);

// Change volume by delta (capped by settings). Returns the new volume in *vol.
esp_err_t sonos_change_volume(int delta, int *vol);

esp_err_t sonos_get_status(sonos_status_t *st);

// Forget the cached group leader (after room change).
void sonos_reset_cache(void);
