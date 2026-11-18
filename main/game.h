// Game core: logic and rendering with no hardware dependencies.
// The same code builds for ESP32-S3 and for PC (host/sim.c) for debugging.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SW 320
#define SH 240

// Buttons (bit mask of game_input_t.held)
enum {
    BTN_FIRE        = 1 << 0,   // shoot / punch
    BTN_ENTER       = 1 << 1,   // enter / exit car
    BTN_WEAPON      = 1 << 2,   // next weapon
    BTN_RUN         = 1 << 3,   // run (on foot) / handbrake (in car)
    BTN_START       = 1 << 4,   // pause / start
    BTN_SHOT        = 1 << 5,   // screenshot to SD
    BTN_WEAPON_PREV = 1 << 6,   // previous weapon
};

typedef struct {
    float mx, my;       // movement, -1..1 (y down)
    float ax, ay;       // right stick (aim), -1..1
    bool  aim;          // right stick deflected
    uint16_t held;      // pressed buttons
} game_input_t;

typedef struct {
    int32_t money;
    int32_t kills;
    int32_t deliveries;
    int32_t best_wanted;
    int32_t flags;          // bit0 = VORTEX GT won at the casino
} game_save_t;

void game_init(uint32_t seed, const game_save_t *save);
void game_update(const game_input_t *in, float dt);
// Render the screen strip [y0, y0+lines) into buf (RGB565, bytes swapped for SPI)
void game_render_chunk(uint16_t *buf, int y0, int lines);
// Frame setup (camera etc.) - call once before a series of render_chunk
void game_begin_frame(void);

void game_set_status(bool pad_connected, bool sd_ok, const char *pad_name);
void game_get_save(game_save_t *s);
bool game_take_save_request(void);        // true if progress changed and is worth saving
bool game_take_screenshot_request(void);  // true if the player pressed "screenshot"
void game_message(const char *text, float seconds);
// 1 = +3 hours of game time, 2 = rain on/off, 3 = cinema mode, 4 = replay (for video recording)
void game_debug(int what);
// Vibration strength 0..255 (vibration motor / gamepad rumble), decays on its own
uint8_t game_rumble(void);
// GTA.PAK graphics pack (data must live for the whole game). false = format not accepted.
#include <stddef.h>
bool game_set_pak(const uint8_t *data, size_t len);

// 1024x1024 world (WORLD.BIN). Generated on PC (tools/worldgen), ESP32 only loads it.
// rd/wr = read/write a block, return the number of bytes.
typedef size_t (*game_io_fn)(void *ctx, void *buf, size_t n);
void game_set_world_source(game_io_fn rd, void *ctx);   // before game_init: load the world instead of generating it
bool game_world_loaded(void);                          // true = world loaded (or generated on PC)
bool game_world_save(game_io_fn wr, void *ctx);        // after game_init: write out the current world
// Loading/diagnostics screen before the game starts: text with lines separated by \n ('#' red, '+' green, '!' yellow, '*' header)
void game_draw_boot(uint16_t *buf, int y0, int lines, const char *text);
