// Host map test: world checks + overview map + game frames at given points.
// gcc -O2 -DGAME_C='"../main/game.c"' -o map_test map_test.c -lm && ./map_test out_dir
#include GAME_C
#include <time.h>

static uint16_t frame[SW * SH];

static void rgb_of(uint16_t v, uint8_t *o)
{
    v = (uint16_t)((v >> 8) | (v << 8));
    o[0] = (uint8_t)((v >> 11) << 3); o[1] = (uint8_t)(((v >> 5) & 63) << 2); o[2] = (uint8_t)((v & 31) << 3);
}
static void save_frame(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < SW * SH; i++) { uint8_t c[3]; rgb_of(frame[i], c); fwrite(c, 1, 3, f); }
    fclose(f);
}
static uint16_t ground_px(int wx, int wy)
{
    wx &= WMASK; wy &= WMASK;
    int tx = wx >> 4, ty = wy >> 4, lx = wx & 15, ly = wy & 15;
    uint16_t v = map_v[MI(tx, ty)];
    return pal[MV_PAL(v)][tex_ptr[MV_TEX(v)][ly * 16 + lx]];
}
static void save_world(const char *path, int step)
{
    int n = WORLD / step;
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", n, n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                uint8_t c[3]; rgb_of(ground_px(x * step + sx * step / 2 + step / 4, y * step + sy * step / 2 + step / 4), c);
                r += c[0]; g += c[1]; b += c[2];
            }
            uint8_t o[3] = { r / 4, g / 4, b / 4 };
            fwrite(o, 1, 3, f);
        }
    fclose(f);
}
static void shot(const char *dir, const char *name, float cx, float cy)
{
    camX = cx; camY = cy;
    game_begin_frame();
    for (int y = 0; y < SH; y += 30) game_render_chunk(frame + y * SW, y, 30);
    char p[300]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    save_frame(p);
}
static double bench(float cx, float cy)
{
    struct timespec a, b;
    camX = cx; camY = cy;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int k = 0; k < 20; k++) { game_begin_frame(); for (int y = 0; y < SH; y += 30) game_render_chunk(frame + y * SW, y, 30); }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 20;
}

static size_t f_rd(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    game_save_t sv = { 120, 0, 0, 0, 0 };
    FILE *wf = argc > 2 ? fopen(argv[2], "rb") : NULL;      // WORLD.BIN (otherwise generated)
    if (wf) game_set_world_source(f_rd, wf);
    game_init(777, &sv);
    if (wf) fclose(wf);
    printf("world %s, textures: %d (limit %d), lots %d, trees %d, lamps %d\n", wf ? "loaded" : "generated", ntex, MAX_TEX, g_nlots, ntrees, nlamps);
    // ---- 1. AI roads: connectivity and dead ends ----
    int N = MAP * MAP;
    uint8_t *seen = calloc(N, 1);
    int *q = malloc(N * sizeof(int));
    int comps = 0, roads = 0, biggest = 0;
    for (int i = 0; i < N; i++) if (lanes[i]) roads++;
    for (int s = 0; s < N; s++) {
        if (!lanes[s] || seen[s]) continue;
        comps++;
        int head = 0, tail = 0; q[tail++] = s; seen[s] = 1; int cnt = 0;
        while (head < tail) {
            int i = q[head++]; cnt++;
            int x = i & (MAP - 1), y = i >> MSH;
            int nb[4] = { MI(x + 1, y), MI(x - 1, y), MI(x, y + 1), MI(x, y - 1) };
            for (int k = 0; k < 4; k++) if (lanes[nb[k]] && !seen[nb[k]]) { seen[nb[k]] = 1; q[tail++] = nb[k]; }
        }
        if (cnt > biggest) biggest = cnt;
        if (cnt < 2000) printf("  small road component %d: %d tiles at %d,%d\n", comps, cnt, s & (MAP - 1), s >> MSH);
    }
    int dead = 0;
    for (int y = 0; y < MAP; y++) for (int x = 0; x < MAP; x++) {
        int f = lanes[MI(x, y)];
        if (!f || (f & (f - 1))) continue;          // intersection or no road
        int dx = f == LF_E ? 1 : f == LF_W ? -1 : 0, dy = f == LF_S ? 1 : f == LF_N ? -1 : 0;
        if (!lanes[MI(x + dx, y + dy)]) { dead++; if (dead <= 8) printf("  dead end at tile %d,%d lane %d\n", x, y, f); }
    }
    printf("roads: %d lane tiles, %d components (largest %d), dead-end lane tiles: %d\n", roads, comps, biggest, dead);
    // ---- 2. fixed objects not in water/buildings ----
    int bad_obj = 0;
    #define CHK(name, px, py) do { int k_ = tile_kind(((int)(px)) >> 4, ((int)(py)) >> 4); bool b_ = (k_ == T_WATER || k_ == T_BUILDING); bad_obj += b_; printf("  %-16s tile %4d,%4d kind %d %s\n", name, ((int)(px)) >> 4, ((int)(py)) >> 4, k_, b_ ? "<-- BAD" : "ok"); } while (0)
    for (int k = 0; k < NPOI; k++) CHK(POI_NAME[k], pois[k].dx, pois[k].dy);
    for (int k = 0; k < NPHONE; k++) CHK("phone", phone_x[k], phone_y[k]);
    CHK("heli pad", pad_x, pad_y);
    CHK("spray", spray_x, spray_y);
    for (int k = 0; k < NSTASH; k++) CHK(STASH_NAME[k], stash_x[k], stash_y[k]);
    CHK("player start", plx, ply);
    for (int i = 0; i < NPICK; i++) CHK("pickup", pick[i].x, pick[i].y);
    for (int g = 1; g <= 2; g++) { float x, y; ghetto_center(g, &x, &y); CHK(g == 1 ? "gang1 home" : "gang2 home", x, y); printf("    gang zone there: %d\n", gang_zone(((int)x) >> 4, ((int)y) >> 4)); }
    printf("fixed objects in water/buildings: %d\n", bad_obj);
    {   // boats must float only on water
        init_boats();
        int bad = 0;
        for (int b = 0; b < NBOATS; b++)
            for (float t = boats[b].a; t <= boats[b].b; t += 4) {
                float x = boats[b].horiz ? t : boats[b].x, y = boats[b].horiz ? boats[b].y : t;
                int k = tile_kind((((int)x) & WMASK) >> 4, (((int)y) & WMASK) >> 4);
                if (k != T_WATER) { bad++; if (bad < 6) printf("  boat %d on kind %d at tile %d,%d\n", b, k, ((int)x) >> 4, ((int)y) >> 4); }
            }
        printf("boat path px not on water: %d\n", bad);
    }
    char p[300];
    camX = plx; camY = ply; game_begin_frame();
    snprintf(p, sizeof p, "%s/world.ppm", out); save_world(p, 16);
    // ---- 3. frames ----
    gstate = G_PLAY; cinema = true;
    day_t = DAY_LEN * 0.40f; tint_timer = 0; game_update(&(game_input_t){0}, 0.01f);
    static const struct { const char *n; float x, y; } SHOTS[] = {
        { "a_downtown", 236, 470 }, { "a_plaza", 256, 508 }, { "a_park", 284, 420 }, { "a_ghetto", 262, 612 },
        { "a_industry", 350, 622 }, { "a_docks", 180, 632 }, { "a_rich_coast", 62, 452 }, { "a_hills", 150, 372 },
        { "a_panels", 340, 400 }, { "a_river", 216, 520 }, { "b_center", 800, 565 }, { "b_ghetto", 930, 610 },
        { "b_industry", 800, 705 }, { "b_port", 790, 790 }, { "b_piers", 744, 830 }, { "b_resid", 870, 520 },
        { "air_terminal", 730, 268 }, { "air_runway", 592, 168 }, { "air_hangars", 846, 242 }, { "air_apron", 704, 244 },
        { "hw_gas", 506, 490 }, { "hw_motel", 490, 506 }, { "fields", 420, 420 }, { "forest_edge", 160, 300 },
        { "lake_north", 160, 238 }, { "lake_south", 430, 800 }, { "village_nw", 100, 150 }, { "village_s", 380, 905 },
        { "farm", 300, 166 }, { "river_bend", 222, 820 }, { "coast_west", 40, 760 }, { "lighthouse", 1005, 66 },
        { "bay_island", 805, 925 }, { "nw_strip", 80, 340 }, { "hw_cross_air", 500, 292 }, { "country_road", 330, 760 },
    };
    for (unsigned i = 0; i < sizeof(SHOTS) / sizeof(SHOTS[0]); i++) shot(out, SHOTS[i].n, SHOTS[i].x * 16 + 8, SHOTS[i].y * 16 + 8);
    cinema = false; gstate = G_PAUSE;
    shot(out, "pause_map", plx, ply);
    gstate = G_PLAY; cinema = true;
    day_t = DAY_LEN * 0.90f; tint_timer = 0; game_update(&(game_input_t){0}, 0.01f);
    shot(out, "night_a_downtown", 236 * 16, 470 * 16);
    shot(out, "night_b_center", 800 * 16, 565 * 16);
    shot(out, "night_airport", 730 * 16, 262 * 16);
    shot(out, "night_lighthouse", 1003 * 16, 66 * 16);
    day_t = DAY_LEN * 0.40f; tint_timer = 0; game_update(&(game_input_t){0}, 0.01f);
    printf("render ms: a_downtown %.3f, b_center %.3f, forest %.3f, coast %.3f, port %.3f\n",
           bench(236 * 16, 470 * 16), bench(800 * 16, 565 * 16), bench(160 * 16, 300 * 16), bench(40 * 16, 760 * 16), bench(790 * 16, 790 * 16));
    // shore masks: how many are built per frame when moving along the shore (worst case)
    {
        int mx = 0, sum = 0, n = 0;
        for (float y = 600 * 16; y < 900 * 16; y += 6) { camX = 40 * 16; camY = y; game_begin_frame(); if (shore_gen_frame > mx) mx = shore_gen_frame; sum += shore_gen_frame; n++; }
        printf("shore masks built per frame along west coast at car speed: avg %.2f, max %d\n", (float)sum / n, mx);
    }
    return 0;
}
