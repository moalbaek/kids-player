#include "images.h"
#include "config.h"
#include "board.h"
#include "http_util.h"
#include "kp_util.h"
#include "jpeg_decoder.h"
#include "misc/cache/instance/lv_image_cache.h"  // lv_image_cache_drop (not in the public headers in 9.6)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "images";

#define URL_MAX 256
#define PENDING_MAX 8

typedef struct {
    char url[URL_MAX];
    lv_image_dsc_t dsc;
    uint8_t *pixels;
    int refs;
    bool failed;
    TickType_t failed_at;
    uint32_t last_used;
    bool used;
} entry_t;

static entry_t *s_cache;  // KP_IMG_CACHE entries (PSRAM)
static char s_pending[PENDING_MAX][URL_MAX];
static int s_npending;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static uint32_t s_clock;

static entry_t *find(const char *url)
{
    for (int i = 0; i < KP_IMG_CACHE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].url, url) == 0) return &s_cache[i];
    }
    return NULL;
}

const lv_image_dsc_t *images_acquire(const char *url, bool *failed)
{
    if (failed) *failed = false;
    if (!url || !url[0] || !s_cache) {
        if (failed) *failed = true;
        return NULL;
    }
    const lv_image_dsc_t *res = NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    entry_t *e = find(url);
    if (e) {
        e->last_used = ++s_clock;
        if (e->failed) {
            if (xTaskGetTickCount() - e->failed_at > pdMS_TO_TICKS(60000) && e->refs == 0) {
                e->used = false;  // allow another try after a minute
            } else if (failed) {
                *failed = true;
            }
        } else if (e->pixels) {
            e->refs++;
            res = &e->dsc;
        }
    }
    xSemaphoreGive(s_mutex);
    return res;
}

void images_release(const lv_image_dsc_t *dsc)
{
    if (!dsc || !s_cache) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < KP_IMG_CACHE; i++) {
        if (&s_cache[i].dsc == dsc && s_cache[i].refs > 0) {
            s_cache[i].refs--;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

void images_request(const char *url)
{
    if (!url || !url[0] || !s_cache) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool known = find(url) != NULL;
    if (!known) {
        // Remove an existing pending copy, then push to the front (newest first).
        for (int i = 0; i < s_npending; i++) {
            if (strcmp(s_pending[i], url) == 0) {
                memmove(&s_pending[i], &s_pending[i + 1], (size_t)(s_npending - i - 1) * URL_MAX);
                s_npending--;
                break;
            }
        }
        if (s_npending == PENDING_MAX) s_npending--;  // drop the oldest
        memmove(&s_pending[1], &s_pending[0], (size_t)s_npending * URL_MAX);
        kp_strlcpy(s_pending[0], url, URL_MAX);
        s_npending++;
    }
    xSemaphoreGive(s_mutex);
    if (!known) xTaskNotifyGive(s_task);
}

// Pick a free or least-recently-used unreferenced slot. Called with s_mutex held.
static entry_t *take_slot(void)
{
    entry_t *best = NULL;
    for (int i = 0; i < KP_IMG_CACHE; i++) {
        entry_t *e = &s_cache[i];
        if (!e->used) return e;
        if (e->refs == 0 && (!best || e->last_used < best->last_used)) best = e;
    }
    return best;
}

static esp_jpeg_image_scale_t pick_scale(int w)
{
    // Smallest decode that is still at least ~the cover size.
    for (int s = 3; s >= 0; s--) {
        if ((w >> s) >= KP_COVER - 12) return (esp_jpeg_image_scale_t)s;
    }
    return JPEG_IMAGE_SCALE_0;
}

static bool decode(const char *url, uint8_t **pixels, int *w, int *h)
{
    http_resp_t r;
    if (http_request("GET", url, NULL, 0, NULL, 0, 600 * 1024, 15000, &r) != ESP_OK || r.status != 200 || !r.len) {
        ESP_LOGW(TAG, "download failed (%d): %s", r.status, url);
        http_resp_free(&r);
        return false;
    }
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)r.data,
        .indata_size = r.len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0) {
        ESP_LOGW(TAG, "not a baseline JPEG: %s", url);
        http_resp_free(&r);
        return false;
    }
    cfg.out_scale = pick_scale(info.width);
    int ow = info.width >> cfg.out_scale, oh = info.height >> cfg.out_scale;
    size_t sz = (size_t)ow * oh * 2;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        http_resp_free(&r);
        return false;
    }
    cfg.outbuf = buf;
    cfg.outbuf_size = sz;
    cfg.priv.read = 0;
    esp_jpeg_image_output_t out;
    esp_err_t err = esp_jpeg_decode(&cfg, &out);
    http_resp_free(&r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode failed: %s", esp_err_to_name(err));
        heap_caps_free(buf);
        return false;
    }
    *pixels = buf;
    *w = out.width;
    *h = out.height;
    return true;
}

static void loader_task(void *arg)
{
    char url[URL_MAX];
    for (;;) {
        bool have = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_npending > 0) {
            kp_strlcpy(url, s_pending[0], URL_MAX);
            memmove(&s_pending[0], &s_pending[1], (size_t)(s_npending - 1) * URL_MAX);
            s_npending--;
            have = find(url) == NULL;
        }
        xSemaphoreGive(s_mutex);
        if (!have) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        bool ok = decode(url, &pixels, &w, &h);

        // Make room. Dropping LVGL's cached reference needs the LVGL lock.
        board_lock(-1);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        entry_t *e = take_slot();
        if (!e) {
            xSemaphoreGive(s_mutex);
            board_unlock();
            if (pixels) heap_caps_free(pixels);
            ESP_LOGW(TAG, "cache full of images in use");
            continue;
        }
        if (e->used && e->pixels) {
            lv_image_cache_drop(&e->dsc);
            heap_caps_free(e->pixels);
        }
        memset(e, 0, sizeof(*e));
        e->used = true;
        kp_strlcpy(e->url, url, URL_MAX);
        e->last_used = ++s_clock;
        if (ok) {
            e->pixels = pixels;
            e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            e->dsc.header.w = w;
            e->dsc.header.h = h;
            e->dsc.header.stride = w * 2;
            e->dsc.data_size = (uint32_t)w * h * 2;
            e->dsc.data = pixels;
        } else {
            e->failed = true;
            e->failed_at = xTaskGetTickCount();
        }
        xSemaphoreGive(s_mutex);
        board_unlock();
    }
}

void images_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_cache = heap_caps_calloc(KP_IMG_CACHE, sizeof(entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreatePinnedToCore(loader_task, "images", 12288, NULL, 3, &s_task, 0);
}
