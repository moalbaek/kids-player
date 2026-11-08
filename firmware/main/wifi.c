#include "wifi.h"
#include "kp_util.h"
#include "settings.h"
#include "dns_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "wifi";
static EventGroupHandle_t s_ev;
#define BIT_CONNECTED BIT0
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16];
static bool s_sta_configured, s_ap_on;
static dns_server_handle_t s_dns;
static esp_timer_handle_t s_retry_timer;
static int s_retries;

static void retry_cb(void *arg)
{
    if (s_sta_configured) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_configured) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        s_ip[0] = 0;
        if (s_sta_configured) {
            // Back off: 1 s, 2 s, ... up to 30 s between attempts.
            int delay_s = s_retries < 30 ? s_retries + 1 : 30;
            s_retries++;
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        ESP_LOGI(TAG, "connected, IP %s", s_ip);
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

void wifi_init(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "kids-player");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_timer_create_args_t ta = {.callback = retry_cb, .name = "wifi_retry"};
    esp_timer_create(&ta, &s_retry_timer);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

bool wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t wc = {0};
    kp_strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    kp_strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_sta_configured = true;
    s_retries = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

bool wifi_is_connected(void) { return (xEventGroupGetBits(s_ev) & BIT_CONNECTED) != 0; }

void wifi_ip(char *out, int len) { kp_strlcpy(out, s_ip, (size_t)len); }

bool wifi_ap_running(void) { return s_ap_on; }

void wifi_start_ap(char *ssid_out, int ssid_len, char *pass_out, int pass_len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid_out, ssid_len, "KidsPlayer-%02X%02X", mac[4], mac[5]);
    // Random rather than derived from the MAC (which the AP broadcasts), and created once so a phone that
    // saved the setup network can join it again next time.
    settings_lock();
    bool created = !g_set.ap_pass[0];
    if (created) {
        uint32_t random[2];
        esp_fill_random(random, sizeof(random));
        snprintf(g_set.ap_pass, sizeof(g_set.ap_pass), "%08" PRIx32 "%08" PRIx32, random[0], random[1]);
    }
    kp_strlcpy(pass_out, g_set.ap_pass, pass_len);
    settings_unlock();
    if (created) settings_save();
    if (s_ap_on) return;
    wifi_config_t ac = {0};
    kp_strlcpy((char *)ac.ap.ssid, ssid_out, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = (uint8_t)strlen(ssid_out);
    kp_strlcpy((char *)ac.ap.password, pass_out, sizeof(ac.ap.password));
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = 2;
    ac.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dc);
    s_ap_on = true;
    ESP_LOGI(TAG, "setup network %s started", ssid_out);
}

void wifi_stop_ap(void)
{
    if (!s_ap_on) return;
    if (s_dns) stop_dns_server(s_dns);
    s_dns = NULL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
}

int wifi_scan(char ssids[][33], int max)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&n, recs);  // sorted by RSSI
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *s = (const char *)recs[i].ssid;
        if (!s[0]) continue;
        bool dup = false;
        for (int j = 0; j < count; j++) if (strcmp(ssids[j], s) == 0) dup = true;
        if (!dup) kp_strlcpy(ssids[count++], s, 33);
    }
    free(recs);
    return count;
}
