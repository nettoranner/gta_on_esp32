#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "game.h"

bool storage_init(void);
bool storage_ok(void);
bool storage_load(game_save_t *s);
void storage_save(const game_save_t *s);

bool storage_shot_begin(int w, int h, char *name_out, int name_len);
void storage_shot_rows(const uint16_t *px, int count);
void storage_shot_end(void);
// GTA.PAK from SD into PSRAM (NULL = no file / PSRAM)
const uint8_t *storage_load_pak(size_t *len);
// WORLD.BIN (1024x1024 world) from SD: open / read in blocks / close
void *storage_world_open(void);
size_t storage_world_read(void *ctx, void *buf, size_t n);
void storage_world_close(void *ctx);
