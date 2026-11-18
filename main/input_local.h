#pragma once
#include <stdbool.h>
#include "game.h"

// Analog joystick module (KY-023 and similar) + 4 buttons (1x4 membrane or tactile)
void input_local_init(void);
void input_local_read(game_input_t *out);
bool input_local_joy_present(void);
