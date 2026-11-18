#pragma once
#include <stdbool.h>
#include "game.h"

void usb_gamepad_start(void);
bool usb_gamepad_read(game_input_t *out);
bool usb_gamepad_connected(void);
const char *usb_gamepad_name(void);
