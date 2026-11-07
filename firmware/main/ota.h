#pragma once
#include <stdbool.h>

typedef enum { OTA_NONE, OTA_UP_TO_DATE, OTA_FAILED } ota_result_t;

// Check the GitHub Pages site for a newer firmware. If found, shows "Updating", installs it and restarts
// (does not return). Returns OTA_UP_TO_DATE / OTA_FAILED otherwise.
ota_result_t ota_check_and_update(void);

// Confirm the running firmware works (cancels automatic rollback).
void ota_mark_valid(void);
