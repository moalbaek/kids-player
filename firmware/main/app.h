// Commands from the UI, buttons and web pages to the app task.
#pragma once

typedef enum {
    CMD_NONE = 0,
    CMD_OPEN_HOME,      // a = index of the card on the home screen
    CMD_OPEN_RESULT,    // a = index of the search result
    CMD_PLAY,           // a = list token, b = song index
    CMD_SEARCH,         // text set by app_post_search()
    CMD_TOGGLE,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
    CMD_BACK,           // one screen back
    CMD_SELECT_ROOM,    // a = room index from the last discovery
    CMD_PICK_ROOM,      // show the room picker
    CMD_PARENT,         // grown-up menu
    CMD_CLOSE_PARENT,
    CMD_LOGIN_DONE,     // Spotify login finished (from the web server)
    CMD_SETTINGS_CHANGED,
    CMD_REFRESH,
    CMD_OTA_CHECK,
    CMD_POWER_OFF,
    CMD_RESTART,
} app_cmd_type_t;

void app_post(app_cmd_type_t type, int a, int b);

// Start a search for text (called from the keyboard).
void app_post_search(const char *text);
