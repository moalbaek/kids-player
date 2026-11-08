#pragma once
#include <stdbool.h>

typedef enum {
    OTA_UP_TO_DATE,
    OTA_REJECTED,        // the newest version was rolled back too often; nothing on screen changed
    OTA_FAILED,          // could not check; nothing on screen changed
    OTA_INSTALL_FAILED,  // "Updating" was shown but the install failed: the caller redraws its screen
} ota_result_t;

// Check the GitHub Pages site for a newer firmware. If found, shows "Updating", installs it and restarts
// (does not return). A version the bootloader rolled back is retried KP_OTA_RETRIES times, or always if
// manual (the grown-up asked for it).
ota_result_t ota_check_and_update(bool manual);

// Confirm the running firmware works (cancels automatic rollback).
void ota_mark_valid(void);
