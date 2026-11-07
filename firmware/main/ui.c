#include "ui.h"
#include "app.h"
#include "board.h"
#include "config.h"
#include "images.h"
#include "kp_util.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(kp_font_18);
LV_FONT_DECLARE(kp_font_24);
LV_FONT_DECLARE(kp_font_32);

#define COL_BG lv_color_hex(0x000000)
#define COL_CARD lv_color_hex(0x262626)
#define COL_TEXT lv_color_hex(0xFFFFFF)
#define COL_MUTED lv_color_hex(0xB3B3B3)
#define COL_ACCENT lv_color_hex(0x1ED760)

#if CONFIG_KP_BOARD_CUBE
#define IS_CUBE 1
#define F_TITLE (&kp_font_32)
#define F_BODY (&kp_font_24)
#define F_SMALL (&kp_font_24)
#define COVER_BOX 300
#define CARD_PITCH KP_SCREEN_W
#define WINDOW_BEFORE 2
#define WINDOW_AFTER 2
#else
#define IS_CUBE 0
#define F_TITLE (&kp_font_24)
#define F_BODY (&kp_font_18)
#define F_SMALL (&kp_font_18)
#define COVER_BOX 140
#define CTRL_W 64                  // control column on the right
#define CARD_W COVER_BOX
#define CARD_GAP 12
#define CARD_PITCH (CARD_W + CARD_GAP)
#define WINDOW_BEFORE 2
#define WINDOW_AFTER 5
#endif

typedef enum { MODE_NONE, MODE_HOME, MODE_TRACKS, MODE_RESULTS, MODE_SEARCH } mode_t_;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *img;
    lv_obj_t *placeholder;
    lv_obj_t *badge;
    lv_obj_t *badge_label;
    char url[SP_IMG_LEN];
    char id[SP_ID_LEN];
    const lv_image_dsc_t *dsc;
} slot_t;

static lv_obj_t *s_root;     // the screen
static lv_obj_t *s_pager;    // scrolling container in list modes
static slot_t *s_slots;
static int s_nslots;
static mode_t_ s_mode;
static int s_list_token;
static char s_np_id[SP_ID_LEN];
static bool s_np_playing;
static char s_followed_id[SP_ID_LEN];
static lv_obj_t *s_vol_box, *s_vol_bar, *s_toast, *s_battery, *s_wake_catcher, *s_play_btn_label;
static lv_timer_t *s_vol_timer, *s_toast_timer;
static lv_obj_t *s_search_ta;  // search text field while the keyboard is shown
static int s_desired_brightness = 100;
static int s_applied_brightness = 100;

int ui_desired_brightness(void) { return s_desired_brightness; }

// ---------------------------------------------------------------- helpers (LVGL lock held)

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static void release_slots(void)
{
    for (int i = 0; i < s_nslots; i++) {
        if (s_slots[i].dsc) {
            lv_image_set_src(s_slots[i].img, NULL);
            images_release(s_slots[i].dsc);
            s_slots[i].dsc = NULL;
        }
    }
    s_nslots = 0;
    s_mode = MODE_NONE;
    s_pager = NULL;
    s_play_btn_label = NULL;
}

static void clear_screen(void)
{
    release_slots();
    s_search_ta = NULL;
    lv_obj_clean(s_root);
    lv_obj_scroll_to(s_root, 0, 0, LV_ANIM_OFF);
}

static int current_index(void)
{
    if (!s_pager || s_nslots == 0) return 0;
    int x = lv_obj_get_scroll_x(s_pager);
    int i = (x + CARD_PITCH / 2) / CARD_PITCH;
    if (i < 0) i = 0;
    if (i >= s_nslots) i = s_nslots - 1;
    return i;
}

static void scroll_to_index(int i, bool anim)
{
    if (!s_pager || i < 0 || i >= s_nslots) return;
    lv_obj_update_layout(s_pager);
    lv_obj_scroll_to_x(s_pager, i * CARD_PITCH, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void update_badges(void)
{
    for (int i = 0; i < s_nslots; i++) {
        slot_t *s = &s_slots[i];
        if (!s->badge) continue;
        bool on = (s_mode == MODE_TRACKS || s_mode == MODE_RESULTS) && s_np_id[0] && strcmp(s->id, s_np_id) == 0;
        if (on) {
            lv_obj_set_hidden(s->badge, false);
            lv_label_set_text(s->badge_label, s_np_playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        } else {
            lv_obj_set_hidden(s->badge, true);
        }
#if !IS_CUBE
        lv_obj_set_style_border_width(s->img, on ? 4 : 0, 0);
#endif
    }
    if (s_play_btn_label) lv_label_set_text(s_play_btn_label, s_np_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// ---------------------------------------------------------------- periodic work (runs in the LVGL task)

static void manage_images(void)
{
    if (s_mode == MODE_NONE || !s_pager || s_nslots == 0) return;
    int cur = current_index();
    int lo = cur - WINDOW_BEFORE, hi = cur + WINDOW_AFTER;
    // Drop images far away, so the cache can reuse them.
    for (int i = 0; i < s_nslots; i++) {
        slot_t *s = &s_slots[i];
        if ((i < lo - 2 || i > hi + 2) && s->dsc) {
            lv_image_set_src(s->img, NULL);
            images_release(s->dsc);
            s->dsc = NULL;
            lv_obj_set_hidden(s->placeholder, false);
        }
    }
    // Load what is close; request the current page last so it is fetched first.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = hi; i >= lo; i--) {
            if (i < 0 || i >= s_nslots) continue;
            if ((pass == 0) == (i == cur)) continue;
            slot_t *s = &s_slots[i];
            if (s->dsc || !s->url[0]) continue;
            bool failed = false;
            const lv_image_dsc_t *d = images_acquire(s->url, &failed);
            if (d) {
                s->dsc = d;
                lv_image_set_src(s->img, d);
                lv_obj_set_hidden(s->placeholder, true);
            } else if (!failed) {
                images_request(s->url);
            }
        }
    }
}

static void follow_now_playing(void)
{
    if (s_mode != MODE_TRACKS || !s_np_id[0] || strcmp(s_np_id, s_followed_id) == 0) return;
    if (lv_display_get_inactive_time(NULL) < 15000) return;  // don't fight a child who is browsing
    for (int i = 0; i < s_nslots; i++) {
        if (strcmp(s_slots[i].id, s_np_id) == 0) {
            if (current_index() != i) scroll_to_index(i, true);
            break;
        }
    }
    kp_strlcpy(s_followed_id, s_np_id, sizeof(s_followed_id));
}

static void wake_catcher_cb(lv_event_t *e)
{
    // Swallows the touch that wakes the screen.
}

static void dim_screen(void)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    int want = idle < 60000 ? 100 : (idle < 300000 ? 25 : 0);
    s_desired_brightness = want;
    if (want == 0 && !s_wake_catcher) {
        s_wake_catcher = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(s_wake_catcher);
        lv_obj_set_size(s_wake_catcher, LV_PCT(100), LV_PCT(100));
        lv_obj_set_clickable(s_wake_catcher, true);
        lv_obj_add_event_cb(s_wake_catcher, wake_catcher_cb, LV_EVENT_CLICKED, NULL);
    } else if (want > 0 && s_wake_catcher && s_applied_brightness > 0) {
        lv_obj_delete(s_wake_catcher);
        s_wake_catcher = NULL;
    }
}

static void tick_cb(lv_timer_t *t)
{
    manage_images();
    follow_now_playing();
    dim_screen();
}

// Called by the app's button task (outside the LVGL lock).
void ui_applied_brightness(int b) { s_applied_brightness = b; }

// ---------------------------------------------------------------- overlays

static void hide_cb(lv_timer_t *t)
{
    lv_obj_t *o = lv_timer_get_user_data(t);
    lv_obj_set_hidden(o, true);
    lv_timer_pause(t);
}

static void make_overlays(void)
{
    lv_obj_t *top = lv_layer_top();
    s_vol_box = lv_obj_create(top);
    lv_obj_set_size(s_vol_box, IS_CUBE ? 340 : 360, IS_CUBE ? 70 : 56);
    lv_obj_align(s_vol_box, LV_ALIGN_BOTTOM_MID, 0, IS_CUBE ? -24 : -8);
    lv_obj_set_style_bg_color(s_vol_box, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_vol_box, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_vol_box, 35, 0);
    lv_obj_set_style_border_width(s_vol_box, 0, 0);
    lv_obj_set_scrollable(s_vol_box, false);
    lv_obj_t *icon = label(s_vol_box, &lv_font_montserrat_28, COL_TEXT, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
    s_vol_bar = lv_bar_create(s_vol_box);
    lv_obj_set_size(s_vol_bar, IS_CUBE ? 240 : 270, 14);
    lv_obj_align(s_vol_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0x505050), 0);
    lv_obj_set_style_bg_color(s_vol_bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_vol_box, true);
    s_vol_timer = lv_timer_create(hide_cb, 1500, s_vol_box);
    lv_timer_pause(s_vol_timer);

    s_toast = lv_label_create(top);
    lv_obj_set_style_text_font(s_toast, F_BODY, 0);
    lv_obj_set_style_text_color(s_toast, COL_TEXT, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_toast, 12, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_width(s_toast, IS_CUBE ? 400 : 420);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, IS_CUBE ? 24 : 8);
    lv_obj_set_hidden(s_toast, true);
    s_toast_timer = lv_timer_create(hide_cb, 3000, s_toast);
    lv_timer_pause(s_toast_timer);

    s_battery = label(top, &lv_font_montserrat_20, COL_MUTED, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -28, 22);
    lv_obj_set_hidden(s_battery, true);
}

// ---------------------------------------------------------------- init

void ui_init(lv_display_t *disp)
{
    s_slots = heap_caps_calloc(KP_MAX_TRACKS > KP_MAX_HOME ? KP_MAX_TRACKS : KP_MAX_HOME, sizeof(slot_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    board_lock(-1);
    s_root = lv_screen_active();
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_root, false);
    make_overlays();
    lv_timer_create(tick_cb, 120, NULL);
    board_unlock();
}

// ---------------------------------------------------------------- simple screens

void ui_show_message(const char *icon, const char *title, const char *text, bool spinner)
{
    board_lock(-1);
    clear_screen();
#if IS_CUBE
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 420, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 16, 0);
    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(col);
        lv_obj_set_size(sp, 72, 72);
        lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
    } else if (icon) {
        label(col, &lv_font_montserrat_40, COL_ACCENT, icon);
    }
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if (text && text[0]) {
        lv_obj_t *b = label(col, F_BODY, COL_MUTED, text);
        lv_obj_set_width(b, 420);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    }
#else
    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(s_root);
        lv_obj_set_size(sp, 64, 64);
        lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_align(sp, LV_ALIGN_LEFT_MID, 24, 0);
    } else if (icon) {
        lv_obj_t *i = label(s_root, &lv_font_montserrat_40, COL_ACCENT, icon);
        lv_obj_align(i, LV_ALIGN_LEFT_MID, 32, 0);
    }
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 500, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 116, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 500);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    if (text && text[0]) {
        lv_obj_t *b = label(col, F_BODY, COL_MUTED, text);
        lv_obj_set_width(b, 500);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    }
#endif
    board_unlock();
}

void ui_show_qr(const char *title, const char *qr_text, const char *line1, const char *line2)
{
    board_lock(-1);
    clear_screen();
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, title);
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_size(qr, 240);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_text, strlen(qr_text));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 10, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *l1 = label(s_root, &kp_font_18, COL_TEXT, line1);
    lv_obj_set_width(l1, 420);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 340);
    if (line2 && line2[0]) {
        lv_obj_t *l2 = label(s_root, &kp_font_18, COL_MUTED, line2);
        lv_obj_set_width(l2, 400);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(l2, l1, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    }
#else
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_size(qr, 148);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_text, strlen(qr_text));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 450, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 178, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *t = label(col, F_TITLE, COL_TEXT, title);
    lv_obj_set_width(t, 450);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *l1 = label(col, &kp_font_18, COL_TEXT, line1);
    lv_obj_set_width(l1, 450);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_WRAP);
    if (line2 && line2[0]) {
        lv_obj_t *l2 = label(col, &kp_font_18, COL_MUTED, line2);
        lv_obj_set_width(l2, 450);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_WRAP);
    }
#endif
    board_unlock();
}

static void room_cb(lv_event_t *e)
{
    app_post(CMD_SELECT_ROOM, (int)(intptr_t)lv_event_get_user_data(e), 0);
}

static lv_obj_t *big_button(lv_obj_t *parent, const char *text, int w, int h, bool selected)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, selected ? COL_ACCENT : COL_CARD, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, F_BODY, selected ? lv_color_black() : COL_TEXT, text);
    lv_obj_set_width(l, w - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

void ui_show_rooms(const sonos_room_t *rooms, int n, const char *current_uid)
{
    board_lock(-1);
    clear_screen();
    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Which speaker should the music play on?");
    lv_obj_set_width(t, 420);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_size(list, 420, 360);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = big_button(list, rooms[i].name, 400, 72, current_uid && strcmp(current_uid, rooms[i].uid) == 0);
        lv_obj_add_event_cb(b, room_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
#else
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Speaker:");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_size(list, 540, 150);
    lv_obj_align(list, LV_ALIGN_LEFT_MID, 96, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_HOR);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = big_button(list, rooms[i].name, 180, 120, current_uid && strcmp(current_uid, rooms[i].uid) == 0);
        lv_obj_add_event_cb(b, room_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
#endif
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    board_unlock();
}

static void parent_btn_cb(lv_event_t *e)
{
    app_post((app_cmd_type_t)(intptr_t)lv_event_get_user_data(e), 0, 0);
}

void ui_show_parent(const char *settings_url, const char *room_name)
{
    char room_txt[96];
    snprintf(room_txt, sizeof(room_txt), LV_SYMBOL_AUDIO "  %s", room_name && room_name[0] ? room_name : "Choose speaker");
    char address[72];
    const char *key = strchr(settings_url, '=');
    if (key) {
        snprintf(address, sizeof(address), "%.*s\n%s", (int)(key + 1 - settings_url), settings_url, key + 1);
    } else {
        kp_strlcpy(address, settings_url, sizeof(address));
    }
    board_lock(-1);
    clear_screen();
    lv_obj_t *qr = lv_qrcode_create(s_root);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
#if IS_CUBE
    lv_obj_t *t = label(s_root, F_BODY, COL_TEXT, "Settings: scan with your phone");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lv_qrcode_set_size(qr, 180);
    lv_qrcode_update(qr, settings_url, strlen(settings_url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *u = label(s_root, &kp_font_18, COL_MUTED, address);
    lv_obj_set_width(u, 420);
    lv_label_set_long_mode(u, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(u, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_t *rb = big_button(s_root, room_txt, 400, 64, false);
    lv_obj_align(rb, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_t *db = big_button(s_root, "Done", 400, 64, true);
    lv_obj_align(db, LV_ALIGN_TOP_MID, 0, 394);
#else
    lv_qrcode_set_size(qr, 148);
    lv_qrcode_update(qr, settings_url, strlen(settings_url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *u = label(s_root, &kp_font_18, COL_MUTED, address);
    lv_obj_set_width(u, 450);
    lv_label_set_long_mode(u, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(u, LV_ALIGN_TOP_LEFT, 178, 6);
    lv_obj_t *rb = big_button(s_root, room_txt, 300, 50, false);
    lv_obj_align(rb, LV_ALIGN_TOP_LEFT, 178, 52);
    lv_obj_t *db = big_button(s_root, "Done", 300, 50, true);
    lv_obj_align(db, LV_ALIGN_TOP_LEFT, 178, 110);
#endif
    lv_obj_add_event_cb(rb, parent_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)CMD_PICK_ROOM);
    lv_obj_add_event_cb(db, parent_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)CMD_CLOSE_PARENT);
    board_unlock();
}

// ---------------------------------------------------------------- browsing screens

static void card_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nslots) return;
    bool is_playing_song = s_np_id[0] && strcmp(s_slots[i].id, s_np_id) == 0;
    if (s_mode == MODE_HOME) {
        app_post(CMD_OPEN_HOME, i, 0);
    } else if ((s_mode == MODE_TRACKS || s_mode == MODE_RESULTS) && is_playing_song) {
        // Tapping the song that is on: pause / resume instead of starting it over.
        app_post(CMD_TOGGLE, 0, 0);
        s_np_playing = !s_np_playing;
        update_badges();
    } else if (s_mode == MODE_TRACKS) {
        app_post(CMD_PLAY, s_list_token, i);
        ui_mark_playing_request(i);
    } else if (s_mode == MODE_RESULTS) {
        app_post(CMD_OPEN_RESULT, i, 0);
    }
}

static void ctrl_cb(lv_event_t *e)
{
    app_post((app_cmd_type_t)(intptr_t)lv_event_get_user_data(e), 0, 0);
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int size, app_cmd_type_t cmd)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COL_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = label(b, &lv_font_montserrat_28, COL_TEXT, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, ctrl_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return b;
}

static const char *placeholder_icon(uint8_t kind)
{
    switch (kind) {
    case SP_KIND_SEARCH: return LV_SYMBOL_KEYBOARD;
    case SP_KIND_LIKED: return LV_SYMBOL_OK;
    case SP_KIND_ALBUM: return LV_SYMBOL_IMAGE;
    case SP_KIND_PLAYLIST: return LV_SYMBOL_LIST;
    default: return LV_SYMBOL_AUDIO;
    }
}

#define CHIP_ALBUM 1
#define CHIP_PLAYLIST 2

// One line of text, cut with "…" when too long (DOTS needs a fixed height).
static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text, int w)
{
    lv_obj_t *l = label(parent, font, color, text);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// Build a pager/carousel of cards. subs may be NULL (second line, cube only). chips: on the strip, mark
// albums and/or playlists with a small tag on the cover (the cube shows this in the second line).
static void build_cards(const sp_entry_t *e, int n, const char *const *subs, bool can_go_back, unsigned chips)
{
    s_pager = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pager);
#if IS_CUBE
    lv_obj_set_size(s_pager, KP_SCREEN_W, KP_SCREEN_H);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_style_pad_column(s_pager, 0, 0);
#else
    lv_obj_set_size(s_pager, KP_SCREEN_W - CTRL_W, KP_SCREEN_H);
    lv_obj_set_style_pad_left(s_pager, CARD_GAP, 0);
    lv_obj_set_style_pad_right(s_pager, CARD_GAP, 0);
    lv_obj_set_style_pad_column(s_pager, CARD_GAP, 0);
#endif
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
#if IS_CUBE
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_one(s_pager, true);
#endif

    for (int i = 0; i < n; i++) {
        slot_t *s = &s_slots[i];
        memset(s, 0, sizeof(*s));
        kp_strlcpy(s->url, e[i].img, sizeof(s->url));
        kp_strlcpy(s->id, e[i].id, sizeof(s->id));
        bool special = e[i].kind == SP_KIND_SEARCH || e[i].kind == SP_KIND_LIKED;
        lv_obj_t *c = lv_obj_create(s_pager);
        s->card = c;
        lv_obj_remove_style_all(c);
        lv_obj_set_clickable(c, true);
        lv_obj_set_scrollable(c, false);
        lv_obj_set_event_bubble(c, true);
        lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
#if IS_CUBE
        lv_obj_set_size(c, KP_SCREEN_W, KP_SCREEN_H);
        int cx = (KP_SCREEN_W - COVER_BOX) / 2, cy = 46;
#else
        lv_obj_set_size(c, CARD_W, KP_SCREEN_H);
        int cx = 0, cy = 4;
#endif
        s->placeholder = lv_obj_create(c);
        lv_obj_remove_style_all(s->placeholder);
        lv_obj_set_size(s->placeholder, COVER_BOX, COVER_BOX);
        lv_obj_set_pos(s->placeholder, cx, cy);
        lv_obj_set_style_bg_color(s->placeholder, special ? COL_ACCENT : COL_CARD, 0);
        lv_obj_set_style_bg_opa(s->placeholder, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s->placeholder, 16, 0);
        lv_obj_set_clickable(s->placeholder, false);
        lv_obj_t *note = label(s->placeholder, &lv_font_montserrat_40, special ? lv_color_black() : COL_MUTED,
                               placeholder_icon(e[i].kind));
        lv_obj_center(note);

        s->img = lv_image_create(c);
        lv_obj_set_size(s->img, COVER_BOX, COVER_BOX);
        lv_obj_set_pos(s->img, cx, cy);
        lv_image_set_inner_align(s->img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_clickable(s->img, false);
#if !IS_CUBE
        lv_obj_set_style_border_color(s->img, COL_ACCENT, 0);  // shown on the song that is playing
        lv_obj_set_style_border_width(s->img, 0, 0);
        lv_obj_set_style_border_post(s->img, true, 0);  // draw it over the cover, not under it
        const char *chip = (e[i].kind == SP_KIND_ALBUM && (chips & CHIP_ALBUM))         ? "Album"
                           : (e[i].kind == SP_KIND_PLAYLIST && (chips & CHIP_PLAYLIST)) ? "Playlist"
                                                                                       : NULL;
        if (chip) {
            lv_obj_t *ch = label(c, &lv_font_montserrat_14, COL_TEXT, chip);
            lv_obj_set_style_bg_color(ch, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(ch, LV_OPA_70, 0);
            lv_obj_set_style_pad_hor(ch, 6, 0);
            lv_obj_set_style_pad_ver(ch, 2, 0);
            lv_obj_set_style_radius(ch, 6, 0);
            lv_obj_set_pos(ch, cx + 6, cy + 6);
        }
#endif

        s->badge = lv_obj_create(c);
        lv_obj_remove_style_all(s->badge);
        int bs = IS_CUBE ? 72 : 44;
        lv_obj_set_size(s->badge, bs, bs);
        lv_obj_set_pos(s->badge, cx + COVER_BOX - bs - 8, cy + COVER_BOX - bs - 8);
        lv_obj_set_style_radius(s->badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s->badge, COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(s->badge, LV_OPA_COVER, 0);
        lv_obj_set_clickable(s->badge, false);
        s->badge_label = label(s->badge, IS_CUBE ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                               lv_color_black(), LV_SYMBOL_PLAY);
        lv_obj_center(s->badge_label);
        lv_obj_set_hidden(s->badge, true);

#if IS_CUBE
        lv_obj_t *t = one_line(c, F_TITLE, COL_TEXT, e[i].name, 440);
        lv_obj_set_pos(t, 20, cy + COVER_BOX + 14);
        if (subs && subs[i] && subs[i][0]) {
            lv_obj_t *a = one_line(c, F_BODY, COL_MUTED, subs[i], 440);
            lv_obj_set_pos(a, 20, cy + COVER_BOX + 62);
        }
#else
        lv_obj_t *t = one_line(c, F_SMALL, COL_TEXT, e[i].name, CARD_W);
        lv_obj_set_pos(t, 0, cy + COVER_BOX + 2);
#endif
    }
    s_nslots = n;

#if IS_CUBE
    if (can_go_back) {
        lv_obj_t *b = round_button(s_root, LV_SYMBOL_LEFT, 60, CMD_BACK);
        lv_obj_set_pos(b, 22, 22);
    }
#else
    lv_obj_t *ctrl = lv_obj_create(s_root);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, CTRL_W, KP_SCREEN_H);
    lv_obj_align(ctrl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ctrl, false);
    if (can_go_back) round_button(ctrl, LV_SYMBOL_LEFT, 38, CMD_BACK);
    round_button(ctrl, LV_SYMBOL_PLUS, 38, CMD_VOL_UP);
    lv_obj_t *pb = round_button(ctrl, LV_SYMBOL_PLAY, 38, CMD_TOGGLE);
    s_play_btn_label = lv_obj_get_child(pb, 0);
    round_button(ctrl, LV_SYMBOL_MINUS, 38, CMD_VOL_DOWN);
#endif
}

void ui_show_home(const sp_entry_t *e, int n, int focus)
{
    if (n > KP_MAX_HOME) n = KP_MAX_HOME;
    // Second line on the cube: the artist of an album.
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!subs) return;
    for (int i = 0; i < n; i++) subs[i] = e[i].kind == SP_KIND_ALBUM ? e[i].artist : "";
    board_lock(-1);
    clear_screen();
    build_cards(e, n, subs, false, CHIP_ALBUM);
    s_mode = MODE_HOME;
    scroll_to_index(focus, false);
    board_unlock();
    heap_caps_free(subs);
}

void ui_show_tracks(int list_token, const char *title, const sp_entry_t *t, int n, bool can_go_back, int focus)
{
    if (n > KP_MAX_TRACKS) n = KP_MAX_TRACKS;
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!subs) return;
    for (int i = 0; i < n; i++) subs[i] = t[i].artist;
    board_lock(-1);
    clear_screen();
    build_cards(t, n, subs, can_go_back, 0);
    s_mode = MODE_TRACKS;
    s_list_token = list_token;
    s_followed_id[0] = 0;
    update_badges();
    int f = focus;
    if (f < 0) {  // start at the song that is playing, if it is in this list
        f = 0;
        for (int i = 0; i < n; i++) if (s_np_id[0] && strcmp(t[i].id, s_np_id) == 0) f = i;
    }
    scroll_to_index(f, false);
    board_unlock();
    heap_caps_free(subs);
}

void ui_show_results(const sp_entry_t *e, int n)
{
    if (n > KP_MAX_RESULTS) n = KP_MAX_RESULTS;
    char *buf = heap_caps_calloc(n ? n : 1, 96, MALLOC_CAP_SPIRAM);
    const char **subs = heap_caps_calloc(n ? n : 1, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!buf || !subs) {
        heap_caps_free(buf);
        heap_caps_free(subs);
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *what = e[i].kind == SP_KIND_ALBUM ? "Album" : e[i].kind == SP_KIND_PLAYLIST ? "Playlist" : "Song";
        snprintf(buf + i * 96, 96, e[i].artist[0] ? "%s · %s" : "%s", what, e[i].artist);
        subs[i] = buf + i * 96;
    }
    board_lock(-1);
    clear_screen();
    build_cards(e, n, subs, true, CHIP_ALBUM | CHIP_PLAYLIST);
    s_mode = MODE_RESULTS;
    update_badges();
    scroll_to_index(0, false);
    board_unlock();
    heap_caps_free(subs);
    heap_caps_free(buf);
}

// ---------------------------------------------------------------- search keyboard

// Danish letters on the main layouts; the special-character layout is LVGL's default.
static const char *const kb_lower[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "å", LV_SYMBOL_BACKSPACE, "\n",
                                       "a", "s", "d", "f", "g", "h", "j", "k", "l", "æ", "ø", LV_SYMBOL_OK, "\n",
                                       "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "-", "\n",
                                       LV_SYMBOL_CLOSE, "1#", " ", "'", "&", ""};
static const char *const kb_upper[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Å", LV_SYMBOL_BACKSPACE, "\n",
                                       "A", "S", "D", "F", "G", "H", "J", "K", "L", "Æ", "Ø", LV_SYMBOL_OK, "\n",
                                       "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "-", "\n",
                                       LV_SYMBOL_CLOSE, "1#", " ", "'", "&", ""};
#define KB_KEY 4
#define KB_FN (LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6)
static const lv_buttonmatrix_ctrl_t kb_ctrl[] = {
    KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_FN,
    KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_FN,
    KB_FN, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY, KB_KEY,
    KB_FN, KB_FN, 15, KB_KEY, KB_KEY,  // widths are 4 bits (max 15)
};


static void search_ready_cb(lv_event_t *e)
{
    const char *txt = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";
    while (*txt == ' ') txt++;
    if (*txt) app_post_search(txt);
}

static void search_cancel_cb(lv_event_t *e) { app_post(CMD_BACK, 0, 0); }

void ui_show_search(const char *initial_text)
{
    board_lock(-1);
    clear_screen();
    s_mode = MODE_SEARCH;
    lv_obj_t *ta = lv_textarea_create(s_root);
    s_search_ta = ta;
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 60);
    lv_textarea_set_placeholder_text(ta, "Search Spotify");
    lv_textarea_set_text(ta, initial_text ? initial_text : "");
    lv_obj_set_style_text_font(ta, F_BODY, 0);
    lv_obj_set_style_bg_color(ta, COL_CARD, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);  // shows the cursor
    lv_obj_t *kb = lv_keyboard_create(s_root);
#if IS_CUBE
    lv_obj_set_size(ta, 400, 52);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_size(kb, KP_SCREEN_W, 300);
#else
    lv_obj_set_size(ta, KP_SCREEN_W - 16, 38);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_pad_ver(ta, 4, 0);
    lv_obj_set_size(kb, KP_SCREEN_W, KP_SCREEN_H - 44);
#endif
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, F_BODY, 0);
    lv_obj_set_style_bg_color(kb, COL_BG, 0);
    lv_obj_set_style_bg_color(kb, COL_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A3A3A), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, search_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, search_cancel_cb, LV_EVENT_CANCEL, NULL);
    board_unlock();
}

void ui_mark_playing_request(int track_index)
{
    // Called from the LVGL task (tap handler) as well as the app: no locking here.
    if (s_mode != MODE_TRACKS || track_index < 0 || track_index >= s_nslots) return;
    kp_strlcpy(s_np_id, s_slots[track_index].id, sizeof(s_np_id));
    kp_strlcpy(s_followed_id, s_np_id, sizeof(s_followed_id));
    s_np_playing = true;
    update_badges();
}

void ui_set_now_playing(const char *track_id, bool playing)
{
    board_lock(-1);
    bool changed = strcmp(track_id ? track_id : "", s_np_id) != 0 || playing != s_np_playing;
    kp_strlcpy(s_np_id, track_id ? track_id : "", sizeof(s_np_id));
    s_np_playing = playing;
    if (changed) update_badges();
    board_unlock();
}

void ui_show_volume(int vol)
{
    board_lock(-1);
    lv_bar_set_value(s_vol_bar, vol, LV_ANIM_OFF);
    lv_obj_set_hidden(s_vol_box, false);
    lv_timer_reset(s_vol_timer);
    lv_timer_resume(s_vol_timer);
    board_unlock();
}

void ui_toast(const char *text)
{
    board_lock(-1);
    lv_label_set_text(s_toast, text);
    lv_obj_set_hidden(s_toast, false);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
    board_unlock();
}

void ui_set_battery(int pct, bool charging)
{
    board_lock(-1);
    if (pct < 0) {
        lv_obj_set_hidden(s_battery, true);
    } else {
        const char *sym = pct > 85 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3
                        : pct > 35 ? LV_SYMBOL_BATTERY_2 : pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_battery, "%s%s", charging ? LV_SYMBOL_CHARGE " " : "", sym);
        lv_obj_set_style_text_color(s_battery, pct <= 12 && !charging ? lv_color_hex(0xFF5252) : COL_MUTED, 0);
        lv_obj_set_hidden(s_battery, false);
    }
    board_unlock();
}

void ui_wake(void)
{
    board_lock(-1);
    lv_display_trigger_activity(NULL);
    s_desired_brightness = 100;
    board_unlock();
}
