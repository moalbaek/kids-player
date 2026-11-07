// Read-only snapshot of app data for the web pages.
#pragma once
#include "config.h"
#include "sonos_proto.h"

typedef struct {
    sonos_room_t rooms[KP_MAX_ROOMS];
    int nrooms;
    int nplaylists;
    char spotify_user[64];
} app_snapshot_t;

void app_get_snapshot(app_snapshot_t *out);
