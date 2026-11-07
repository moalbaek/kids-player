// Desktop render of the player's screens (no hardware): writes PNG-ready PPM files.
#include "lvgl.h"
#include "ui.h"
#include "app.h"
#include "board.h"
#include "images.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint16_t *fb;
static uint32_t now_ms;
static uint32_t tick(void) { return now_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&fb[y * KP_SCREEN_W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}

bool board_lock(int t) { return true; }
void board_unlock(void) {}
void app_post(app_cmd_type_t t, int a, int b) { printf("  app_post(%d,%d,%d)\n", t, a, b); }
void app_post_search(const char *s) { printf("  search '%s'\n", s); }

// Fake covers: a coloured gradient per URL.
typedef struct { char url[256]; lv_image_dsc_t d; } fake_t;
static fake_t fakes[64];
static int nfakes;
const lv_image_dsc_t *images_acquire(const char *url, bool *failed)
{
    if (failed) *failed = false;
    if (!url || !url[0]) { if (failed) *failed = true; return NULL; }
    for (int i = 0; i < nfakes; i++) if (!strcmp(fakes[i].url, url)) return &fakes[i].d;
    if (nfakes == 64) return NULL;
    fake_t *f = &fakes[nfakes++];
    snprintf(f->url, sizeof f->url, "%s", url);
    unsigned h = 5381; for (const char *p = url; *p; p++) h = h * 33 + (unsigned char)*p;
    int S = KP_COVER;
    uint16_t *px = malloc(S * S * 2);
    int r0 = 60 + h % 160, g0 = 60 + (h >> 8) % 160, b0 = 60 + (h >> 16) % 160;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        int r = r0 * (S - y) / S + 30, g = g0 * (S - x) / S + 30, b = b0;
        if (((x / (S / 6)) + (y / (S / 6))) % 2) { r = r * 8 / 10; g = g * 8 / 10; b = b * 8 / 10; }
        px[y * S + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    f->d.header.magic = LV_IMAGE_HEADER_MAGIC;
    f->d.header.cf = LV_COLOR_FORMAT_RGB565;
    f->d.header.w = S; f->d.header.h = S; f->d.header.stride = S * 2;
    f->d.data_size = S * S * 2; f->d.data = (uint8_t *)px;
    return &f->d;
}
void images_release(const lv_image_dsc_t *d) {}
void images_request(const char *url) {}

static void run(int ms) { for (int i = 0; i < ms; i += 10) { now_ms += 10; lv_timer_handler(); } }

static void shot(const char *name)
{
    run(400);
    lv_refr_now(NULL);
    char fn[128]; snprintf(fn, sizeof fn, "out/%s_%s.ppm", BOARD, name);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", KP_SCREEN_W, KP_SCREEN_H);
    for (int i = 0; i < KP_SCREEN_W * KP_SCREEN_H; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", fn);
}

static sp_entry_t E(int kind, const char *id, const char *name, const char *artist, const char *img)
{
    sp_entry_t e; memset(&e, 0, sizeof e);
    e.kind = kind; snprintf(e.id, sizeof e.id, "%s", id); snprintf(e.name, sizeof e.name, "%s", name);
    snprintf(e.artist, sizeof e.artist, "%s", artist); snprintf(e.img, sizeof e.img, "%s", img);
    e.can_list = true;
    return e;
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(KP_SCREEN_W, KP_SCREEN_H);
    size_t sz = (size_t)KP_SCREEN_W * KP_SCREEN_H * 2;
    fb = calloc(1, sz);
    lv_display_set_buffers(d, malloc(sz), NULL, sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);
    ui_init(d);

    ui_show_message(LV_SYMBOL_AUDIO, "Music Player", "Starting…", false); shot("1_start");
    ui_show_qr("Scan to log in to Spotify", "http://192.168.1.57/login", "Use a phone on the home Wi-Fi, or open\nhttp://192.168.1.57/login", "Afterwards the player continues by itself."); shot("2_login");

    sp_entry_t home[8];
    int n = 0;
    home[n++] = E(SP_KIND_SEARCH, "", "Search", "", "");
    home[n++] = E(SP_KIND_LIKED, "", "Liked Songs", "", "");
    home[n++] = E(SP_KIND_PLAYLIST, "1aaaaaaaaaaaaaaaaaaaaa", "Kids: Frida", "", "u1");
    home[n++] = E(SP_KIND_PLAYLIST, "2bbbbbbbbbbbbbbbbbbbbb", "Søndag morgen", "", "u2");
    home[n++] = E(SP_KIND_ALBUM, "3ccccccccccccccccccccc", "Kind of Blue", "Miles Davis", "u3");
    home[n++] = E(SP_KIND_ALBUM, "4ddddddddddddddddddddd", "Hjerteknuser", "Kim Larsen", "u4");
    ui_show_home(home, n, 0); shot("3_home");
    ui_show_home(home, n, 4); shot("4_home_album");

    sp_entry_t tr[5];
    tr[0] = E(SP_KIND_TRACK, "5eeeeeeeeeeeeeeeeeeeee", "So What", "Miles Davis", "u3");
    tr[1] = E(SP_KIND_TRACK, "6fffffffffffffffffffff", "Freddie Freeloader (Take 3, remastered)", "Miles Davis", "u3");
    tr[2] = E(SP_KIND_TRACK, "7ggggggggggggggggggggg", "Blue in Green", "Miles Davis", "u3");
    tr[3] = E(SP_KIND_TRACK, "8hhhhhhhhhhhhhhhhhhhhh", "All Blues", "Miles Davis", "u3");
    tr[4] = E(SP_KIND_TRACK, "9iiiiiiiiiiiiiiiiiiiii", "Flamenco Sketches", "Miles Davis", "u3");
    ui_set_now_playing("6fffffffffffffffffffff", true);
    ui_show_tracks(1, "Kind of Blue", tr, 5, true, -1); shot("5_tracks");
    ui_show_volume(35); shot("6_volume");
    run(2000);

    ui_show_search("kim lar"); shot("7_search");
    sp_entry_t res[4];
    res[0] = E(SP_KIND_TRACK, "aaaaaaaaaaaaaaaaaaaaa1", "Midt om natten", "Kim Larsen", "u5");
    res[1] = E(SP_KIND_ALBUM, "aaaaaaaaaaaaaaaaaaaaa2", "Midt om natten", "Kim Larsen", "u5");
    res[2] = E(SP_KIND_PLAYLIST, "aaaaaaaaaaaaaaaaaaaaa3", "This Is Kim Larsen", "Spotify", "u6");
    res[3] = E(SP_KIND_TRACK, "aaaaaaaaaaaaaaaaaaaaa4", "Papirklip", "Kim Larsen", "u7");
    ui_show_results(res, 4); shot("8_results");
    ui_toast("Playing This Is Kim Larsen"); shot("9_toast");
    run(4000);
    ui_show_parent("http://192.168.1.57/", "Stue"); shot("10_parent");
    return 0;
}
