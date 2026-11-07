// Waveshare ESP32-S3-Touch-AMOLED-2.16 ("cube"): 480x480 AMOLED (CO5300), CST9220 touch, AXP2101 PMU.
// Buttons: KEY3 = GPIO18, BOOT = GPIO0, PWR = AXP2101 power key (read via its IRQ registers).
#include "sdkconfig.h"
#if CONFIG_KP_BOARD_CUBE
#include "board.h"
#include "settings.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cube";
static i2c_master_dev_handle_t s_pmu;

#define PMU_ADDR 0x34
#define GPIO_KEY3 GPIO_NUM_18
#define GPIO_BOOT GPIO_NUM_0

static esp_err_t pmu_read(uint8_t reg, uint8_t *val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, 100);
}

static esp_err_t pmu_write(uint8_t reg, uint8_t val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_pmu, b, 2, 100);
}

static void pmu_set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t v;
    if (pmu_read(reg, &v) == ESP_OK) pmu_write(reg, v | bits);
}

static void pmu_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMU_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_pmu) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not available");
        s_pmu = NULL;
        return;
    }
    uint8_t id = 0;
    pmu_read(0x03, &id);
    ESP_LOGI(TAG, "AXP2101 chip id 0x%02x", id);
    pmu_set_bits(0x68, 0x01);  // battery detection on
    pmu_set_bits(0x30, 0x01);  // battery voltage ADC on
    pmu_set_bits(0x18, 0x08);  // fuel gauge on
    pmu_write(0x48, 0xFF);     // clear pending IRQs
    pmu_write(0x49, 0xFF);
    pmu_write(0x4A, 0xFF);
    pmu_set_bits(0x41, 0x0C);  // power key short (bit3) and long (bit2) press IRQs
}

lv_display_t *board_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_KEY3) | (1ULL << GPIO_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    bsp_i2c_init();
    pmu_init();
    lv_display_t *disp = bsp_display_start();
    if (disp) bsp_display_brightness_set(100);
    return disp;
}

// The adapter's lock is recursive. Call it directly: bsp_display_lock(0) does not wait (0 = try once in
// esp_lvgl_adapter) and its bool result is ESP_OK cast to false.
bool board_lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms < 0 ? -1 : timeout_ms) == ESP_OK; }
void board_unlock(void) { esp_lv_adapter_unlock(); }

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (board_lock(500)) {
        bsp_display_brightness_set(percent);
        board_unlock();
    }
}

void board_apply_rotation(void) {}

typedef struct {
    bool down;
    int64_t since;
    bool long_sent;
} key_t_;

static key_t_ s_key3, s_boot;
static int s_poll;

// Returns 1 on release after a short press, 2 once when held for long_ms.
static int key_update(key_t_ *k, bool pressed, int long_ms)
{
    int64_t now = esp_timer_get_time();
    if (pressed && !k->down) {
        k->down = true;
        k->since = now;
        k->long_sent = false;
    } else if (pressed && k->down && !k->long_sent && long_ms > 0 && now - k->since > (int64_t)long_ms * 1000) {
        k->long_sent = true;
        return 2;
    } else if (!pressed && k->down) {
        k->down = false;
        if (!k->long_sent && now - k->since > 30000) return 1;  // >30 ms = debounced click
    }
    return 0;
}

board_button_t board_poll_button(void)
{
    bool swap = g_set.swap_buttons != 0;
    int k3 = key_update(&s_key3, gpio_get_level(GPIO_KEY3) == 0, 0);
    if (k3 == 1) return swap ? BTN_VOL_DOWN : BTN_VOL_UP;
    int kb = key_update(&s_boot, gpio_get_level(GPIO_BOOT) == 0, 3000);
    if (kb == 2) return BTN_PARENT;
    if (kb == 1) return swap ? BTN_VOL_UP : BTN_VOL_DOWN;
    if (++s_poll >= 5) {  // PMU every ~100 ms
        s_poll = 0;
        uint8_t st = 0;
        if (pmu_read(0x49, &st) == ESP_OK && (st & 0x0C)) {
            pmu_write(0x49, st & 0x0C);
            if (st & 0x08) return BTN_PLAY_PAUSE;  // short press
        }
    }
    return BTN_NONE;
}

int board_battery(bool *charging)
{
    uint8_t s0 = 0, s1 = 0, pct = 0;
    if (pmu_read(0x00, &s0) != ESP_OK || !(s0 & 0x08)) return -1;  // no battery
    if (charging && pmu_read(0x01, &s1) == ESP_OK) *charging = ((s1 >> 5) & 0x03) == 0x01;
    if (pmu_read(0xA4, &pct) != ESP_OK || pct > 100) return -1;
    return pct;
}

void board_power_off(void)
{
    pmu_set_bits(0x10, 0x01);  // AXP2101: power off
}
#endif
