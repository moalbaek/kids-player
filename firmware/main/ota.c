#include "ota.h"
#include "config.h"
#include "ui.h"
#include "kp_util.h"
#include "settings.h"
#include "http_util.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ota";

void ota_mark_valid(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

ota_result_t ota_check_and_update(bool manual)
{
    http_resp_t r;
    if (http_request("GET", KP_OTA_BASE "version.json", NULL, 0, NULL, 0, 4096, 10000, &r) != ESP_OK ||
        r.status != 200) {
        http_resp_free(&r);
        return OTA_FAILED;
    }
    char latest[32] = "";
    cJSON *root = cJSON_Parse(r.data);
    const cJSON *v = root ? cJSON_GetObjectItemCaseSensitive(root, "version") : NULL;
    if (cJSON_IsString(v)) kp_strlcpy(latest, v->valuestring, sizeof(latest));
    cJSON_Delete(root);
    http_resp_free(&r);
    const char *mine = esp_app_get_description()->version;
    if (!latest[0]) return OTA_FAILED;
    if (kp_version_cmp(latest, mine) <= 0) {
        ESP_LOGI(TAG, "up to date (%s, latest %s)", mine, latest);
        return OTA_UP_TO_DATE;
    }
    // A rollback also happens when a good version was switched off within 30 s, so give it a few more tries.
    const esp_partition_t *rejected = esp_ota_get_last_invalid_partition();
    esp_app_desc_t rejected_desc;
    if (!manual && rejected && esp_ota_get_partition_description(rejected, &rejected_desc) == ESP_OK &&
        !strcmp(latest, rejected_desc.version)) {
        settings_lock();
        if (strcmp(g_set.ota_retry_ver, latest) != 0) {
            kp_strlcpy(g_set.ota_retry_ver, latest, sizeof(g_set.ota_retry_ver));
            g_set.ota_retries = 0;
        }
        bool give_up = g_set.ota_retries >= KP_OTA_RETRIES;
        if (!give_up) g_set.ota_retries++;
        int tries = g_set.ota_retries;
        settings_unlock();
        if (give_up) {
            ESP_LOGW(TAG, "skipping version %s: rolled back after %d retries", latest, tries);
            return OTA_REJECTED;
        }
        settings_save();
        ESP_LOGW(TAG, "version %s was rolled back; retry %d of %d", latest, tries, KP_OTA_RETRIES);
    }
    ESP_LOGI(TAG, "updating %s -> %s", mine, latest);
    char msg[64];
    snprintf(msg, sizeof(msg), "Version %s. This takes about a minute.", latest);
    // The download blocks the app task, so say so instead of leaving a screen that ignores taps.
    ui_show_message(NULL, "Updating…", msg, true);
    esp_http_client_config_t http = {
        .url = KP_OTA_BASE "kids-player.bin",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_err_t err = esp_https_ota(&cfg);
    if (err == ESP_OK) {
        ui_show_message(LV_SYMBOL_OK, "Updated", "Restarting…", false);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    return OTA_INSTALL_FAILED;
}
