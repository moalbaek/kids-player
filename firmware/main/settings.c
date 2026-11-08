#include "settings.h"
#include "config.h"
#include "kp_util.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "settings";
kp_settings_t g_set;
static SemaphoreHandle_t s_mutex;

void settings_lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void settings_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *def)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) kp_strlcpy(out, def, len);
}

static int get_int(nvs_handle_t h, const char *key, int def)
{
    int32_t v;
    return nvs_get_i32(h, key, &v) == ESP_OK ? (int)v : def;
}

void settings_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    memset(&g_set, 0, sizeof(g_set));
    nvs_handle_t h;
    if (nvs_open("kp", NVS_READONLY, &h) != ESP_OK) {
        kp_strlcpy(g_set.prefix, KP_DEFAULT_PREFIX, sizeof(g_set.prefix));
        g_set.max_vol = KP_DEFAULT_MAX_VOL;
        g_set.full_library = KP_DEFAULT_FULL_LIBRARY;
        return;
    }
    get_str(h, "ssid", g_set.wifi_ssid, sizeof(g_set.wifi_ssid), "");
    get_str(h, "pass", g_set.wifi_pass, sizeof(g_set.wifi_pass), "");
    get_str(h, "rtok", g_set.refresh_token, sizeof(g_set.refresh_token), "");
    get_str(h, "spk_uid", g_set.speaker_uid, sizeof(g_set.speaker_uid), "");
    get_str(h, "spk_name", g_set.speaker_name, sizeof(g_set.speaker_name), "");
    get_str(h, "spk_ip", g_set.speaker_ip, sizeof(g_set.speaker_ip), "");
    get_str(h, "prefix", g_set.prefix, sizeof(g_set.prefix), KP_DEFAULT_PREFIX);
    g_set.service_type = get_int(h, "svc", 0);
    g_set.max_vol = get_int(h, "maxvol", KP_DEFAULT_MAX_VOL);
    g_set.flip = get_int(h, "flip", 0);
    g_set.swap_buttons = get_int(h, "swapbtn", 0);
    g_set.full_library = get_int(h, "full", KP_DEFAULT_FULL_LIBRARY);
    get_str(h, "ap_pass", g_set.ap_pass, sizeof(g_set.ap_pass), "");
    get_str(h, "ota_ver", g_set.ota_retry_ver, sizeof(g_set.ota_retry_ver), "");
    g_set.ota_retries = get_int(h, "ota_tries", 0);
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: ssid='%s' logged_in=%d room='%s' prefix='%s'", g_set.wifi_ssid,
             g_set.refresh_token[0] != 0, g_set.speaker_name, g_set.prefix);
}

void settings_save(void)
{
    settings_lock();
    nvs_handle_t h;
    if (nvs_open("kp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", g_set.wifi_ssid);
        nvs_set_str(h, "pass", g_set.wifi_pass);
        nvs_set_str(h, "rtok", g_set.refresh_token);
        nvs_set_str(h, "spk_uid", g_set.speaker_uid);
        nvs_set_str(h, "spk_name", g_set.speaker_name);
        nvs_set_str(h, "spk_ip", g_set.speaker_ip);
        nvs_set_str(h, "prefix", g_set.prefix);
        nvs_set_i32(h, "svc", g_set.service_type);
        nvs_set_i32(h, "maxvol", g_set.max_vol);
        nvs_set_i32(h, "flip", g_set.flip);
        nvs_set_i32(h, "swapbtn", g_set.swap_buttons);
        nvs_set_i32(h, "full", g_set.full_library);
        nvs_set_str(h, "ap_pass", g_set.ap_pass);
        nvs_set_str(h, "ota_ver", g_set.ota_retry_ver);
        nvs_set_i32(h, "ota_tries", g_set.ota_retries);
        esp_err_t err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
        nvs_close(h);
    }
    settings_unlock();
}
