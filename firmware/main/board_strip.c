// Waveshare ESP32-S3-Touch-LCD-3.49 V2 ("strip"): 172x640 IPS (AXS15231B over QSPI), shown in landscape.
// TCA9554 expander (I2C 47/48): bit1 backlight enable, bit5 LCD reset, bit6 power hold (battery).
// Touch: own I2C bus (SDA 17 / SCL 18), address 0x3B. Buttons: BOOT = GPIO0, PWR = GPIO16.
// Based on Waveshare's ESP-IDF example (10_LVGL_V9_Test); this panel needs full-frame refreshes.
#include "sdkconfig.h"
#if CONFIG_KP_BOARD_STRIP
#include "board.h"
#include "settings.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_io_expander_tca9554.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "strip";

#define PHYS_W 172
#define PHYS_H 640
#define LCD_HOST SPI3_HOST
#define PIN_CS 9
#define PIN_CLK 10
#define PIN_D0 11
#define PIN_D1 12
#define PIN_D2 13
#define PIN_D3 14
#define PIN_BL 42
#define PIN_I2C0_SDA 47
#define PIN_I2C0_SCL 48
#define PIN_TP_SDA 17
#define PIN_TP_SCL 18
#define PIN_BOOT 0
#define PIN_PWR 16
#define EXIO_BL_EN (1ULL << 1)
#define EXIO_LCD_RST (1ULL << 5)
#define EXIO_SYS_EN (1ULL << 6)
#define TOUCH_ADDR 0x3B
#define CHUNK_LINES 64
#define CHUNK_BYTES (PHYS_W * CHUNK_LINES * 2)
#define FRAME_BYTES (PHYS_W * PHYS_H * 2)

static esp_io_expander_handle_t s_exio;
static i2c_master_bus_handle_t s_bus0, s_bus_tp;
static i2c_master_dev_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_sem, s_lvgl_mutex;
static uint16_t *s_dma_buf;
static uint8_t *s_rot_buf;
static lv_display_t *s_disp;
static volatile bool s_first_frame_done;
static int s_brightness = 100;

static void backlight_pwm(int percent)
{
    // The backlight PWM is inverted on this board: duty 0 = full brightness.
    uint32_t duty = 255 - (uint32_t)(percent * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    lv_draw_rgb565_swap(px, lv_area_get_width(area) * lv_area_get_height(area));
    const uint8_t *src = px;
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    if (rot != LV_DISPLAY_ROTATION_0) {
        lv_area_t ra = *area;
        lv_display_rotate_area(disp, &ra);
        int32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
        uint32_t dst_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&ra), LV_COLOR_FORMAT_RGB565);
        lv_draw_rotate(px, s_rot_buf, w, h, src_stride, dst_stride, rot, LV_COLOR_FORMAT_RGB565);
        src = s_rot_buf;
    }
    // Full frame in physical orientation: push it in DMA-sized chunks.
    xSemaphoreGive(s_flush_sem);
    for (int y = 0; y < PHYS_H; y += CHUNK_LINES) {
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        memcpy(s_dma_buf, src + (size_t)y * PHYS_W * 2, CHUNK_BYTES);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, PHYS_W, y + CHUNK_LINES, s_dma_buf);
    }
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    s_first_frame_done = true;
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buf[32] = {0};
    data->state = LV_INDEV_STATE_RELEASED;
    if (i2c_master_transmit_receive(s_touch, cmd, sizeof(cmd), buf, sizeof(buf), 50) != ESP_OK) return;
    if (buf[1] == 0 || buf[1] > 4) return;
    int px = ((buf[2] & 0x0f) << 8) | buf[3];  // along the long side
    int py = ((buf[4] & 0x0f) << 8) | buf[5];  // along the short side
    if (px > PHYS_H - 1) px = PHYS_H - 1;
    if (py > PHYS_W - 1) py = PHYS_W - 1;
    // Physical (portrait) coordinates; LVGL applies the display rotation itself.
    data->point.x = py;
    data->point.y = PHYS_H - 1 - px;
    data->state = LV_INDEV_STATE_PRESSED;
}

static uint32_t tick_get(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lvgl_task(void *arg)
{
    bool lit = false;
    for (;;) {
        uint32_t delay = 10;
        if (xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            delay = lv_timer_handler();
            xSemaphoreGiveRecursive(s_lvgl_mutex);
        }
        if (!lit && s_first_frame_done) {
            lit = true;
            esp_io_expander_set_level(s_exio, EXIO_BL_EN, 1);
            backlight_pwm(s_brightness);
        }
        if (delay < 5) delay = 5;
        if (delay > 50) delay = 50;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

lv_display_t *board_init(void)
{
    // Buttons
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BOOT) | (1ULL << PIN_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    // Backlight PWM off until the first frame is drawn
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                             .timer_num = LEDC_TIMER_3, .freq_hz = 50000, .clk_cfg = LEDC_SLOW_CLK_RC_FAST};
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {.gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                                .timer_sel = LEDC_TIMER_3, .duty = 255, .hpoint = 0};
    ledc_channel_config(&ch);

    // I2C buses
    i2c_master_bus_config_t b0 = {.clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
                                  .scl_io_num = PIN_I2C0_SCL, .sda_io_num = PIN_I2C0_SDA,
                                  .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&b0, &s_bus0));
    i2c_master_bus_config_t b1 = b0;
    b1.i2c_port = I2C_NUM_1;
    b1.scl_io_num = PIN_TP_SCL;
    b1.sda_io_num = PIN_TP_SDA;
    ESP_ERROR_CHECK(i2c_new_master_bus(&b1, &s_bus_tp));
    i2c_device_config_t tdev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDR,
                                .scl_speed_hz = 300000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus_tp, &tdev, &s_touch));

    // IO expander: hold power on, backlight off, release LCD reset
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(s_bus0, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_exio));
    esp_io_expander_set_dir(s_exio, EXIO_SYS_EN | EXIO_BL_EN | EXIO_LCD_RST, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_exio, EXIO_SYS_EN, 1);
    esp_io_expander_set_level(s_exio, EXIO_BL_EN, 0);
    esp_io_expander_set_level(s_exio, EXIO_LCD_RST, 1);

    // QSPI panel
    s_flush_sem = xSemaphoreCreateBinary();
    spi_bus_config_t bus = {.sclk_io_num = PIN_CLK, .data0_io_num = PIN_D0, .data1_io_num = PIN_D1,
                            .data2_io_num = PIN_D2, .data3_io_num = PIN_D3, .max_transfer_sz = CHUNK_BYTES};
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t pio = NULL;
    esp_lcd_panel_io_spi_config_t ioc = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = -1, .spi_mode = 3, .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10, .on_color_trans_done = on_color_done, .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, &pio));
    const axs15231b_lcd_init_cmd_t init_cmds[] = {  // used only during esp_lcd_panel_init below
        {0x11, (uint8_t[]){0x00}, 0, 100},
        {0x29, (uint8_t[]){0x00}, 0, 100},
    };
    axs15231b_vendor_config_t vc = {.init_cmds = init_cmds, .init_cmds_size = 2, .flags.use_qspi_interface = 1};
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                     .bits_per_pixel = 16, .vendor_config = &vc};
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(pio, &pc, &s_panel));
    esp_io_expander_set_level(s_exio, EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_io_expander_set_level(s_exio, EXIO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_io_expander_set_level(s_exio, EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // LVGL
    lv_init();
    lv_tick_set_cb(tick_get);
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    s_disp = lv_display_create(PHYS_W, PHYS_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    void *b1buf = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    void *b2buf = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_rot_buf = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_dma_buf = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1buf && b2buf && s_rot_buf && s_dma_buf);
    lv_display_set_buffers(s_disp, b1buf, b2buf, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_rotation(s_disp, g_set.flip ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_disp);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "display ready");
    return s_disp;
}

bool board_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, t) == pdTRUE;
}

void board_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mutex); }

void board_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (!s_first_frame_done) return;
    esp_io_expander_set_level(s_exio, EXIO_BL_EN, percent > 0);
    backlight_pwm(percent);
}

void board_apply_rotation(void)
{
    if (board_lock(-1)) {
        lv_display_set_rotation(s_disp, g_set.flip ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
        board_unlock();
    }
}

typedef struct {
    bool down;
    int64_t since;
    bool long_sent;
} key_t_;

static key_t_ s_boot, s_pwr;

static int key_update(key_t_ *k, bool pressed, int long_ms)
{
    int64_t now = esp_timer_get_time();
    if (pressed && !k->down) {
        k->down = true;
        k->since = now;
        k->long_sent = false;
    } else if (pressed && k->down && !k->long_sent && now - k->since > (int64_t)long_ms * 1000) {
        k->long_sent = true;
        return 2;
    } else if (!pressed && k->down) {
        k->down = false;
        if (!k->long_sent && now - k->since > 30000) return 1;
    }
    return 0;
}

board_button_t board_poll_button(void)
{
    int b = key_update(&s_boot, gpio_get_level(PIN_BOOT) == 0, 3000);
    if (b == 2) return BTN_PARENT;
    if (b == 1) return BTN_PLAY_PAUSE;
    int p = key_update(&s_pwr, gpio_get_level(PIN_PWR) == 0, 2000);
    if (p == 2) return BTN_POWER_OFF;
    if (p == 1) return BTN_PLAY_PAUSE;
    return BTN_NONE;
}

int board_battery(bool *charging) { return -1; }

void board_power_off(void)
{
    board_set_brightness(0);
    esp_io_expander_set_level(s_exio, EXIO_SYS_EN, 0);  // cuts power when running on the battery
}
#endif
