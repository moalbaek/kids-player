// Album cover download + JPEG decode + small LRU cache in PSRAM.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

void images_init(void);

// Call with the LVGL lock held. Returns a decoded image and takes a reference (release it when the
// widget stops showing it), or NULL if not ready yet. *failed is set if the image could not be loaded.
const lv_image_dsc_t *images_acquire(const char *url, bool *failed);
void images_release(const lv_image_dsc_t *dsc);

// Ask the loader to fetch an image (newest requests first). Safe from any task.
void images_request(const char *url);
