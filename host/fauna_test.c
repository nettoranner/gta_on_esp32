// Wildlife test: which animals appear in different places of the world + the "geese" scenario (GOOSED)
#include "../main/game.c"
#include <time.h>
static uint16_t frame[SW * SH];
static const char *OUT = ".";
static size_t f_rd(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }
static void shot(const char *name)
{
    game_begin_frame();
    for (int y = 0; y < SH; y += 30) game_render_chunk(frame + y * SW, y, 30);
    char p[300]; snprintf(p, sizeof p, "%s/%s.ppm", OUT, name);
    FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < SW * SH; i++) { uint16_t v = (uint16_t)((frame[i] >> 8) | (frame[i] << 8)); uint8_t c[3] = { (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3 }; fwrite(c, 1, 3, f); }
    fclose(f);
}
static const char *AN_NAME[AN_TYPES] = { "-", "cow", "sheep", "horse", "deer", "chicken", "dog", "cat", "pigeon", "gull", "duck", "goose" };
static void stay(float x, float y, float secs)
{
    pcar = -1; plx = x; ply = y; camX = x; camY = y;
    memset(anim, 0, sizeof(anim));
    for (int f = 0; f < (int)(secs * 30); f++) { game_input_t in = {0}; game_update(&in, 1.0f / 30); plx = x; ply = y; php = 100; }
}
static void report(const char *where)
{
    int cnt[AN_TYPES] = {0};
    for (int i = 0; i < NANIM; i++) cnt[anim[i].type]++;
    printf("%-12s:", where);
    for (int t = 1; t < AN_TYPES; t++) if (cnt[t]) printf(" %s %d", AN_NAME[t], cnt[t]);
    printf("\n");
}
// find a tile of the needed type/tone near the point
static bool find_tile(int cx, int cy, int kind, int tone, int need_pond, float *x, float *y)
{
    for (int r = 0; r < 120; r++)
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            if (abs(dx) != r && abs(dy) != r) continue;
            int tx = (cx + dx) & (MAP - 1), ty = (cy + dy) & (MAP - 1);
            if (tile_kind(tx, ty) != kind || (tone >= 0 && tile_tone_rt(tx, ty) != tone)) continue;
            if (need_pond && !near_kind(tx, ty, 3, T_WATER, 0)) continue;
            *x = tx * 16 + 8; *y = ty * 16 + 8; return true;
        }
    return false;
}
// position the camera so the nearest animals of type t are visible
static void look_at_type(int t)
{
    float bx = plx, by = ply, bd = 1e9;
    for (int i = 0; i < NANIM; i++) if (anim[i].type == t) { float d = dist2w(plx, ply, anim[i].x, anim[i].y); if (d < bd) { bd = d; bx = anim[i].x; by = anim[i].y; } }
    plx = bx; ply = by + 30; camX = bx; camY = by;
}
int main(int argc, char **argv)
{
    OUT = argc > 1 ? argv[1] : ".";
    game_save_t sv = { 500, 0, 0, 0, 0 };
    FILE *wf = argc > 2 ? fopen(argv[2], "rb") : NULL;
    if (wf) game_set_world_source(f_rd, wf);
    game_init(5, &sv);
    gstate = G_PLAY; day_t = DAY_LEN * 0.4f; tint_timer = 0;
    cinema = true;
    float x, y;
    static const struct { const char *n; int cx, cy, kind, tone, pond, look; } B[] = {
        { "meadow", 300, 760, T_PARK, TN_MEADOW, 0, AN_COW },
        { "meadow2", 700, 330, T_PARK, TN_MEADOW, 0, AN_SHEEP },
        { "farmyard", 600, 200, T_PLAZA, TN_RURAL, 0, AN_CHICKEN }, { "gas_station", 506, 488, T_PLAZA, -1, 0, AN_COW },
        { "forest", 150, 280, T_PARK, TN_FOREST, 0, AN_DEER },
        { "city_plaza", 240, 470, T_PLAZA, -1, 0, AN_PIGEON },
        { "city_resid", 140, 540, T_SIDEWALK, -1, 0, AN_DOG },
        { "park_pond", 284, 420, T_PARK, -1, 1, AN_GOOSE },
        { "lake_south", 430, 760, T_PARK, -1, 1, AN_DUCK },
        { "port_coast", 800, 805, T_PLAZA, -1, 0, AN_GULL },
    };
    for (unsigned b = 0; b < sizeof(B) / sizeof(B[0]); b++) {
        if (!find_tile(B[b].cx, B[b].cy, B[b].kind, B[b].tone, B[b].pond, &x, &y)) { printf("%s: no tile\n", B[b].n); continue; }
        stay(x, y, 8);
        report(B[b].n);
        look_at_type(B[b].look);
        for (int f = 0; f < 6; f++) { game_input_t in = {0}; game_update(&in, 1.0f / 30); }
        camX = plx; camY = ply - 30;
        char nm[64]; snprintf(nm, sizeof nm, "an_%s", B[b].n); shot(nm);
    }
    // ---- geese: approach up close ----
    cinema = false;
    find_tile(284, 420, T_PARK, -1, 1, &x, &y);
    int tries = 0, g = -1;
    while (g < 0 && tries < 12) {
        stay(x, y, 6);
        for (int i = 0; i < NANIM; i++) if (anim[i].type == AN_GOOSE && anim[i].state != AS_DEAD) { g = i; break; }
        tries++;
        if (g < 0) { float a = tries * 1.3f; x = wrapf(x + cosf(a) * 120); y = wrapf(y + sinf(a) * 120); }
    }
    if (g < 0) { printf("GOOSE TEST: no geese spawned\n"); return 1; }
    printf("goose found after %d tries at %.0f,%.0f\n", tries, anim[g].x, anim[g].y);
    // stand 60 px from the goose, then walk toward it
    plx = anim[g].x - 60; ply = anim[g].y; camX = plx; camY = ply;
    int angry_t = -1, dead_t = -1;
    for (int f = 0; f < 30 * 12; f++) {
        game_input_t in = {0};
        float dx = wrapd(anim[g].x - plx), dy = wrapd(anim[g].y - ply), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        if (angry_t < 0) { in.mx = dx / d * 0.5f; in.my = dy / d * 0.5f; }       // approach at a walk
        else { in.mx = -dx / d; in.my = -dy / d; in.held = BTN_RUN; }             // angered - run away
        game_update(&in, 1.0f / 30);
        if (angry_t < 0 && anim[g].state == AS_ANGRY) { angry_t = f; printf("geese angry at t=%.2f s (distance %.1f px)\n", f / 30.0f, d); shot("goose_angry"); }
        if (angry_t >= 0 && f == angry_t + 12) shot("goose_chase");
        if (gstate == G_WASTED && dead_t < 0) { dead_t = f; printf("player killed by geese at t=%.2f s, goosed=%d\n", f / 30.0f, goosed); }
        if (dead_t >= 0 && f == dead_t + 20) { shot("goosed_screen"); break; }
    }
    if (dead_t < 0) printf("GOOSE TEST FAILED: not killed\n");
    // respawn clears the caption
    for (int f = 0; f < 30 * 5 && gstate != G_PLAY; f++) { game_input_t in = {0}; game_update(&in, 1.0f / 30); }
    printf("after respawn: gstate=%d goosed=%d\n", gstate, goosed);
    // ---- shot near geese ----
    stay(x, y, 6);
    g = -1; for (int i = 0; i < NANIM; i++) if (anim[i].type == AN_GOOSE && anim[i].state != AS_DEAD) { g = i; break; }
    if (g >= 0) {
        plx = anim[g].x - 70; ply = anim[g].y; weapon = W_PISTOL; ammo[W_PISTOL] = 50;
        game_input_t in = {0}; in.held = BTN_FIRE; pang = PIf / 2;
        fire_cd = 0; game_update(&in, 1.0f / 30);
        int n = 0; for (int i = 0; i < NANIM; i++) if (anim[i].type == AN_GOOSE && anim[i].state == AS_ANGRY) n++;
        printf("shot fired 70 px from geese -> angry geese: %d\n", n);
        // geese do not kill you in a car
        pcar = 0; cars[0].mode = CAR_PLAYER; cars[0].x = plx; cars[0].y = ply; cars[0].speed = 0; cars[0].hp = 999;
        for (int f = 0; f < 30 * 4; f++) { game_input_t in2 = {0}; game_update(&in2, 1.0f / 30); }
        printf("in car with angry geese for 4 s: gstate=%d (1 = alive)\n", gstate);
    }
    // ---- performance: 40 animals on screen ----
    {
        stay(x, y, 1);
        for (int i = 0; i < NANIM; i++) { anim[i].type = 1 + i % (AN_TYPES - 1); anim[i].state = AS_WALK; anim[i].x = plx + (i % 8) * 30 - 105; anim[i].y = ply + (i / 8) * 30 - 70; anim[i].hp = 5; anim[i].z = 0; }
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < 100; k++) { fauna_update(1.0f / 30); game_begin_frame(); for (int yy = 0; yy < SH; yy += 30) game_render_chunk(frame + yy * SW, yy, 30); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("frame with %d animals: %.3f ms (PC)\n", NANIM, ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 100);
        cinema = true; shot("an_zoo");
    }
    return 0;
}
