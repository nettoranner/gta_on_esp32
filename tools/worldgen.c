// 1024x1024 world generator -> WORLD.BIN (goes in the SD card root and next to gta_pc.exe).
// Build and run (Linux/macOS/WSL, from the project folder):
//   gcc -O2 -o worldgen tools/worldgen.c -lm && ./worldgen sd_card/WORLD.BIN
#include "../main/game.c"

static size_t f_wr(void *ctx, void *buf, size_t n) { return fwrite(buf, 1, n, (FILE *)ctx); }
static size_t f_rd(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "WORLD.BIN";
    game_save_t sv = { 0 };
    game_init(1, &sv);
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    bool ok = game_world_save(f_wr, f);
    long size = ftell(f);
    fclose(f);
    if (!ok) { fprintf(stderr, "write failed\n"); return 1; }
    printf("saved %s: %ld bytes\n", out, size);
    // check: load it back and compare the map
    uint16_t *mv = malloc((size_t)MAP * MAP * 2);
    uint8_t *ln = malloc((size_t)MAP * MAP);
    memcpy(mv, map_v, (size_t)MAP * MAP * 2);
    memcpy(ln, lanes, (size_t)MAP * MAP);
    int nl = g_nlots, nt = ntrees;
    f = fopen(out, "rb");
    game_set_world_source(f_rd, f);
    game_init(2, &sv);
    fclose(f);
    if (!game_world_loaded() || memcmp(mv, map_v, (size_t)MAP * MAP * 2) || memcmp(ln, lanes, (size_t)MAP * MAP) || nl != g_nlots || nt != ntrees) {
        fprintf(stderr, "verify FAILED\n");
        return 1;
    }
    printf("verify OK\n");
    return 0;
}
