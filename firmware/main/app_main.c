// Kids Player: Spotify on Sonos from a small touchscreen.
// The app task owns the flow (Wi-Fi -> Spotify login -> speaker -> home screen) and runs all network calls;
// the UI and the web server send it commands through a queue.
//
// Two library modes (setting "full_library"):
//   kids: the home screen shows the playlists whose name starts with the prefix (e.g. "Kids:").
//   full: Search, Liked Songs, all playlists and saved albums.
#include "app.h"
#include "app_state.h"
#include "board.h"
#include "config.h"
#include "images.h"
#include "kp_util.h"
#include "ota.h"
#include "settings.h"
#include "sonos.h"
#include "spotify.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

static const char *TAG = "app";

typedef struct {
    app_cmd_type_t type;
    int a, b;
} app_cmd_t;

typedef enum {
    ST_BOOT,
    ST_WIFI_SETUP,
    ST_OFFLINE,
    ST_LOGIN,
    ST_PICK_ROOM,
    ST_MESSAGE,  // error / empty state, retried periodically
    ST_HOME,     // playlists (and albums etc. in full mode)
    ST_TRACKS,   // songs of s_list
    ST_SEARCH,   // keyboard
    ST_RESULTS,  // search results
    ST_PARENT,
} state_t;

static QueueHandle_t s_q;
static atomic_bool s_settings_changed;
static SemaphoreHandle_t s_data_mutex;
static state_t s_state = ST_BOOT, s_state_before_parent = ST_BOOT;

// Lists shown on screen. New data is loaded into the spare buffer and swapped in only on success, so a
// failed download never leaves a half-overwritten list behind. The UI keeps its own copies of what it shows.
static sp_entry_t *s_home, *s_home_spare;
static int s_nhome, s_home_focus;
static sp_entry_t *s_tracks, *s_tracks_spare;
static int s_ntr;
static sp_entry_t s_list;         // the playlist / album / Liked Songs whose songs are shown
static int s_list_token;          // changes whenever the song list on screen changes (taps carry it)
static state_t s_list_back;       // where Back goes from the songs: ST_HOME or ST_RESULTS
static sp_entry_t *s_results;
static int s_nres;
static char s_query[64], s_query_req[64];

static sonos_room_t s_rooms[KP_MAX_ROOMS];
static int s_nrooms;
static char s_user[64];
static int64_t s_last_retry_us;

static bool full_mode(void) { return g_set.full_library != 0; }

void app_post(app_cmd_type_t type, int a, int b)
{
    // A persisted settings change must be applied even if the command queue is full.
    if (type == CMD_SETTINGS_CHANGED) {
        atomic_store(&s_settings_changed, true);
        return;
    }
    if (!s_q) return;
    app_cmd_t c = {type, a, b};
    xQueueSend(s_q, &c, 0);
}

void app_post_search(const char *text)
{
    if (!s_data_mutex) return;
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    kp_strlcpy(s_query_req, text, sizeof(s_query_req));
    xSemaphoreGive(s_data_mutex);
    app_post(CMD_SEARCH, 0, 0);
}

void app_get_snapshot(app_snapshot_t *out)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    memcpy(out->rooms, s_rooms, sizeof(s_rooms));
    out->nrooms = s_nrooms;
    out->nplaylists = 0;
    for (int i = 0; i < s_nhome; i++) if (s_home[i].kind == SP_KIND_PLAYLIST) out->nplaylists++;
    kp_strlcpy(out->spotify_user, s_user, sizeof(out->spotify_user));
    xSemaphoreGive(s_data_mutex);
}

static void refresh_user(void)
{
    char name[64];
    if (spotify_get_display_name(name, sizeof(name)) != SP_OK) return;
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    kp_strlcpy(s_user, name, sizeof(s_user));
    xSemaphoreGive(s_data_mutex);
}

static void set_rooms(const sonos_room_t *rooms, int n)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    memcpy(s_rooms, rooms, sizeof(sonos_room_t) * n);
    s_nrooms = n;
    xSemaphoreGive(s_data_mutex);
}

static bool same_entries(const sp_entry_t *a, int na, const sp_entry_t *b, int nb)
{
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (a[i].kind != b[i].kind || strcmp(a[i].id, b[i].id) || strcmp(a[i].name, b[i].name) ||
            strcmp(a[i].artist, b[i].artist) || strcmp(a[i].img, b[i].img))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- loading

static sp_entry_t *special_card(sp_entry_t *e, sp_kind_t kind, const char *name)
{
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    kp_strlcpy(e->name, name, sizeof(e->name));
    return e;
}

// Home screen contents. Kids mode: prefix playlists. Full mode: Search, Liked Songs, playlists, saved albums.
static sp_result_t load_home(int *n)
{
    sp_entry_t *h = s_home_spare;
    int k = 0;
    if (full_mode()) {
        special_card(&h[k++], SP_KIND_SEARCH, "Search");
        special_card(&h[k++], SP_KIND_LIKED, "Liked Songs");
    }
    int np = 0;
    sp_result_t r = spotify_get_playlists(h + k, KP_MAX_HOME - k, &np);
    if (r != SP_OK) return r;
    k += np;
    if (full_mode() && k < KP_MAX_HOME) {
        int na = 0;
        r = spotify_get_saved_albums(h + k, KP_MAX_HOME - k, &na);
        if (r == SP_ERR_LOGGED_OUT) return r;
        if (r == SP_OK) k += na;  // albums are a bonus: show the playlists even if this failed
    }
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_home_spare = s_home;
    s_home = h;
    s_nhome = k;
    xSemaphoreGive(s_data_mutex);
    *n = k;
    return SP_OK;
}

// Songs of a playlist, album or Liked Songs into s_tracks (only replaced if something was found).
static sp_result_t load_songs(const sp_entry_t *list, int *n)
{
    *n = 0;
    sp_result_t r;
    switch (list->kind) {
    case SP_KIND_PLAYLIST: r = spotify_get_tracks(list->id, s_tracks_spare, KP_MAX_TRACKS, n); break;
    case SP_KIND_ALBUM: r = spotify_get_album_tracks(list->id, list->img, s_tracks_spare, KP_MAX_TRACKS, n); break;
    case SP_KIND_LIKED: r = spotify_get_liked(s_tracks_spare, KP_MAX_TRACKS, n); break;
    default: return SP_ERR_OTHER;
    }
    if (r != SP_OK || *n == 0) return r;
    sp_entry_t *t = s_tracks;
    s_tracks = s_tracks_spare;
    s_tracks_spare = t;
    s_ntr = *n;
    return SP_OK;
}

// ---------------------------------------------------------------- screens

static void show_login(void)
{
    char url[64];
    web_open_login(url, sizeof(url));
    ui_show_qr(full_mode() ? "Scan to log in to Spotify" : "Grown-up: scan to log in to Spotify", url,
               "Scan or open on your home Wi-Fi.", url);
    s_state = ST_LOGIN;
}

static void show_room_picker(void)
{
    ui_show_message(NULL, "Looking for Sonos speakers…", "", true);
    sonos_room_t *rooms = calloc(KP_MAX_ROOMS, sizeof(sonos_room_t));
    int n = rooms ? sonos_discover(rooms, KP_MAX_ROOMS) : 0;
    if (n > 0) set_rooms(rooms, n);
    free(rooms);
    s_state = ST_PICK_ROOM;
    if (n == 0) {
        ui_show_message(LV_SYMBOL_WARNING, "No Sonos speakers found",
                        "Check that the player is on the same Wi-Fi. To enter a speaker IP, hold BOOT and scan "
                        "the settings QR code.", false);
        s_state = ST_MESSAGE;
        s_last_retry_us = esp_timer_get_time();
        return;
    }
    ui_show_rooms(s_rooms, s_nrooms, g_set.speaker_uid);
}

// If only an IP was entered on the settings page, find which room it is.
static void resolve_room_from_ip(void)
{
    if (g_set.speaker_uid[0] || !g_set.speaker_ip[0]) return;
    sonos_room_t *rooms = calloc(KP_MAX_ROOMS, sizeof(sonos_room_t));
    int n = rooms ? sonos_discover(rooms, KP_MAX_ROOMS) : 0;
    if (n > 0) set_rooms(rooms, n);
    for (int i = 0; i < n; i++) {
        if (strcmp(rooms[i].ip, g_set.speaker_ip) == 0) {
            settings_lock();
            kp_strlcpy(g_set.speaker_uid, rooms[i].uid, sizeof(g_set.speaker_uid));
            kp_strlcpy(g_set.speaker_name, rooms[i].name, sizeof(g_set.speaker_name));
            settings_unlock();
            settings_save();
            break;
        }
    }
    free(rooms);
}

// In kids mode with a single playlist, its songs are the home screen (no way back).
static bool songs_can_go_back(void) { return s_list_back == ST_RESULTS || full_mode() || s_nhome > 1; }

static void show_home(void)
{
    if (s_home_focus >= s_nhome) s_home_focus = 0;
    ui_show_home(s_home, s_nhome, s_home_focus);
    s_state = ST_HOME;
}

static void show_songs(void)
{
    ui_show_tracks(s_list_token, s_list.name, s_tracks, s_ntr, songs_can_go_back(), -1);
    s_state = ST_TRACKS;
}

static void show_results(void)
{
    ui_show_results(s_results, s_nres);
    s_state = ST_RESULTS;
}

static void show_search(void)
{
    ui_show_search(s_query);
    s_state = ST_SEARCH;
}

static void go_back_to(state_t st)
{
    if (st == ST_RESULTS && s_nres > 0) show_results();
    else show_home();
}

static void play_failed_toast(void)
{
    char t[120];
    snprintf(t, sizeof(t), "Couldn't start the music on %s.", g_set.speaker_name);
    ui_toast(t);
}

// Play a whole playlist from the start (used where Spotify won't list its songs).
static void play_playlist_directly(const sp_entry_t *e)
{
    if (sonos_play_container(SONOS_SP_PLAYLIST, e->id, 0, NULL) == ESP_OK) {
        char t[140];
        snprintf(t, sizeof(t), "Playing %s", e->name);
        ui_toast(t);
    } else {
        play_failed_toast();
    }
}

static void go_home(void);

// Open a card. Only an explicit tap can start playback.
static void open_entry(const sp_entry_t *src, state_t back, bool tapped)
{
    sp_entry_t e = *src;  // the lists may be replaced while we work
    switch (e.kind) {
    case SP_KIND_SEARCH:
        show_search();
        return;
    case SP_KIND_TRACK:  // a song from the search: play its album from that song, so the music continues
        if (e.album_id[0] ? sonos_play_container(SONOS_SP_ALBUM, e.album_id, e.track_no > 0 ? e.track_no - 1 : 0, e.id)
                          : sonos_play_tracks((const char *const[]){e.id}, 1)) {
            play_failed_toast();
        } else {
            ui_set_now_playing(e.id, true);
        }
        return;
    case SP_KIND_PLAYLIST:
        if (!e.can_list) {  // Spotify only lists songs of your own playlists; Sonos can still play it
            if (tapped) play_playlist_directly(&e);
            return;
        }
        break;
    default:
        break;
    }
    ui_show_message(NULL, e.name, "", true);
    int n = 0;
    sp_result_t r = load_songs(&e, &n);
    if (r == SP_ERR_LOGGED_OUT) {
        show_login();
        return;
    }
    if (r == SP_OK && n > 0) {
        s_list = e;
        s_list_token++;
        s_list_back = back;
        show_songs();
        return;
    }
    if (tapped && e.kind == SP_KIND_PLAYLIST && r == SP_OK) {
        play_playlist_directly(&e);  // nothing listed (e.g. someone else's playlist): just play it
    } else {
        ui_toast(r != SP_OK ? "Could not load the songs. Try again." : "No songs here yet.");
    }
    if (back == ST_HOME && !full_mode() && s_nhome <= 1) {
        ui_show_message(LV_SYMBOL_AUDIO, "No songs yet", "Add songs to the playlist in the Spotify app.", false);
        s_state = ST_MESSAGE;
        s_last_retry_us = esp_timer_get_time();
    } else {
        go_back_to(back);
    }
}

// Decide what to show based on what is set up. Loads the home screen from Spotify.
static void go_home(void)
{
    if (!wifi_is_connected()) return;
    if (!spotify_logged_in()) {
        show_login();
        return;
    }
    resolve_room_from_ip();
    if (!g_set.speaker_uid[0]) {
        show_room_picker();
        return;
    }
    if (s_state != ST_HOME && s_state != ST_TRACKS) ui_show_message(NULL, "Loading your music…", "", true);
    int n = 0;
    sp_result_t r = load_home(&n);
    if (r == SP_ERR_LOGGED_OUT) {
        show_login();
        return;
    }
    if (r != SP_OK) {
        ui_show_message(LV_SYMBOL_WARNING, "Can't reach Spotify", "Trying again in a minute.", false);
        s_state = ST_MESSAGE;
        s_last_retry_us = esp_timer_get_time();
        return;
    }
    if (n == 0) {
        char text[160];
        snprintf(text, sizeof(text), "Make a playlist in Spotify whose name starts with \"%s\", e.g. \"%s Frida\".",
                 g_set.prefix, g_set.prefix);
        ui_show_message(LV_SYMBOL_LIST, "No playlists yet", text, false);
        s_state = ST_MESSAGE;
        s_last_retry_us = esp_timer_get_time();
        return;
    }
    if (!full_mode() && n == 1 && s_home[0].can_list) {
        open_entry(&s_home[0], ST_HOME, false);
        return;
    }
    show_home();
}

// Pick up playlist and song changes made in Spotify, without disturbing the screen if nothing changed.
// Only called while nobody is using the player.
static void refresh_quiet(void)
{
    int old_n = s_nhome, n = 0;
    sp_result_t r = load_home(&n);
    if (r == SP_ERR_LOGGED_OUT) {
        show_login();
        return;
    }
    if (r != SP_OK) return;  // try again next time
    bool home_changed = !same_entries(s_home, n, s_home_spare, old_n);  // spare holds the previous list now
    if (s_state == ST_HOME) {
        if (!full_mode() && (n == 0 || (n == 1 && s_home[0].can_list))) go_home();
        else if (home_changed) show_home();
        return;
    }
    // Showing songs. If they came from the home screen, check the list still exists (and its name).
    bool redraw = false;
    if (s_list_back == ST_HOME && s_list.kind != SP_KIND_LIKED) {
        int idx = -1;
        for (int i = 0; i < n; i++) if (s_home[i].kind == s_list.kind && !strcmp(s_home[i].id, s_list.id)) idx = i;
        if (idx < 0 || (!full_mode() && n == 0)) {
            go_home();  // deleted, or renamed away from the prefix
            return;
        }
        if (strcmp(s_home[idx].name, s_list.name) != 0) {
            kp_strlcpy(s_list.name, s_home[idx].name, sizeof(s_list.name));
            redraw = true;
        }
        if (!full_mode() && (n > 1) != (old_n > 1)) redraw = true;  // the back button appears/disappears
    }
    int old_nt = s_ntr, nt = 0;
    if (load_songs(&s_list, &nt) == SP_OK && nt > 0 && !same_entries(s_tracks, nt, s_tracks_spare, old_nt))
        redraw = true;
    if (redraw) {
        s_list_token++;  // indexes may have moved: ignore taps meant for the old list
        show_songs();
    }
}

static void start_wifi_setup(const char *reason)
{
    char ssids[16][33];
    int n = wifi_scan(ssids, 16);
    web_set_scan_results(ssids, n);
    char ap[33], pass[17], qr[96], line[200];
    wifi_start_ap(ap, sizeof(ap), pass, sizeof(pass));
    web_set_portal_mode(true);
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ap, pass);
    snprintf(line, sizeof(line), "If no page opens, go to http://" KP_SETUP_IP " (network %s, password %s).", ap, pass);
    ui_show_qr(reason ? reason : "Grown-up: scan to set up Wi-Fi", qr,
               "Scan with your phone's camera to join the player's setup network.", line);
    s_state = ST_WIFI_SETUP;
}

// Back to what was on screen before the grown-up menu.
static void restore_screen(state_t st)
{
    switch (st) {
    case ST_TRACKS:
        if (s_ntr > 0) {
            show_songs();
            return;
        }
        break;
    case ST_RESULTS:
        if (s_nres > 0) {
            show_results();
            return;
        }
        break;
    case ST_HOME:
    case ST_SEARCH:
        if (s_nhome > 0) {
            show_home();
            return;
        }
        break;
    default:
        break;
    }
    go_home();
}

// ---------------------------------------------------------------- commands

static void play_song(int index)
{
    esp_err_t err = ESP_FAIL;
    switch (s_list.kind) {
    case SP_KIND_PLAYLIST:
        err = sonos_play_container(SONOS_SP_PLAYLIST, s_list.id, index, s_tracks[index].id);
        break;
    case SP_KIND_ALBUM:
        err = sonos_play_container(SONOS_SP_ALBUM, s_list.id, index, s_tracks[index].id);
        break;
    case SP_KIND_LIKED: {  // no container for Liked Songs on Sonos: queue the songs one by one
        const char *ids[KP_LIKED_QUEUE];
        int n = 0;
        for (int i = index; i < s_ntr && n < KP_LIKED_QUEUE; i++) ids[n++] = s_tracks[i].id;
        err = sonos_play_tracks(ids, n);
        break;
    }
    default:
        break;
    }
    if (err != ESP_OK) play_failed_toast();
}

static void do_search(void)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    kp_strlcpy(s_query, s_query_req, sizeof(s_query));
    xSemaphoreGive(s_data_mutex);
    ui_show_message(NULL, "Searching…", s_query, true);
    int n = 0;
    sp_result_t r = spotify_search(s_query, s_results, KP_MAX_RESULTS, &n);
    if (r == SP_ERR_LOGGED_OUT) {
        show_login();
        return;
    }
    if (r != SP_OK || n == 0) {
        s_nres = 0;
        ui_toast(r != SP_OK ? "Search didn't work. Try again." : "Nothing found.");
        show_search();
        return;
    }
    s_nres = n;
    show_results();
}

static void handle(const app_cmd_t *c)
{
    switch (c->type) {
    case CMD_OPEN_HOME:
        if (s_state == ST_HOME && c->a >= 0 && c->a < s_nhome) {
            s_home_focus = c->a;
            open_entry(&s_home[c->a], ST_HOME, true);
        }
        break;
    case CMD_OPEN_RESULT:
        if (s_state == ST_RESULTS && c->a >= 0 && c->a < s_nres) open_entry(&s_results[c->a], ST_RESULTS, true);
        break;
    case CMD_PLAY:
        if (s_state == ST_TRACKS && c->a == s_list_token && c->b >= 0 && c->b < s_ntr) play_song(c->b);
        break;
    case CMD_SEARCH:
        if (s_state == ST_SEARCH) do_search();
        break;
    case CMD_TOGGLE:
        if (g_set.speaker_uid[0] && sonos_toggle_play() != ESP_OK) ui_toast("The speaker didn't answer.");
        break;
    case CMD_VOL_UP:
    case CMD_VOL_DOWN: {
        int v = 0;
        if (g_set.speaker_uid[0] && sonos_change_volume(c->type == CMD_VOL_UP ? 3 : -3, &v) == ESP_OK) ui_show_volume(v);
        break;
    }
    case CMD_BACK:
        if (s_state == ST_TRACKS && songs_can_go_back()) go_back_to(s_list_back);
        else if (s_state == ST_RESULTS || s_state == ST_SEARCH) show_home();
        break;
    case CMD_PARENT: {
        if (s_state == ST_WIFI_SETUP || !wifi_is_connected()) break;
        if (s_state != ST_PARENT) s_state_before_parent = s_state;
        char url[64];
        web_open_settings(url, sizeof(url));
        ui_show_parent(url, g_set.speaker_name);
        s_state = ST_PARENT;
        break;
    }
    case CMD_PICK_ROOM:
        if (s_state != ST_PARENT) break;
        web_close_settings();
        show_room_picker();
        break;
    case CMD_SELECT_ROOM:
        if (s_state == ST_PICK_ROOM && c->a >= 0 && c->a < s_nrooms) {
            settings_lock();
            kp_strlcpy(g_set.speaker_uid, s_rooms[c->a].uid, sizeof(g_set.speaker_uid));
            kp_strlcpy(g_set.speaker_name, s_rooms[c->a].name, sizeof(g_set.speaker_name));
            kp_strlcpy(g_set.speaker_ip, s_rooms[c->a].ip, sizeof(g_set.speaker_ip));
            settings_unlock();
            settings_save();
            sonos_reset_cache();
            ESP_LOGI(TAG, "room set to %s (%s)", g_set.speaker_name, g_set.speaker_ip);
            restore_screen(s_state_before_parent);
            s_state_before_parent = ST_BOOT;
        }
        break;
    case CMD_CLOSE_PARENT:
        if (s_state != ST_PARENT) break;
        web_close_settings();
        restore_screen(s_state_before_parent);
        s_state_before_parent = ST_BOOT;
        break;
    case CMD_LOGIN_DONE:
        refresh_user();
        s_state_before_parent = ST_BOOT;
        if (s_state != ST_PARENT) go_home();
        break;
    case CMD_SETTINGS_CHANGED:
        board_apply_rotation();
        sonos_reset_cache();
        s_home_focus = 0;
        s_state_before_parent = ST_BOOT;
        if (s_state != ST_WIFI_SETUP && s_state != ST_PARENT) go_home();
        break;
    case CMD_REFRESH:
        go_home();
        break;
    case CMD_OTA_CHECK: {
        ota_result_t r = ota_check_and_update();
        if (c->a) {
            if (r == OTA_UP_TO_DATE) ui_toast("The newest version is installed.");
            else if (r == OTA_REJECTED) ui_toast("The available update was rolled back.");
            else ui_toast("Could not install an update. Try again later.");
        }
        break;
    }
    case CMD_POWER_OFF:
        board_power_off();
        break;
    case CMD_RESTART:
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- buttons + brightness

static void button_task(void *arg)
{
    int applied = 100;
    for (;;) {
        board_button_t b = board_poll_button();
        if (b != BTN_NONE) {
            ui_wake();  // buttons work even with the screen off, and also wake it
            switch (b) {
            case BTN_PLAY_PAUSE: app_post(CMD_TOGGLE, 0, 0); break;
            case BTN_VOL_UP: app_post(CMD_VOL_UP, 0, 0); break;
            case BTN_VOL_DOWN: app_post(CMD_VOL_DOWN, 0, 0); break;
            case BTN_PARENT: app_post(CMD_PARENT, 0, 0); break;
            case BTN_POWER_OFF: app_post(CMD_POWER_OFF, 0, 0); break;
            default: break;
            }
        }
        int want = ui_desired_brightness();
        if (want != applied) {
            board_set_brightness(want);
            applied = want;
            ui_applied_brightness(want);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------- main

static sp_entry_t *psram_list(int n) { return heap_caps_calloc(n, sizeof(sp_entry_t), MALLOC_CAP_SPIRAM); }

void app_main(void)
{
    settings_init();
    spotify_init();
    s_q = xQueueCreate(16, sizeof(app_cmd_t));
    s_data_mutex = xSemaphoreCreateMutex();
    s_home = psram_list(KP_MAX_HOME);
    s_home_spare = psram_list(KP_MAX_HOME);
    s_tracks = psram_list(KP_MAX_TRACKS);
    s_tracks_spare = psram_list(KP_MAX_TRACKS);
    s_results = psram_list(KP_MAX_RESULTS);

    lv_display_t *disp = board_init();
    ui_init(disp);
    ui_show_message(LV_SYMBOL_AUDIO, full_mode() ? "Music Player" : "Kids Player", "Starting…", false);
    images_init();
    wifi_init();
    web_start();
    xTaskCreatePinnedToCore(button_task, "buttons", 4096, NULL, 5, NULL, 0);

    if (!g_set.wifi_ssid[0]) {
        start_wifi_setup(NULL);
    } else {
        char t[80];
        snprintf(t, sizeof(t), "Connecting to %s…", g_set.wifi_ssid);
        ui_show_message(NULL, t, "", true);
        if (!wifi_connect(g_set.wifi_ssid, g_set.wifi_pass, 25000)) {
            char r[96];
            snprintf(r, sizeof(r), "Can't connect to %s. Scan to change Wi-Fi.", g_set.wifi_ssid);
            start_wifi_setup(r);  // keeps trying the old network in the background
        }
    }

    int64_t online_since = 0, offline_since = 0, last_status = 0, last_battery = 0, last_refresh = 0, last_ota = 0;
    bool marked_valid = false;
    for (;;) {
        app_cmd_t c;
        bool have_command = xQueueReceive(s_q, &c, pdMS_TO_TICKS(1000)) == pdTRUE;
        if (atomic_exchange(&s_settings_changed, false)) handle(&(app_cmd_t){.type = CMD_SETTINGS_CHANGED});
        if (have_command) handle(&c);
        int64_t now = esp_timer_get_time();

        // Wi-Fi state changes
        if (wifi_is_connected()) {
            offline_since = 0;
            if (!online_since) {
                online_since = now;
                if (wifi_ap_running()) {  // left setup mode: the old network came back
                    web_set_portal_mode(false);
                    wifi_stop_ap();
                }
                ota_check_and_update();  // restarts if an update was installed
                last_ota = now;
                if (spotify_logged_in()) refresh_user();
                go_home();
            }
        } else if (s_state != ST_WIFI_SETUP) {
            if (online_since) {
                web_close_settings();
                web_close_login();
            }
            online_since = 0;
            if (!offline_since) offline_since = now;
            if (now - offline_since > 10LL * 1000000 && s_state != ST_OFFLINE) {
                ui_show_message(LV_SYMBOL_WIFI, "No Wi-Fi", "Trying to reconnect…", false);
                s_state = ST_OFFLINE;
            }
        }
        if (!online_since) continue;

        if (!marked_valid && now - online_since > 30LL * 1000000) {
            ota_mark_valid();
            marked_valid = true;
        }
        bool browsing = s_state == ST_HOME || s_state == ST_TRACKS || s_state == ST_RESULTS;
        if (browsing && now - last_status > 3LL * 1000000) {
            last_status = now;
            sonos_status_t st;
            if (sonos_get_status(&st) == ESP_OK) ui_set_now_playing(st.track_id, st.playing);
        }
        if (now - last_battery > 60LL * 1000000) {
            last_battery = now;
            bool chg = false;
            int pct = board_battery(&chg);
            ui_set_battery(pct, chg);
        }
        if (s_state == ST_MESSAGE && now - s_last_retry_us > 60LL * 1000000) {
            s_last_retry_us = now;
            go_home();
        }
        // Pick up new playlists and songs now and then, but only while nobody is touching the screen.
        if ((s_state == ST_HOME || s_state == ST_TRACKS) && now - last_refresh > 15LL * 60 * 1000000 &&
            ui_desired_brightness() < 100) {
            last_refresh = now;
            refresh_quiet();
        }
        if (now - last_ota > 24LL * 3600 * 1000000 && ui_desired_brightness() == 0) {
            last_ota = now;
            ota_check_and_update();
        }
    }
}
