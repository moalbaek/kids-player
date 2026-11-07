#pragma once
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char *data;    // NUL-terminated body (PSRAM), or NULL
    size_t len;
    int status;    // HTTP status, or -1 when the request failed
    int retry_after;  // seconds from a Retry-After header, 0 if absent
} http_resp_t;

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

// Perform a request and collect the whole body (up to max_len bytes). Works for http and https.
esp_err_t http_request(const char *method, const char *url, const http_header_t *headers, int nheaders,
                       const char *body, size_t body_len, size_t max_len, int timeout_ms, http_resp_t *out);

void http_resp_free(http_resp_t *r);
