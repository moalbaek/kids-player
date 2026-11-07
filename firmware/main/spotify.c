#include "spotify.h"
#include "config.h"
#include "settings.h"
#include "kp_util.h"
#include "http_util.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "spotify";

static SemaphoreHandle_t s_mutex;
static char s_access[512];
static int64_t s_access_expiry_us;
static char s_me_id[64];  // Spotify user id of the logged-in account

// Pending login (one at a time; valid for 10 minutes).
static char s_verifier[65];
static char s_state[80];
static int64_t s_login_started_us;

static void ensure_mutex(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
}

void spotify_init(void) { ensure_mutex(); }

bool spotify_logged_in(void)
{
    return g_set.refresh_token[0] != 0;
}

void spotify_logout(void)
{
    ensure_mutex();
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    settings_lock();
    g_set.refresh_token[0] = 0;
    settings_unlock();
    settings_save();
    s_access[0] = 0;
    s_access_expiry_us = 0;
    s_me_id[0] = 0;
    xSemaphoreGiveRecursive(s_mutex);
}

static void random_string(char *out, size_t n, const char *alphabet)
{
    size_t al = strlen(alphabet);
    for (size_t i = 0; i < n; i++) out[i] = alphabet[esp_random() % al];
    out[n] = 0;
}

bool spotify_begin_login(const char *device_ip, char *url, size_t urllen)
{
    ensure_mutex();
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    random_string(s_verifier, 64, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
    char nonce[33];
    random_string(nonce, 32, "0123456789abcdef");
    snprintf(s_state, sizeof(s_state), "%s~%s", nonce, device_ip);
    s_login_started_us = esp_timer_get_time();

    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)s_verifier, strlen(s_verifier), digest, 0);
    char challenge[48];
    kp_base64url(digest, sizeof(digest), challenge, sizeof(challenge));

    char redirect_enc[160], scope_enc[128], state_enc[128];
    bool ok = kp_url_encode(KP_REDIRECT_URI, redirect_enc, sizeof(redirect_enc)) &&
              kp_url_encode(KP_SPOTIFY_SCOPES, scope_enc, sizeof(scope_enc)) &&
              kp_url_encode(s_state, state_enc, sizeof(state_enc));
    if (ok) {
        int n = snprintf(url, urllen,
                         "https://accounts.spotify.com/authorize?response_type=code&client_id=%s"
                         "&redirect_uri=%s&scope=%s&state=%s&code_challenge_method=S256&code_challenge=%s",
                         KP_SPOTIFY_CLIENT_ID, redirect_enc, scope_enc, state_enc, challenge);
        ok = n > 0 && (size_t)n < urllen;
    }
    xSemaphoreGiveRecursive(s_mutex);
    return ok;
}

// POST to the token endpoint with a form body. Fills access token; stores a new refresh token if one is returned.
static sp_result_t token_request(const char *form)
{
    static const http_header_t hdr[] = {{"Content-Type", "application/x-www-form-urlencoded"}};
    http_resp_t r;
    esp_err_t err = http_request("POST", "https://accounts.spotify.com/api/token", hdr, 1, form, strlen(form),
                                 16 * 1024, 15000, &r);
    if (err != ESP_OK || r.status < 0) {
        http_resp_free(&r);
        return SP_ERR_NETWORK;
    }
    sp_result_t res = SP_ERR_OTHER;
    if (r.status == 200) {
        char refresh[sizeof(g_set.refresh_token)];
        int expires = 3600;
        if (sp_parse_token(r.data, s_access, sizeof(s_access), refresh, sizeof(refresh), &expires)) {
            s_access_expiry_us = esp_timer_get_time() + (int64_t)(expires - 60) * 1000000;
            if (refresh[0] && strcmp(refresh, g_set.refresh_token) != 0) {
                settings_lock();
                kp_strlcpy(g_set.refresh_token, refresh, sizeof(g_set.refresh_token));
                settings_unlock();
                settings_save();
            }
            res = SP_OK;
        }
    } else {
        char e[96];
        sp_parse_error(r.data ? r.data : "", e, sizeof(e));
        ESP_LOGW(TAG, "token endpoint %d: %s", r.status, e);
        // invalid_grant = refresh token expired/revoked (6-month limit) or code reused.
        if (r.status == 400 || r.status == 401) res = SP_ERR_LOGGED_OUT;
    }
    http_resp_free(&r);
    return res;
}

sp_result_t spotify_finish_login(const char *code, const char *state)
{
    ensure_mutex();
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    sp_result_t res = SP_ERR_OTHER;
    bool fresh = s_state[0] && (esp_timer_get_time() - s_login_started_us) < 10LL * 60 * 1000000;
    if (!fresh || strcmp(state, s_state) != 0) {
        ESP_LOGW(TAG, "login state mismatch or expired");
        xSemaphoreGiveRecursive(s_mutex);
        return SP_ERR_OTHER;
    }
    char code_enc[1024], redirect_enc[160];
    char *form = malloc(2048);
    if (form && kp_url_encode(code, code_enc, sizeof(code_enc)) &&
        kp_url_encode(KP_REDIRECT_URI, redirect_enc, sizeof(redirect_enc))) {
        snprintf(form, 2048,
                 "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
                 code_enc, redirect_enc, KP_SPOTIFY_CLIENT_ID, s_verifier);
        // Clear the stored refresh token first so a successful exchange always stores the new one.
        settings_lock();
        g_set.refresh_token[0] = 0;
        settings_unlock();
        s_me_id[0] = 0;  // possibly another account
        res = token_request(form);
        if (res == SP_OK && !g_set.refresh_token[0]) res = SP_ERR_OTHER;
    }
    free(form);
    s_state[0] = 0;  // single use
    xSemaphoreGiveRecursive(s_mutex);
    ESP_LOGI(TAG, "login %s", res == SP_OK ? "ok" : "failed");
    return res;
}

static sp_result_t ensure_access(bool force)
{
    if (!spotify_logged_in()) return SP_ERR_LOGGED_OUT;
    if (!force && s_access[0] && esp_timer_get_time() < s_access_expiry_us) return SP_OK;
    char tok_enc[700];
    if (!kp_url_encode(g_set.refresh_token, tok_enc, sizeof(tok_enc))) return SP_ERR_OTHER;
    char form[900];
    snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s", tok_enc,
             KP_SPOTIFY_CLIENT_ID);
    sp_result_t r = token_request(form);
    if (r == SP_ERR_LOGGED_OUT) {
        ESP_LOGW(TAG, "refresh token no longer valid: logging out");
        settings_lock();
        g_set.refresh_token[0] = 0;
        settings_unlock();
        settings_save();
        s_access[0] = 0;
    }
    return r;
}

// GET a Web API URL with the access token. Retries once after a 401 (refresh) and once after a 429.
static sp_result_t api_get(const char *url, http_resp_t *out)
{
    memset(out, 0, sizeof(*out));
    ensure_mutex();
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    sp_result_t res = ensure_access(false);
    for (int attempt = 0; res == SP_OK && attempt < 3; attempt++) {
        char auth[540];
        snprintf(auth, sizeof(auth), "Bearer %s", s_access);
        http_header_t hdr[] = {{"Authorization", auth}, {"Accept", "application/json"}};
        esp_err_t err = http_request("GET", url, hdr, 2, NULL, 0, 512 * 1024, 15000, out);
        if (err != ESP_OK || out->status < 0) {
            http_resp_free(out);
            res = SP_ERR_NETWORK;
            break;
        }
        if (out->status == 200) break;
        int status = out->status, wait = out->retry_after;
        http_resp_free(out);
        if (status == 401 && attempt == 0) {
            res = ensure_access(true);
            continue;
        }
        if (status == 429 && attempt < 2) {
            vTaskDelay(pdMS_TO_TICKS(1000 * (wait > 0 && wait < 10 ? wait : 3)));
            continue;
        }
        res = (status == 401) ? SP_ERR_LOGGED_OUT : SP_ERR_OTHER;
        out->status = status;
        break;
    }
    xSemaphoreGiveRecursive(s_mutex);
    return res;
}

static sp_result_t fetch_me(char *name, size_t len)
{
    if (name && len) name[0] = 0;
    http_resp_t r;
    sp_result_t res = api_get("https://api.spotify.com/v1/me", &r);
    if (res != SP_OK) return res;
    cJSON *root = cJSON_Parse(r.data);
    const cJSON *dn = root ? cJSON_GetObjectItemCaseSensitive(root, "display_name") : NULL;
    const cJSON *id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
    if (cJSON_IsString(id)) kp_strlcpy(s_me_id, id->valuestring, sizeof(s_me_id));
    if (name && len) {
        if (cJSON_IsString(dn) && dn->valuestring[0]) kp_strlcpy(name, dn->valuestring, len);
        else if (cJSON_IsString(id)) kp_strlcpy(name, id->valuestring, len);
    }
    cJSON_Delete(root);
    http_resp_free(&r);
    return SP_OK;
}

sp_result_t spotify_get_display_name(char *out, size_t len) { return fetch_me(out, len); }

// Fetch up to max_pages pages starting at url. parse() appends entries and fills the next URL.
typedef bool (*page_parser_t)(const char *json, const void *ctx, sp_entry_t *out, int *count, int max, char *next,
                              size_t nextlen);

static sp_result_t fetch_pages(const char *first_url, int max_pages, page_parser_t parse, const void *ctx,
                               sp_entry_t *out, int max, int *count)
{
    *count = 0;
    char *url = malloc(768), *next = malloc(768);
    if (!url || !next) {
        free(url);
        free(next);
        return SP_ERR_OTHER;
    }
    kp_strlcpy(url, first_url, 768);
    sp_result_t res = SP_OK;
    for (int page = 0; page < max_pages && url[0] && *count < max; page++) {
        http_resp_t r;
        res = api_get(url, &r);
        if (res != SP_OK) break;
        next[0] = 0;
        if (!parse(r.data, ctx, out, count, max, next, 768)) res = SP_ERR_OTHER;
        http_resp_free(&r);
        if (res != SP_OK) break;
        kp_strlcpy(url, next, 768);
    }
    free(url);
    free(next);
    return res;
}

typedef struct {
    const char *prefix;
} playlists_ctx_t;

static bool parse_playlists_page(const char *json, const void *ctx, sp_entry_t *out, int *count, int max, char *next,
                                 size_t nextlen)
{
    return sp_parse_playlists(json, ((const playlists_ctx_t *)ctx)->prefix, s_me_id, out, count, max, next, nextlen);
}

sp_result_t spotify_get_playlists(sp_playlist_t *out, int max, int *count)
{
    *count = 0;
    char prefix[32];
    settings_lock();
    kp_strlcpy(prefix, g_set.full_library ? "" : g_set.prefix, sizeof(prefix));
    settings_unlock();
    if (!s_me_id[0]) {
        sp_result_t r = fetch_me(NULL, 0);  // needed to tell own playlists from followed ones
        if (r != SP_OK) return r;
    }
    playlists_ctx_t ctx = {prefix};
    sp_result_t res = fetch_pages("https://api.spotify.com/v1/me/playlists?limit=50", 6, parse_playlists_page, &ctx,
                                  out, max, count);
    ESP_LOGI(TAG, "%d playlist(s)%s%s", *count, prefix[0] ? " starting with " : "", prefix);
    return res;
}

static bool parse_albums_page(const char *json, const void *ctx, sp_entry_t *out, int *count, int max, char *next,
                              size_t nextlen)
{
    return sp_parse_saved_albums(json, out, count, max, next, nextlen);
}

sp_result_t spotify_get_saved_albums(sp_entry_t *out, int max, int *count)
{
    sp_result_t res = fetch_pages("https://api.spotify.com/v1/me/albums?limit=50", 2, parse_albums_page, NULL, out,
                                  max, count);
    ESP_LOGI(TAG, "%d saved album(s)", *count);
    return res;
}

typedef struct {
    const char *album_id, *img;
} album_ctx_t;

static bool parse_album_tracks_page(const char *json, const void *ctx, sp_entry_t *out, int *count, int max,
                                    char *next, size_t nextlen)
{
    const album_ctx_t *c = ctx;
    return sp_parse_album_tracks(json, c->album_id, c->img, out, count, max, next, nextlen);
}

sp_result_t spotify_get_album_tracks(const char *album_id, const char *album_img, sp_entry_t *out, int max,
                                     int *count)
{
    char url[96];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/albums/%s/tracks?limit=50", album_id);
    album_ctx_t ctx = {album_id, album_img};
    sp_result_t res = fetch_pages(url, 3, parse_album_tracks_page, &ctx, out, max, count);
    ESP_LOGI(TAG, "album %s: %d song(s)", album_id, *count);
    return res;
}

static bool parse_tracks_page(const char *json, const void *ctx, sp_entry_t *out, int *count, int max, char *next,
                              size_t nextlen)
{
    return sp_parse_tracks(json, out, count, max, next, nextlen, NULL);
}

sp_result_t spotify_get_liked(sp_entry_t *out, int max, int *count)
{
    sp_result_t res =
        fetch_pages("https://api.spotify.com/v1/me/tracks?limit=50", 3, parse_tracks_page, NULL, out, max, count);
    ESP_LOGI(TAG, "%d liked song(s)", *count);
    return res;
}

sp_result_t spotify_search(const char *query, sp_entry_t *out, int max, int *count)
{
    *count = 0;
    char q[200], url[320];
    if (!kp_url_encode(query, q, sizeof(q))) return SP_ERR_OTHER;
    // Development Mode allows at most 10 results per type.
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/search?q=%s&type=track,album,playlist&limit=10", q);
    if (!s_me_id[0]) fetch_me(NULL, 0);
    http_resp_t r;
    sp_result_t res = api_get(url, &r);
    if (res != SP_OK) return res;
    if (!sp_parse_search(r.data, s_me_id, out, count, max)) res = SP_ERR_OTHER;
    http_resp_free(&r);
    ESP_LOGI(TAG, "search '%s': %d result(s)", query, *count);
    return res;
}

sp_result_t spotify_get_tracks(const char *playlist_id, sp_track_t *out, int max, int *count)
{
    *count = 0;
    char *url = malloc(768);
    char *next = malloc(768);
    if (!url || !next) { free(url); free(next); return SP_ERR_OTHER; }
    // Ask only for the fields we use (the field was renamed from "track" to "item" in 2026).
    static const char *fields = "items(is_local,item(id,name,type,is_local,artists(name),album(images))),next";
    char fields_enc[200];
    kp_url_encode(fields, fields_enc, sizeof(fields_enc));
    bool use_fields = true;
    snprintf(url, 768, "https://api.spotify.com/v1/playlists/%s/items?limit=50&fields=%s", playlist_id, fields_enc);
    sp_result_t res = SP_OK;
    for (int page = 0; page < 6 && url[0] && *count < max; page++) {
        http_resp_t r;
        res = api_get(url, &r);
        if (res == SP_ERR_OTHER && use_fields && page == 0) {
            // The fields filter was rejected: fall back to full objects.
            use_fields = false;
            snprintf(url, 768, "https://api.spotify.com/v1/playlists/%s/items?limit=50", playlist_id);
            res = api_get(url, &r);
        }
        if (res != SP_OK) break;
        int raw = 0, before = *count;
        if (!sp_parse_tracks(r.data, out, count, max, next, 768, &raw)) res = SP_ERR_OTHER;
        http_resp_free(&r);
        if (res != SP_OK) break;
        if (use_fields && page == 0 && raw > 0 && *count == before) {
            // Entries came back but none parsed: the filter dropped what we need. Retry without it.
            use_fields = false;
            snprintf(url, 768, "https://api.spotify.com/v1/playlists/%s/items?limit=50", playlist_id);
            page = -1;
            continue;
        }
        kp_strlcpy(url, next, 768);
    }
    free(url);
    free(next);
    ESP_LOGI(TAG, "playlist %s: %d song(s)", playlist_id, *count);
    return res;
}

