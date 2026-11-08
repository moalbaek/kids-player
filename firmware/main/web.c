#include "web.h"
#include "app.h"
#include "app_state.h"
#include "config.h"
#include "settings.h"
#include "spotify.h"
#include "wifi.h"
#include "kp_util.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>

static const char *TAG = "web";
static char s_scan[16][33];
static int s_nscan;
static atomic_bool s_portal;

#define ACCESS_KEY_LEN 22  // 128 bits in unpadded base64url
#define SETTINGS_SECONDS 300
#define SETTINGS_COOKIE "kp_settings="
#define SETTINGS_LOCKED "Hold BOOT on the player and scan the settings QR code."

// The settings mutex also protects these keys from concurrent app/HTTP task access.
static char s_settings_key[ACCESS_KEY_LEN + 1], s_login_key[ACCESS_KEY_LEN + 1];
static int64_t s_settings_until;
// Held from a handler's session check until its change is done, and by web_close_settings(), so Done can't
// close the session in between. Taken before the Spotify and settings locks (spotify_logout takes both).
static SemaphoreHandle_t s_session_mutex;

void web_set_scan_results(char ssids[][33], int n)
{
    s_nscan = n > 16 ? 16 : n;
    for (int i = 0; i < s_nscan; i++) kp_strlcpy(s_scan[i], ssids[i], 33);
}

void web_set_portal_mode(bool on) { s_portal = on; }

static void open_link(char *key, const char *path, char *url, size_t len)
{
    char ip[16];
    uint8_t random[16];
    wifi_ip(ip, sizeof(ip));
    esp_fill_random(random, sizeof(random));
    kp_base64url(random, sizeof(random), key, ACCESS_KEY_LEN + 1);
    snprintf(url, len, "http://%s%s?key=%s", ip, path, key);
}

void web_open_settings(char *url, size_t len)
{
    settings_lock();
    open_link(s_settings_key, "/", url, len);
    s_settings_until = esp_timer_get_time() + SETTINGS_SECONDS * 1000000LL;
    settings_unlock();
}

bool web_settings_open(void)
{
    settings_lock();
    bool open = s_settings_key[0] && esp_timer_get_time() < s_settings_until;
    settings_unlock();
    return open;
}

void web_close_settings(void)
{
    if (s_session_mutex) xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    settings_lock();
    s_settings_key[0] = 0;
    settings_unlock();
    if (s_session_mutex) xSemaphoreGive(s_session_mutex);
}

void web_open_login(char *url, size_t len)
{
    settings_lock();
    open_link(s_login_key, "/login", url, len);
    settings_unlock();
}

void web_close_login(void)
{
    settings_lock();
    s_login_key[0] = 0;
    settings_unlock();
}

static const char *PAGE_HEAD =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>" KP_DEVICE_NAME "</title><style>"
    ":root{--bg:#f6f4ef;--card:#fff;--text:#1d1b18;--muted:#6b665d;--accent:#1db954}"
    "@media(prefers-color-scheme:dark){:root{--bg:#121212;--card:#1e1e1e;--text:#f2f2f2;--muted:#a7a7a7;--accent:#1ed760}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
    "font:17px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding:16px}"
    "main{max-width:560px;margin:0 auto}h1{font-size:24px;margin:8px 0 16px}"
    "section{background:var(--card);border-radius:16px;padding:18px;margin-bottom:14px}"
    "h2{font-size:18px;margin:0 0 10px}p,.muted{color:var(--muted)}label{display:block;margin:10px 0 4px;font-weight:600}"
    "input,select{width:100%;font:inherit;padding:10px 12px;border-radius:10px;border:1px solid #8884;"
    "background:var(--bg);color:var(--text)}input[type=checkbox]{width:auto;margin-right:8px}"
    "button,.btn{display:inline-block;margin-top:14px;padding:12px 18px;border:0;border-radius:999px;"
    "background:var(--accent);color:#05240f;font:inherit;font-weight:700;text-decoration:none;cursor:pointer}"
    ".secondary{background:#8883;color:var(--text)}.row{display:flex;gap:8px;flex-wrap:wrap}"
    "code{font-size:15px}</style></head><body><main>";
static const char *PAGE_TAIL = "</main></body></html>";

static void send_str(httpd_req_t *req, const char *s) { httpd_resp_sendstr_chunk(req, s); }

static void send_esc(httpd_req_t *req, const char *s)
{
    char buf[640];
    if (kp_xml_escape(s ? s : "", buf, sizeof(buf))) send_str(req, buf);
}

static void sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_str(req, buf);
}

static void page_begin(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    send_str(req, PAGE_HEAD);
}

static esp_err_t page_end(httpd_req_t *req)
{
    send_str(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t simple_page(httpd_req_t *req, const char *title, const char *text, bool back_link)
{
    page_begin(req);
    send_str(req, "<section><h1>");
    send_esc(req, title);
    send_str(req, "</h1><p>");
    send_esc(req, text);
    send_str(req, "</p>");
    if (back_link) send_str(req, "<a class=btn href='/'>Back to settings</a>");
    send_str(req, "</section>");
    return page_end(req);
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", to);
    return httpd_resp_send(req, NULL, 0);
}

static bool portal_request(httpd_req_t *req)
{
    if (!s_portal) return false;
    // During AP+STA operation, the HTTP server also listens on the home network.
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0) return false;
    in_addr_t ap = inet_addr(KP_SETUP_IP);
    if (local.ss_family == AF_INET) return ((struct sockaddr_in *)&local)->sin_addr.s_addr == ap;
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct in6_addr *ip = &((struct sockaddr_in6 *)&local)->sin6_addr;
        return IN6_IS_ADDR_V4MAPPED(ip) && memcmp(ip->s6_addr + 12, &ap, sizeof(ap)) == 0;
    }
#endif
    return false;
}

static bool setup_host(httpd_req_t *req)
{
    char host[32];
    return httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
           (!strcmp(host, KP_SETUP_IP) || !strcmp(host, KP_SETUP_IP ":80"));
}

static bool query_key(httpd_req_t *req, char key[ACCESS_KEY_LEN + 1])
{
    char query[64];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "key", key, ACCESS_KEY_LEN + 1) == ESP_OK;
}

// Each use renews the session: it ends on Done, or after SETTINGS_SECONDS without a request from the phone.
static bool settings_key_matches(const char *key, size_t len)
{
    if (len != ACCESS_KEY_LEN) return false;
    settings_lock();
    int64_t now = esp_timer_get_time();
    bool ok = !s_portal && s_settings_key[0] && now < s_settings_until &&
              memcmp(s_settings_key, key, ACCESS_KEY_LEN) == 0;
    if (ok) s_settings_until = now + SETTINGS_SECONDS * 1000000LL;
    settings_unlock();
    return ok;
}

// The browser drops the cookie SETTINGS_SECONDS after it was last set, so renew it along with the session.
// The header must outlive this call; handlers run one at a time on the server task, so one buffer is enough.
static void set_session_cookie(httpd_req_t *req, const char *key)
{
    static char header[120];
    snprintf(header, sizeof(header), SETTINGS_COOKIE "%.*s; Max-Age=%d; Path=/; HttpOnly; SameSite=Lax",
             ACCESS_KEY_LEN, key, SETTINGS_SECONDS);
    httpd_resp_set_hdr(req, "Set-Cookie", header);
}

// Finds the kp_settings cookie with the current key and copies it to key. Any of them may: a browser can
// still send one from an older session.
static bool settings_cookie_key(httpd_req_t *req, char key[ACCESS_KEY_LEN + 1])
{
    char cookies[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK) return false;
    const size_t name_len = sizeof(SETTINGS_COOKIE) - 1;
    for (char *p = cookies; p; ) {
        while (*p == ' ') p++;
        size_t len = strcspn(p, ";");
        if (len == name_len + ACCESS_KEY_LEN && !memcmp(p, SETTINGS_COOKIE, name_len) &&
            settings_key_matches(p + name_len, ACCESS_KEY_LEN)) {
            kp_strlcpy(key, p + name_len, ACCESS_KEY_LEN + 1);
            return true;
        }
        p = p[len] ? p + len + 1 : NULL;
    }
    return false;
}

static bool settings_authorized(httpd_req_t *req)
{
    char key[ACCESS_KEY_LEN + 1];
    return settings_cookie_key(req, key);
}

static bool login_authorized(httpd_req_t *req)
{
    if (settings_authorized(req)) return true;
    char key[ACCESS_KEY_LEN + 1];
    if (!query_key(req, key)) return false;
    settings_lock();
    bool ok = !s_portal && s_login_key[0] && strcmp(key, s_login_key) == 0;
    settings_unlock();
    return ok;
}

// Read a urlencoded POST body (small forms only).
static char *read_body(httpd_req_t *req)
{
    if (req->content_len > 4096) return NULL;
    char *b = calloc(1, req->content_len + 1);
    if (!b) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    return b;
}

// Values arrive percent-encoded (up to 3x longer), so decode from a larger buffer before the length check.
static bool form_value(const char *body, const char *key, char *out, size_t len)
{
    char tmp[256];
    if (httpd_query_key_value(body, key, tmp, sizeof(tmp)) != ESP_OK) return false;
    kp_url_decode(tmp);
    if (strlen(tmp) >= len) return false;
    strcpy(out, tmp);
    return true;
}

// Refuse form posts made by other web sites (a browser sends their Origin). Requests without Origin are allowed.
static bool same_origin(httpd_req_t *req)
{
    char origin[96], host[64];
    esp_err_t e = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (e == ESP_ERR_NOT_FOUND) return true;
    if (e != ESP_OK || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    return !strncmp(origin, "http://", 7) && strcmp(origin + 7, host) == 0;
}

static bool valid_ipv4(const char *s)
{
    int dots = 0, digits = 0;
    for (; *s; s++) {
        if (*s == '.') {
            if (!digits) return false;
            dots++;
            digits = 0;
        } else if (*s >= '0' && *s <= '9') {
            if (++digits > 3) return false;
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0;
}

#define REQUIRE_SAME_ORIGIN(req) \
    do { \
        if (!same_origin(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open the settings page directly."); \
    } while (0)

// Check the settings session and keep Done from closing it until session_end(). Sends the 403 itself.
static bool session_begin(httpd_req_t *req)
{
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    char key[ACCESS_KEY_LEN + 1];
    if (settings_cookie_key(req, key)) {
        set_session_cookie(req, key);
        return true;
    }
    xSemaphoreGive(s_session_mutex);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, SETTINGS_LOCKED);
    return false;
}

static void session_end(void) { xSemaphoreGive(s_session_mutex); }

#define BEGIN_SETTINGS_CHANGE(req) \
    do { \
        REQUIRE_SAME_ORIGIN(req); \
        if (!session_begin(req)) return ESP_OK; \
    } while (0)

// ---------------------------------------------------------------- Wi-Fi setup portal

static esp_err_t portal_page(httpd_req_t *req)
{
    page_begin(req);
    send_str(req, "<h1>🎵 " KP_DEVICE_NAME " setup</h1><section><h2>Home Wi-Fi</h2>"
                  "<p>Choose the network the Sonos speakers are on.</p><form method=post action='/wifi'>"
                  "<label for=s>Network</label><input id=s name=ssid list=nets required autocomplete=off "
                  "placeholder='Network name'><datalist id=nets>");
    for (int i = 0; i < s_nscan; i++) {
        send_str(req, "<option value=\"");
        send_esc(req, s_scan[i]);
        send_str(req, "\">");
    }
    send_str(req, "</datalist><label for=p>Password</label><input id=p name=pass type=password "
                  "autocomplete=off><button type=submit>Save and connect</button></form></section>");
    return page_end(req);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!portal_request(req) || !setup_host(req))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Join the player's setup Wi-Fi first.");
    REQUIRE_SAME_ORIGIN(req);
    char *body = read_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    char ssid[33] = "", pass[65] = "";
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    free(body);
    if (!ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing network name");
    settings_lock();
    kp_strlcpy(g_set.wifi_ssid, ssid, sizeof(g_set.wifi_ssid));
    kp_strlcpy(g_set.wifi_pass, pass, sizeof(g_set.wifi_pass));
    settings_unlock();
    settings_save();
    simple_page(req, "Saved", "The player restarts and joins your Wi-Fi. Reconnect your phone to your home "
                              "Wi-Fi, then follow the instructions on the player.", false);
    app_post(CMD_RESTART, 0, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------- settings page

static esp_err_t settings_page(httpd_req_t *req)
{
    if (portal_request(req)) {
        if (!setup_host(req)) return redirect(req, "http://" KP_SETUP_IP "/");
        return portal_page(req);
    }
    char key[ACCESS_KEY_LEN + 1];
    if (query_key(req, key) && settings_key_matches(key, strlen(key))) {
        set_session_cookie(req, key);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
        return redirect(req, "/");
    }
    if (!settings_cookie_key(req, key)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return simple_page(req, "Settings are locked", SETTINGS_LOCKED, false);
    }
    set_session_cookie(req, key);
    app_snapshot_t *snap = heap_caps_malloc(sizeof(app_snapshot_t), MALLOC_CAP_SPIRAM);
    if (!snap) return httpd_resp_send_500(req);
    app_get_snapshot(snap);
    char ip[16];
    wifi_ip(ip, sizeof(ip));
    page_begin(req);
    send_str(req, "<h1>🎵 " KP_DEVICE_NAME "</h1>");

    send_str(req, "<section><h2>Spotify</h2>");
    if (spotify_logged_in()) {
        send_str(req, "<p>Logged in");
        if (snap->spotify_user[0]) {
            send_str(req, " as <b>");
            send_esc(req, snap->spotify_user);
            send_str(req, "</b>");
        }
        sendf(req, ". %d playlist(s) found.</p>", snap->nplaylists);
        send_str(req, "<div class=row><a class=btn href='/login'>Log in again</a>"
                      "<form method=post action='/logout'><button class=secondary>Log out</button></form></div>");
    } else {
        send_str(req, "<p>Not logged in.</p><a class=btn href='/login'>Log in to Spotify</a>");
    }
    send_str(req, "</section>");

    send_str(req, "<section><h2>Settings</h2><form method=post action='/settings'>"
                  "<label for=room>Speaker</label><select id=room name=room>");
    bool listed = false;
    for (int i = 0; i < snap->nrooms; i++) {
        bool sel = strcmp(snap->rooms[i].uid, g_set.speaker_uid) == 0;
        listed |= sel;
        sendf(req, "<option value='%s'%s>", snap->rooms[i].uid, sel ? " selected" : "");
        send_esc(req, snap->rooms[i].name);
        send_str(req, "</option>");
    }
    if (!listed) {
        send_str(req, "<option value='' selected>");
        send_esc(req, g_set.speaker_name[0] ? g_set.speaker_name : "Not chosen");
        send_str(req, "</option>");
    }
    send_str(req, "</select><label for=ip>Speaker IP (only if it is not found automatically)</label>"
                  "<input id=ip name=spk_ip inputmode=decimal value='");
    send_esc(req, g_set.speaker_ip);
    sendf(req, "'><label><input type=checkbox name=full value=1%s>Whole library: all playlists, saved albums, "
               "Liked Songs and search</label>",
          g_set.full_library ? " checked" : "");
    send_str(req, "<label for=prefix>Otherwise only show playlists whose name starts with</label>"
                  "<input id=prefix name=prefix value='");
    send_esc(req, g_set.prefix);
    sendf(req, "'><label for=mv>Maximum volume from the player (0-100)</label>"
               "<input id=mv name=max_vol type=number min=5 max=100 value='%d'>", g_set.max_vol);
#if CONFIG_KP_BOARD_CUBE
    sendf(req, "<label><input type=checkbox name=swap value=1%s>Swap the volume buttons</label>",
          g_set.swap_buttons ? " checked" : "");
#else
    sendf(req, "<label><input type=checkbox name=flip value=1%s>Turn the picture upside down</label>",
          g_set.flip ? " checked" : "");
#endif
    send_str(req, "<button type=submit>Save</button></form></section>");

    const esp_app_desc_t *d = esp_app_get_description();
    sendf(req, "<section><h2>About</h2><p>Firmware %s (%s) · IP %s · free memory %u KB</p>"
               "<div class=row><form method=post action='/update'><button class=secondary>Check for update</button></form>"
               "<form method=post action='/forget-wifi' onsubmit=\"return confirm('Forget the Wi-Fi and restart in setup mode?')\">"
               "<button class=secondary>Change Wi-Fi</button></form></div></section>",
          d->version, CONFIG_KP_BOARD_NAME, ip, (unsigned)(esp_get_free_heap_size() / 1024));
    heap_caps_free(snap);
    return page_end(req);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    REQUIRE_SAME_ORIGIN(req);  // the session is checked below, after the slow part (reading the body)
    char *body = read_body(req);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    char room[48] = "", ip[16] = "", prefix[32] = "", mv[8] = "", chk[4] = "";
    form_value(body, "room", room, sizeof(room));
    form_value(body, "spk_ip", ip, sizeof(ip));
    form_value(body, "prefix", prefix, sizeof(prefix));
    form_value(body, "max_vol", mv, sizeof(mv));
    app_snapshot_t *snap = heap_caps_malloc(sizeof(app_snapshot_t), MALLOC_CAP_SPIRAM);
    if (snap) app_get_snapshot(snap);
    if (!session_begin(req)) {
        free(body);
        heap_caps_free(snap);
        return ESP_OK;
    }
    settings_lock();
    if (room[0] && snap) {
        for (int i = 0; i < snap->nrooms; i++) {
            if (strcmp(snap->rooms[i].uid, room) == 0) {
                kp_strlcpy(g_set.speaker_uid, room, sizeof(g_set.speaker_uid));
                kp_strlcpy(g_set.speaker_name, snap->rooms[i].name, sizeof(g_set.speaker_name));
                kp_strlcpy(g_set.speaker_ip, snap->rooms[i].ip, sizeof(g_set.speaker_ip));
            }
        }
    } else if (ip[0] && valid_ipv4(ip) && strcmp(ip, g_set.speaker_ip) != 0) {
        kp_strlcpy(g_set.speaker_ip, ip, sizeof(g_set.speaker_ip));
        g_set.speaker_uid[0] = 0;  // the app looks the room up on this IP
    }
    if (prefix[0]) kp_strlcpy(g_set.prefix, prefix, sizeof(g_set.prefix));
    int v = atoi(mv);
    if (v >= 5 && v <= 100) g_set.max_vol = v;
    g_set.full_library = form_value(body, "full", chk, sizeof(chk)) ? 1 : 0;
#if CONFIG_KP_BOARD_CUBE
    g_set.swap_buttons = form_value(body, "swap", chk, sizeof(chk)) ? 1 : 0;
#else
    g_set.flip = form_value(body, "flip", chk, sizeof(chk)) ? 1 : 0;
#endif
    settings_unlock();
    settings_save();
    session_end();
    free(body);
    if (snap) heap_caps_free(snap);
    app_post(CMD_SETTINGS_CHANGED, 0, 0);
    return redirect(req, "/");
}

static esp_err_t logout_post(httpd_req_t *req)
{
    BEGIN_SETTINGS_CHANGE(req);
    spotify_logout();
    session_end();
    app_post(CMD_SETTINGS_CHANGED, 0, 0);
    return redirect(req, "/");
}

static esp_err_t update_post(httpd_req_t *req)
{
    BEGIN_SETTINGS_CHANGE(req);
    app_post(CMD_OTA_CHECK, 1, 0);
    session_end();
    return simple_page(req, "Checking for updates",
                       "If there is a new version, the player shows \"Updating\" and restarts by itself.", true);
}

static esp_err_t forget_wifi_post(httpd_req_t *req)
{
    BEGIN_SETTINGS_CHANGE(req);
    settings_lock();
    g_set.wifi_ssid[0] = 0;
    g_set.wifi_pass[0] = 0;
    settings_unlock();
    settings_save();
    session_end();
    simple_page(req, "Restarting", "The player starts in Wi-Fi setup mode. Follow the instructions on its screen.",
                false);
    app_post(CMD_RESTART, 0, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------- Spotify login

static esp_err_t login_get(httpd_req_t *req)
{
    if (!login_authorized(req))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Scan the login QR code on the player.");
    char ip[16];
    wifi_ip(ip, sizeof(ip));
    if (!ip[0]) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not on Wi-Fi");
    char *url = malloc(1024);
    if (!url) return httpd_resp_send_500(req);
    if (!spotify_begin_login(ip, url, 1024)) {
        free(url);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    esp_err_t r = httpd_resp_send(req, NULL, 0);
    free(url);
    return r;
}

static esp_err_t callback_get(httpd_req_t *req)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 1400) return simple_page(req, "Login failed", "The login link was incomplete.", true);
    char *q = malloc(qlen + 1), *code = malloc(1024);
    char state[96] = "";
    if (!q || !code) {
        free(q);
        free(code);
        return httpd_resp_send_500(req);
    }
    httpd_req_get_url_query_str(req, q, qlen + 1);
    bool ok = httpd_query_key_value(q, "code", code, 1024) == ESP_OK &&
              httpd_query_key_value(q, "state", state, sizeof(state)) == ESP_OK;
    free(q);
    esp_err_t r;
    if (!ok) {
        r = simple_page(req, "Login failed", "The login link was incomplete. Scan the QR code again.", true);
    } else {
        kp_url_decode(code);
        kp_url_decode(state);
        sp_result_t res = spotify_finish_login(code, state);
        if (res == SP_OK) {
            web_close_login();
            app_post(CMD_LOGIN_DONE, 0, 0);
            r = simple_page(req, "✅ Logged in",
                            "Spotify is connected. You can close this page and look at the player.", true);
        } else if (res == SP_ERR_NETWORK) {
            r = simple_page(req, "Login failed", "The player could not reach Spotify. Check its Wi-Fi and try again.",
                            true);
        } else {
            r = simple_page(req, "Login failed",
                            "The login link expired or was already used. Scan the QR code on the player again.", true);
        }
    }
    free(code);
    return r;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char ip[16], buf[400];
    wifi_ip(ip, sizeof(ip));
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"board\":\"%s\",\"ip\":\"%s\",\"spotify\":%s,\"room\":\"%s\",\"free_heap\":%u}",
             d->version, CONFIG_KP_BOARD_NAME, ip, spotify_logged_in() ? "true" : "false", g_set.speaker_uid,
             (unsigned)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (portal_request(req)) return redirect(req, "http://" KP_SETUP_IP "/");
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

void web_start(void)
{
    s_session_mutex = xSemaphoreCreateMutex();
    if (!s_session_mutex) {
        ESP_LOGE(TAG, "no memory for the settings session lock");
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 14 * 1024;  // the login callback does a TLS request
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = settings_page},
        {.uri = "/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
        {.uri = "/forget-wifi", .method = HTTP_POST, .handler = forget_wifi_post},
        {.uri = "/login", .method = HTTP_GET, .handler = login_get},
        {.uri = "/callback", .method = HTTP_GET, .handler = callback_get},
        {.uri = "/status.json", .method = HTTP_GET, .handler = status_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web server started");
}
