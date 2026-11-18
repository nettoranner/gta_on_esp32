#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "game.h"

// Starts the BLE HID host: background scan, connect to the first gamepad found
// and automatic reconnect.
bool bt_gamepad_start(void);      // false = out of memory / failed to start (the game runs without BT)
// Current gamepad state in game format. false = no gamepad connected.
bool bt_gamepad_read(game_input_t *out);
bool bt_gamepad_connected(void);
const char *bt_gamepad_name(void);
