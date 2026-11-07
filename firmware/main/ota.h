#pragma once
#include <stdbool.h>

typedef enum { OTA_UP_TO_DATE, OTA_REJECTED, OTA_FAILED } ota_result_t;

// Check the GitHub Pages site for a newer firmware. If found, shows "Updating", installs it and restarts
// (does not return). Skips a version the bootloader has rejected.
ota_result_t ota_check_and_update(void);

// Confirm the running firmware works (cancels automatic rollback).
void ota_mark_valid(void);
