#include "ota.h"
#include "config.h"
#include "ui.h"
#include "kp_util.h"
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

ota_result_t ota_check_and_update(void)
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
    if (!latest[0] || kp_version_cmp(latest, mine) <= 0) {
        ESP_LOGI(TAG, "up to date (%s, latest %s)", mine, latest[0] ? latest : "?");
        return OTA_UP_TO_DATE;
    }
    ESP_LOGI(TAG, "updating %s -> %s", mine, latest);
    char msg[64];
    snprintf(msg, sizeof(msg), "Version %s. This takes about a minute.", latest);
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
    return OTA_FAILED;
}
