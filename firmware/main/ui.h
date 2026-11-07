// Screens. All functions take the LVGL lock themselves; call them from the app task.
#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "spotify_parse.h"
#include "sonos_proto.h"

void ui_init(lv_display_t *disp);

// Big icon (LV_SYMBOL_*), title and a line of text. spinner shows activity.
void ui_show_message(const char *icon, const char *title, const char *text, bool spinner);

// QR code with a title and one or two lines of help text.
void ui_show_qr(const char *title, const char *qr_text, const char *line1, const char *line2);

void ui_show_rooms(const sonos_room_t *rooms, int n, const char *current_uid);
// Home screen: playlists (kids mode), or Search, Liked Songs, playlists and albums (full library mode).
// A tap sends CMD_OPEN_HOME with the card index.
void ui_show_home(const sp_entry_t *e, int n, int focus);

// Songs of a playlist/album. list_token comes back with CMD_PLAY so stale taps can be ignored.
void ui_show_tracks(int list_token, const char *title, const sp_entry_t *t, int n, bool can_go_back, int focus);

// Search results (songs, albums, playlists). A tap sends CMD_OPEN_RESULT with the index.
void ui_show_results(const sp_entry_t *e, int n);

// On-screen keyboard. Search sends the text with app_post_search(); the close key sends CMD_BACK.
void ui_show_search(const char *initial_text);

// Grown-up menu: QR for the settings page plus room and close buttons.
void ui_show_parent(const char *settings_url, const char *room_name);

void ui_set_now_playing(const char *track_id, bool playing);
void ui_show_volume(int vol);
void ui_toast(const char *text);
void ui_set_battery(int pct, bool charging);
void ui_mark_playing_request(int track_index);  // instant feedback after a tap
void ui_wake(void);  // count a button press as activity (wakes the screen)

// Screen dimming: the UI decides, the button task applies it outside the LVGL lock.
int ui_desired_brightness(void);
void ui_applied_brightness(int b);
