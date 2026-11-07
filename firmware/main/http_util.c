#include "http_util.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http";

typedef struct {
    int retry_after;
} hdr_ctx_t;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data && evt->header_key &&
        strcasecmp(evt->header_key, "Retry-After") == 0) {
        ((hdr_ctx_t *)evt->user_data)->retry_after = atoi(evt->header_value);
    }
    return ESP_OK;
}

void http_resp_free(http_resp_t *r)
{
    if (r && r->data) {
        heap_caps_free(r->data);
        r->data = NULL;
    }
}

esp_err_t http_request(const char *method, const char *url, const http_header_t *headers, int nheaders,
                       const char *body, size_t body_len, size_t max_len, int timeout_ms, http_resp_t *out)
{
    memset(out, 0, sizeof(*out));
    out->status = -1;
    hdr_ctx_t ctx = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = on_event,
        .user_data = &ctx,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = false,
        .keep_alive_enable = false,
    };
    if (strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (strcmp(method, "POST") == 0) m = HTTP_METHOD_POST;
    else if (strcmp(method, "PUT") == 0) m = HTTP_METHOD_PUT;
    esp_http_client_set_method(c, m);
    for (int i = 0; i < nheaders; i++) esp_http_client_set_header(c, headers[i].name, headers[i].value);

    esp_err_t err = esp_http_client_open(c, body ? (int)body_len : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: open failed: %s", method, url, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    if (body && body_len) {
        size_t sent = 0;
        while (sent < body_len) {
            int w = esp_http_client_write(c, body + sent, (int)(body_len - sent));
            if (w <= 0) {
                esp_http_client_cleanup(c);
                return ESP_FAIL;
            }
            sent += (size_t)w;
        }
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    out->status = esp_http_client_get_status_code(c);
    out->retry_after = ctx.retry_after;

    size_t cap = (clen > 0 && (size_t)clen < max_len) ? (size_t)clen + 1 : 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(cap);
    if (!buf) {
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    int retries = 0;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= max_len) {  // too big: keep what we have
                ESP_LOGW(TAG, "%s: body truncated at %u bytes", url, (unsigned)len);
                break;
            }
            size_t ncap = cap * 2 > max_len ? max_len : cap * 2;
            char *nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
        if (r == -ESP_ERR_HTTP_EAGAIN && ++retries < 5) continue;
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    out->data = buf;
    out->len = len;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (out->status >= 400) ESP_LOGW(TAG, "%s %s -> %d", method, url, out->status);
    return err;
}
