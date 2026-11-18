// Converts gamepad state (HID / Xbox) into game buttons. Shared by BLE and USB.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "game.h"
#include "hid_gamepad_parse.h"

// Abstract Xbox-layout buttons
enum {
    XB_A = 1 << 0, XB_B = 1 << 1, XB_X = 1 << 2, XB_Y = 1 << 3,
    XB_LB = 1 << 4, XB_RB = 1 << 5, XB_START = 1 << 6, XB_BACK = 1 << 7,
    XB_UP = 1 << 8, XB_DOWN = 1 << 9, XB_LEFT = 1 << 10, XB_RIGHT = 1 << 11,
};

typedef struct {
    float lx, ly, rx, ry;   // -1..1, y down
    float lt, rt;           // 0..1
    uint16_t btn;           // XB_*
} xbox_state_t;

void pad_from_xbox(const xbox_state_t *s, game_input_t *o);
// Generic HID (DInput / BLE). right_ok = the right stick has been at center at least once.
void pad_from_hid(const hid_pad_state_t *s, bool right_ok, game_input_t *o);
