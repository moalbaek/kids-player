// Hardware abstraction for the two supported boards.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

typedef enum {
    BTN_NONE = 0,
    BTN_PLAY_PAUSE,
    BTN_VOL_UP,
    BTN_VOL_DOWN,
    BTN_PARENT,     // long press: open the grown-up menu
    BTN_POWER_OFF,  // long press on power key (strip, on battery)
} board_button_t;

// Start display, touch, power management and buttons. Returns the LVGL display.
lv_display_t *board_init(void);

// LVGL is not thread safe: wrap every lv_* call from other tasks in board_lock()/board_unlock().
bool board_lock(int timeout_ms);  // -1 = wait forever
void board_unlock(void);

void board_set_brightness(int percent);  // 0 = screen off

// Poll for a button event (call every ~20 ms from one task).
board_button_t board_poll_button(void);

// Battery level 0-100, or -1 if no battery/unknown. *charging is set when known.
int board_battery(bool *charging);

void board_power_off(void);

// Apply orientation settings (strip: flip 180 degrees).
void board_apply_rotation(void);
