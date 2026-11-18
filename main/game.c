// GTA-style prototype for ESP32-S3 + 320x240 LCD.
// Top-down view, 1024x1024 px wrapping city, cars, pedestrians, weapons, police and wanted level.
//
// Graphics optimization:
//  * The city is drawn from an atlas of 16x16 textures, 1 byte per pixel (palette index) — ~25 KB.
//    The atlas is generated at startup from a procedural description and deduplicated,
//    so no flash/SD is needed for graphics and all hot data lives in internal SRAM.
//  * Roof color comes from the palette, not from a separate texture.
//  * Background inner loop: 1 index read + 1 palette read per pixel.
//  * Hot functions are marked HOT (IRAM_ATTR on ESP32) so the BLE stack
//    does not evict them from the instruction cache and cause FPS hitches.

#include "game.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HOT IRAM_ATTR
#else
#define HOT
#endif

// ================= World constants =================
#define TILE   16
#define MAP    1024                // 1024x1024 tiles = 16384x16384 px: two cities, airport, countryside (game_world.inc)
#define MSH    10                  // log2(MAP)
#define WORLD  (MAP * TILE)
#define WMASK  (WORLD - 1)

// Volumetric buildings (GTA1 perspective). 0 = flat roofs, if the ESP32 can't keep up FPS
#define BUILDINGS_3D 1

#define NCIVCARS   16
#define NCOPCARS   5               // 3 chasing + roadblocks
#define POLICE_END (NCIVCARS + NCOPCARS)
#define FIRE_IDX   POLICE_END           // fire truck
#define SUPER_IDX  (POLICE_END + 1)     // VORTEX GT — casino prize
#define NCARS      (POLICE_END + 2)
#define NACT       44              // pedestrians, cops, gangs
#define NCIV_TARGET 24
#define NBUL       60
#define NFX        10
#define NPICK      24             // 8 weapon/health + 2 "massacre" + 10 hidden packages

#define AI_SPEED   47.0f
#define CAR_MAX    180.0f
#define CAR_ACC    145.0f
#define CAR_BRAKE  300.0f
#define CAR_TURN   3.3f
#define COP_CAR_MAX 128.0f
#define WALK_SPEED 62.0f
#define RUN_SPEED  100.0f
#define PED_SPEED  18.0f
#define COP_SPEED  66.0f

static const float PIf = 3.14159265f;

// ================= Utilities =================
static uint32_t rng_state = 12345;
static inline uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static inline float frnd(void) { return (rnd() & 0xFFFF) / 65535.0f; }

static inline uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));   // SPI sends the high byte first
}

static inline float wrapf(float v)
{
    v = fmodf(v, (float)WORLD);
    if (v < 0) v += WORLD;
    return v;
}

static inline float wrapd(float d)
{
    if (d > WORLD / 2) d -= WORLD;
    else if (d < -WORLD / 2) d += WORLD;
    return d;
}

static inline float angle_diff(float a, float b)
{
    float d = a - b;
    while (d > PIf) d -= 2 * PIf;
    while (d < -PIf) d += 2 * PIf;
    return d;
}

static inline float dist2w(float x1, float y1, float x2, float y2)
{
    float dx = wrapd(x2 - x1), dy = wrapd(y2 - y1);
    return dx * dx + dy * dy;
}

// ================= Palette =================
enum {
    P_ASPHALT = 0, P_EDGE, P_LINE, P_SIDEWALK, P_SIDEWALK_J, P_GRASS1, P_GRASS2,
    P_TREE1, P_TREE2, P_TREE3, P_AC, P_AC_D, P_ZEBRA, P_CURB, P_FLOWER, P_ASPH_D,
    P_WATER, P_WATER_L, P_QUAY, P_PLAZA, P_PLAZA_J, P_RAIL, P_FOUNT, P_GLASS_R,
    P_SH_SIDEWALK, P_SH_SIDEWALK_J, P_SH_CURB, P_SH_PLAZA, P_SH_PLAZA_J, P_SH_GRASS1, P_SH_GRASS2, P_SH_FLOWER,
    P_SHALLOW, P_FOAM, P_WSAND, P_SAND, P_SAND_L, P_GRASS_D, P_ROCK_D,     // shore masks (game_shore.inc)
    P_ROOF, P_ROOF_EDGE, P_ROOF_LEDGE, P_ROOF_DARK, P_ROOF_LIGHT,
    P_WALL_1, P_WALL_2, P_WALL_3, P_WALL_BASE, P_WIN, P_WIN_LIT,
    NPALC
};
// Palette rows (high byte of map_v): roof/wall colors of lots and ground "tones" of districts.
// 0 = common ground; 1..4 residential; 5..7 downtown; 8..9 industrial; 10 villas; 11 ghetto; 12 panel blocks; 13 casino; 14 CHIKKO; 15 weapons
// 16..18 — water deeper from the shore; 19.. — new buildings; R_TONE0.. — ground tones (clean downtown, dirty ghetto, fields...)
enum { R_DEEP0 = 16, R_GLASS1 = 19, R_GLASS2, R_GLASS3, R_GLASS4, R_CONT_R, R_CONT_B, R_CONT_G, R_CONT_O, R_CONT_W,
       R_BARN, R_FARM, R_HANGAR, R_TERMINAL, R_WARM1, R_WARM2, R_WARM3, R_GAS, R_WAREH, R_TONE0 };
#define NROOF R_TONE0                  // rows with building colors
#define NPAL  (R_TONE0 + 17)           // + ground tones (TN_N, game_world.inc)
static uint16_t pal[NPAL][NPALC];

static uint16_t C_WHITE, C_BLACK, C_SKIN, C_HEAD, C_TAIL, C_GLASS, C_PICK, C_DROP;
static uint16_t C_PLAYER, C_COP, C_COP_HEAD, C_BLOOD, C_BLOOD_D, C_RED, C_BLUE, C_YELLOW;
static uint16_t C_ORANGE, C_SMOKE, C_WRECK, C_WRECK_D, C_HP_BG, C_HP, C_STAR, C_STAR_OFF;
static uint16_t C_DARK, C_GREEN, C_GREY;
static uint16_t car_body[8], car_roof[8], car_hl[8], shirt_c[6];
static uint16_t C_POL_B, C_POL_R, C_POL_H, C_SUIT, C_GOLD, C_GOLD_R, C_GOLD_H;
static uint16_t C_FIRE_B, C_FIRE_S, C_VX_B, C_VX_S, C_VX_H, C_SP_B, C_SP_S, C_SP_H, C_GANG1, C_GANG2, C_ROBBER;

// ---- Lighting: palettes are stored as RGB888 and tinted by time of day / weather ----
#define PK(r, g, b) ((uint32_t)(((r) > 255 ? 255 : (r)) << 16 | ((g) > 255 ? 255 : (g)) << 8 | ((b) > 255 ? 255 : (b))))
static uint32_t (*pal_rgb)[NPALC];                 // [NPAL][NPALC], on the heap (PSRAM) — saves DRAM
typedef struct { uint16_t *dst; uint32_t rgb; } wcol_t;
static wcol_t wcols[48];
static int nwcols = 0;
#define WC(var, r, g, b) wcol(&(var), (r), (g), (b))
static void wcol(uint16_t *dst, int r, int g, int b)
{
    if (nwcols < 48) { wcols[nwcols].dst = dst; wcols[nwcols].rgb = PK(r, g, b); nwcols++; }
    *dst = rgb565(r, g, b);
}
static int tint_r = 256, tint_g = 256, tint_b = 256;   // 256 = 1.0
static float night = 0;                                 // 0 = day, 1 = deep night

static inline uint16_t tint565(uint32_t rgb)
{
    return rgb565((int)((rgb >> 16) & 255) * tint_r >> 8, (int)((rgb >> 8) & 255) * tint_g >> 8, (int)(rgb & 255) * tint_b >> 8);
}
// tint tables for GTA.PAK sprites (RGB888 -> RGB565 accounting for time of day)
static uint16_t lut_r[256], lut_g[256], lut_b[256];
static void apply_tint(int tr, int tg, int tb, int lit_windows)
{
    tint_r = tr; tint_g = tg; tint_b = tb;
    for (int v = 0; v < 256; v++) {
        int r = v * tr >> 8, g = v * tg >> 8, b = v * tb >> 8;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        lut_r[v] = (uint16_t)((r & 0xF8) << 8); lut_g[v] = (uint16_t)((g & 0xFC) << 3); lut_b[v] = (uint16_t)(b >> 3);
    }
    for (int p = 0; p < NPAL; p++) {
        for (int i = 0; i < NPALC; i++) pal[p][i] = tint565(pal_rgb[p][i]);
        if (lit_windows) pal[p][P_WIN_LIT] = rgb565(255, 214, 130);     // windows glow by themselves at night
    }
    for (int i = 0; i < nwcols; i++) *wcols[i].dst = tint565(wcols[i].rgb);
}
static uint16_t mini_kc[8];
static const uint8_t WATER_DEPTH_K[3] = { 94, 87, 78 };   // % water brightness for rows 16..18

static void init_palette(void)
{
    if (!pal_rgb) pal_rgb = malloc(sizeof(uint32_t) * NPAL * NPALC);
    static const uint8_t base[NPALC][3] = {
        [P_ASPHALT] = {74, 76, 82},    [P_EDGE] = {150, 148, 140},  [P_LINE] = {236, 206, 70},
        [P_SIDEWALK] = {214, 192, 154}, [P_SIDEWALK_J] = {190, 168, 132},
        [P_GRASS1] = {86, 152, 62},    [P_GRASS2] = {72, 136, 54},
        [P_TREE1] = {40, 104, 44},     [P_TREE2] = {26, 72, 30},    [P_TREE3] = {72, 140, 62},
        [P_AC] = {196, 196, 200},      [P_AC_D] = {122, 122, 128},  [P_ZEBRA] = {232, 232, 228},
        [P_CURB] = {150, 134, 110},    [P_FLOWER] = {232, 92, 120}, [P_ASPH_D] = {60, 62, 68},
        [P_WATER] = {64, 132, 196},    [P_WATER_L] = {120, 182, 228}, [P_QUAY] = {150, 146, 138},
        [P_PLAZA] = {228, 210, 176},   [P_PLAZA_J] = {200, 182, 148}, [P_RAIL] = {200, 200, 196},
        [P_FOUNT] = {176, 222, 246},   [P_GLASS_R] = {120, 170, 196},
        [P_SH_SIDEWALK] = {164, 146, 116}, [P_SH_SIDEWALK_J] = {146, 128, 100}, [P_SH_CURB] = {116, 104, 86},
        [P_SH_PLAZA] = {176, 160, 132}, [P_SH_PLAZA_J] = {156, 140, 112},
        [P_SH_GRASS1] = {58, 110, 44}, [P_SH_GRASS2] = {50, 98, 38}, [P_SH_FLOWER] = {170, 66, 90},
        [P_SHALLOW] = {92, 184, 212}, [P_FOAM] = {236, 248, 252}, [P_WSAND] = {176, 152, 104}, [P_SAND] = {230, 208, 150},
        [P_SAND_L] = {244, 228, 176}, [P_GRASS_D] = {44, 104, 46}, [P_ROCK_D] = {98, 96, 94},
    };
    static const uint8_t roofs[NROOF][3] = {
        {150, 110, 90},
        {168, 74, 58}, {132, 86, 64}, {188, 112, 70}, {104, 110, 124},     // residential: red, brown, terracotta, slate
        {150, 150, 158}, {112, 128, 150}, {186, 170, 140},                 // downtown: gray, blue-gray, beige
        {70, 72, 76}, {72, 92, 94},                                        // industrial: dark
        {204, 108, 70}, {112, 100, 86}, {168, 168, 162},                   // villas, ghetto, panel blocks
        {112, 44, 140}, {200, 44, 40}, {88, 98, 64},                       // casino, CHIKKO, weapon shop
        {150, 110, 90}, {150, 110, 90}, {150, 110, 90},                    // 16..18 — water (see below)
        {92, 118, 150}, {78, 126, 128}, {64, 66, 78}, {170, 172, 178},     // 19..22 skyscrapers: blue, turquoise, dark, silver glass
        {176, 58, 44}, {52, 92, 160}, {64, 128, 72}, {214, 128, 40}, {200, 200, 196},   // 23..27 containers
        {150, 52, 40}, {112, 98, 82}, {150, 156, 160}, {214, 216, 218},    // 28 barn, 29 farm, 30 hangar/silo, 31 terminal
        {214, 170, 110}, {190, 96, 56}, {230, 226, 214}, {226, 60, 48}, {122, 116, 104},   // 32..34 "warm" city B, 35 gas station, 36 warehouse
    };
    static const uint8_t walls[NROOF][3] = {
        {196, 176, 150}, {196, 176, 150}, {196, 176, 150}, {196, 176, 150}, {196, 176, 150},
        {150, 150, 154}, {131, 139, 150}, {168, 160, 145}, {100, 102, 106}, {102, 122, 124},
        {242, 236, 222}, {146, 124, 104}, {200, 198, 188}, {226, 190, 96}, {240, 236, 228}, {120, 118, 100},
        {196, 176, 150}, {196, 176, 150}, {196, 176, 150},
        {110, 140, 178}, {100, 156, 158}, {60, 70, 92}, {186, 192, 202},
        {176, 64, 50}, {58, 100, 168}, {70, 136, 80}, {220, 136, 48}, {206, 206, 202},
        {176, 72, 54}, {224, 214, 194}, {132, 138, 142}, {196, 222, 238},
        {232, 206, 160}, {236, 220, 190}, {242, 238, 226}, {238, 238, 238}, {154, 138, 116},
    };
    for (int p = 0; p < NROOF; p++) {
        for (int i = 0; i < NPALC; i++) pal_rgb[p][i] = PK(base[i][0], base[i][1], base[i][2]);
        int r = roofs[p][0], g = roofs[p][1], b = roofs[p][2];
        pal_rgb[p][P_ROOF]       = PK(r, g, b);
        pal_rgb[p][P_ROOF_EDGE]  = PK(r * 50 / 100, g * 50 / 100, b * 50 / 100);
        pal_rgb[p][P_ROOF_LEDGE] = PK(r * 78 / 100, g * 78 / 100, b * 78 / 100);
        pal_rgb[p][P_ROOF_DARK]  = PK(r * 66 / 100, g * 66 / 100, b * 66 / 100);
        pal_rgb[p][P_ROOF_LIGHT] = PK(r * 118 / 100, g * 118 / 100, b * 118 / 100);
        // walls: lighter for residential (stucco), darker for industrial; three tones by cardinal side
        int wr = walls[p][0], wg = walls[p][1], wb = walls[p][2];
        pal_rgb[p][P_WALL_1]    = PK(wr, wg, wb);
        pal_rgb[p][P_WALL_2]    = PK(wr * 84 / 100, wg * 84 / 100, wb * 84 / 100);
        pal_rgb[p][P_WALL_3]    = PK(wr * 64 / 100, wg * 64 / 100, wb * 64 / 100);
        pal_rgb[p][P_WALL_BASE] = PK(wr * 45 / 100, wg * 45 / 100, wb * 45 / 100);
        pal_rgb[p][P_WIN]       = PK(52, 70, 96);
        pal_rgb[p][P_WIN_LIT]   = PK(150, 196, 226);
    }
    // district ground tones: asphalt, sidewalk, plaza, grass 1/2, trees, markings
    static const uint8_t TONES[17][7][3] = {
        { {74, 76, 82},  {214, 192, 154}, {228, 210, 176}, {86, 152, 62},  {72, 136, 54},  {40, 104, 44}, {236, 206, 70} },   // regular
        { {70, 72, 80},  {222, 214, 198}, {236, 230, 216}, {82, 160, 66},  {70, 146, 58},  {38, 110, 48}, {240, 236, 228} },  // clean downtown
        { {68, 70, 76},  {232, 220, 196}, {238, 226, 200}, {76, 172, 70},  {62, 156, 60},  {32, 118, 50}, {236, 206, 70} },   // rich
        { {84, 84, 88},  {178, 172, 160}, {190, 186, 174}, {104, 146, 72}, {92, 130, 64},  {52, 100, 48}, {214, 194, 96} },   // shabby panel blocks
        { {62, 60, 58},  {158, 138, 110}, {168, 148, 118}, {130, 136, 64}, {112, 114, 56}, {64, 92, 44},  {188, 160, 72} },   // dirty ghetto
        { {54, 54, 58},  {150, 146, 140}, {136, 134, 128}, {100, 120, 70}, {88, 104, 62},  {54, 84, 46},  {222, 200, 80} },   // industrial, port
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {98, 164, 64},  {86, 150, 58},  {40, 104, 44}, {232, 232, 224} },  // countryside
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {216, 186, 96}, {190, 158, 72}, {40, 104, 44}, {232, 232, 224} },  // wheat
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {112, 178, 72}, {70, 140, 52},  {40, 104, 44}, {232, 232, 224} },  // seedlings
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {128, 98, 70},  {100, 76, 54},  {40, 104, 44}, {232, 232, 224} },  // plowed field
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {144, 168, 84}, {124, 152, 70}, {40, 104, 44}, {232, 232, 224} },  // meadow
        { {96, 96, 100}, {156, 136, 104}, {170, 150, 116}, {46, 102, 46},  {36, 84, 38},   {26, 70, 32},  {232, 232, 224} },  // forest
        { {84, 86, 92},  {190, 190, 186}, {178, 180, 178}, {110, 162, 76}, {98, 148, 68},  {40, 104, 44}, {236, 206, 70} },   // airport
        { {62, 64, 70},  {156, 136, 104}, {170, 150, 116}, {98, 164, 64},  {86, 150, 58},  {40, 104, 44}, {242, 242, 238} },  // highway
        { {82, 78, 76},  {226, 196, 150}, {232, 206, 160}, {108, 160, 64}, {94, 146, 56},  {44, 106, 44}, {236, 206, 70} },   // city B
        { {76, 76, 82},  {236, 222, 190}, {240, 230, 204}, {94, 168, 70},  {80, 152, 62},  {36, 112, 48}, {242, 238, 230} },  // city B, downtown
        { {70, 64, 60},  {184, 156, 116}, {188, 162, 126}, {146, 142, 70}, {126, 122, 60}, {66, 94, 44},  {196, 168, 80} },   // city B, ghetto
    };
    for (int t = 1; t < 17; t++) {
        uint32_t *r = pal_rgb[R_TONE0 + t];
        for (int i = 0; i < NPALC; i++) r[i] = pal_rgb[0][i];
        const uint8_t (*T)[3] = TONES[t];
#define TC(e, k, m) r[e] = PK(T[k][0] * (m) / 100, T[k][1] * (m) / 100, T[k][2] * (m) / 100)
        TC(P_ASPHALT, 0, 100); TC(P_ASPH_D, 0, 81); TC(P_EDGE, 1, 70);
        TC(P_SIDEWALK, 1, 100); TC(P_SIDEWALK_J, 1, 89); TC(P_CURB, 1, 70);
        TC(P_PLAZA, 2, 100); TC(P_PLAZA_J, 2, 88);
        TC(P_GRASS1, 3, 100); TC(P_GRASS2, 4, 100);
        TC(P_TREE1, 5, 100); TC(P_TREE2, 5, 70); TC(P_TREE3, 5, 160);
        TC(P_LINE, 6, 100);
        TC(P_SH_SIDEWALK, 1, 76); TC(P_SH_SIDEWALK_J, 1, 68); TC(P_SH_CURB, 1, 54);
        TC(P_SH_PLAZA, 2, 77); TC(P_SH_PLAZA_J, 2, 68); TC(P_SH_GRASS1, 3, 68); TC(P_SH_GRASS2, 4, 68);
#undef TC
    }
    // water deeper from the shore: three darkening steps (palette rows 16..18)
    for (int p = 16; p < 19; p++) {
        int k = WATER_DEPTH_K[p - 16];
        pal_rgb[p][P_WATER] = PK(64 * k / 100, 132 * k / 100, 196 * k / 100);
        pal_rgb[p][P_WATER_L] = PK(120 * k / 100, 182 * k / 100, 228 * k / 100);
    }

    C_WHITE = rgb565(255, 255, 255); C_BLACK = rgb565(0, 0, 0);
    WC(C_SKIN, 230, 190, 150);  C_HEAD = rgb565(255, 250, 200);
    C_TAIL = rgb565(200, 30, 30);    WC(C_GLASS, 60, 80, 110);
    C_PICK = rgb565(60, 220, 90);    C_DROP = rgb565(250, 210, 40);
    C_PLAYER = rgb565(255, 120, 0);  WC(C_COP, 30, 50, 140);
    WC(C_COP_HEAD, 15, 20, 60); WC(C_BLOOD, 150, 10, 10);
    WC(C_BLOOD_D, 100, 5, 5);   C_RED = rgb565(240, 30, 30);
    C_BLUE = rgb565(40, 90, 255);    C_YELLOW = rgb565(255, 230, 80);
    C_ORANGE = rgb565(255, 140, 20); WC(C_SMOKE, 90, 90, 90);
    WC(C_WRECK, 45, 42, 40);    WC(C_WRECK_D, 25, 22, 20);
    C_HP_BG = rgb565(60, 0, 0);      C_HP = rgb565(230, 40, 40);
    C_STAR = rgb565(255, 220, 40);   C_STAR_OFF = rgb565(70, 70, 70);
    WC(C_DARK, 20, 20, 24);     C_GREEN = rgb565(60, 220, 90);
    C_GREY = rgb565(150, 150, 150);
    mini_kc[0] = rgb565(96, 96, 102);   mini_kc[1] = rgb565(206, 186, 150); mini_kc[2] = 0;
    mini_kc[3] = rgb565(80, 150, 64);   mini_kc[4] = rgb565(60, 120, 190);  mini_kc[5] = rgb565(228, 210, 176);

    static const uint8_t cars_rgb[8][3] = {
        {200, 40, 40}, {40, 80, 200}, {230, 200, 40}, {235, 235, 235},
        {40, 160, 70}, {230, 120, 30}, {130, 60, 170}, {30, 30, 34},
    };
    for (int i = 0; i < 8; i++) {
        int r = cars_rgb[i][0], g = cars_rgb[i][1], b = cars_rgb[i][2];
        WC(car_body[i], r, g, b);
        WC(car_roof[i], r * 8 / 10, g * 8 / 10, b * 8 / 10);
        WC(car_hl[i], r + (255 - r) / 3, g + (255 - g) / 3, b + (255 - b) / 3);
    }
    static const uint8_t shirts[6][3] = {
        {50, 90, 200}, {200, 50, 60}, {240, 240, 240},
        {60, 160, 80}, {160, 80, 200}, {90, 70, 50},
    };
    for (int i = 0; i < 6; i++) WC(shirt_c[i], shirts[i][0], shirts[i][1], shirts[i][2]);
    WC(C_POL_B, 236, 236, 236); WC(C_POL_R, 25, 30, 70); WC(C_POL_H, 255, 255, 255);
    WC(C_SUIT, 30, 30, 34);
    WC(C_FIRE_B, 205, 32, 28); WC(C_FIRE_S, 150, 20, 18); WC(C_VX_B, 36, 34, 44); WC(C_VX_S, 20, 20, 26); WC(C_VX_H, 120, 236, 255);
    WC(C_SP_B, 236, 120, 24); WC(C_SP_S, 170, 70, 10); WC(C_SP_H, 255, 200, 120);
    WC(C_GANG1, 40, 170, 60); WC(C_GANG2, 150, 50, 190); WC(C_ROBBER, 70, 64, 56); WC(C_GOLD, 220, 180, 50); WC(C_GOLD_R, 176, 140, 36); WC(C_GOLD_H, 250, 230, 140);
    apply_tint(256, 256, 256, 0);
}

// ================= City =================
enum { T_ROAD = 0, T_SIDEWALK, T_BUILDING, T_PARK, T_WATER, T_PLAZA, T_KINDS };

// Lane flags: direction 0 = east, 1 = south, 2 = west, 3 = north
enum { LF_E = 1, LF_S = 2, LF_W = 4, LF_N = 8 };

// Tile atlas: up to 1024 textures. The most frequent live in internal SRAM, the rest in PSRAM.
#define MAX_TEX 1024
#define TEX_SRAM 192            // 192 frequent textures (~99% of tiles) in internal SRAM — saves 16 KB DRAM for Bluetooth
static uint8_t tex_sram[TEX_SRAM][TILE * TILE];  // hot render data
static uint8_t *tex_psram;                       // (MAX_TEX - TEX_SRAM) * 256
static const uint8_t *tex_ptr[MAX_TEX];
static uint32_t *tex_hash;                       // only during generation
static uint8_t tex_kind[MAX_TEX];
static int ntex = 0;
static inline uint8_t *tex_w(int i) { return (uint8_t *)tex_ptr[i]; }

// map_v: (palette row << 10) | texture (up to 64 palette rows)
#define MV_TEX(v) ((v) & 1023)
#define MV_PAL(v) ((v) >> 10)
static uint16_t *map_v;      // [MAP*MAP] 2 MB, PSRAM
static uint8_t *lanes;       // [MAP*MAP] lane flags for AI
// pathfinding: 256x256 tile window with "overlap" (BFS no farther than 90 tiles — no collisions)
#define NAVW 256
#define NI(x, y) (((((y) & (NAVW - 1)) << 8) | ((x) & (NAVW - 1))))
static uint8_t *navd;        // [NAVW*NAVW] distance to player (police pathfinding)
static uint16_t *navq;       // BFS queue (window indices)
static uint8_t *navf;        // path field to the fire (for the fire truck)

enum { PT_SMOKE = 0, PT_SPARK, PT_DEBRIS, PT_SPLASH, PT_EXHAUST, PT_BLOOD, PT_WATER, PT_CASING };   // particle types
static void gen_props(void);

#define MI(x, y) ((((y) & (MAP - 1)) << MSH) | ((x) & (MAP - 1)))

// ----- lots (buildings) -----
#define MAX_LOTS 16000
enum { ST_HOUSE = 0, ST_FLAT, ST_INDUSTRY, ST_VILLA, ST_SHACK, ST_BLOCK, ST_TOWER, ST_CONTAINER, ST_BARN, ST_HANGAR };
#define IS_HOUSELIKE(st) ((st) == ST_HOUSE || (st) == ST_VILLA || (st) == ST_SHACK || (st) == ST_BARN)
typedef struct { uint16_t x0, y0, x1, y1; uint8_t style, pal, h10, pad; } lot_t;   // h10 — building height x10
static uint8_t *g_kind;      // tile type + bit 0x80 "bridge"; only during generation
static uint16_t *g_lot;      // building lot number; only during generation
static lot_t *g_lots;
static int g_nlots;
#define MAX_FOUNT 4
static int fount_x[MAX_FOUNT], fount_y[MAX_FOUNT], nfount = 0;   // fountain centers, px

static inline int gk(int tx, int ty) { return g_kind[MI(tx, ty)] & 0x7F; }
static inline int gkp(int px, int py) { return gk((px & WMASK) >> 4, (py & WMASK) >> 4); }

// Districts
enum { D_RESID = 0, D_DOWNTOWN, D_INDUSTRY, D_PARK, D_PLAZA, D_RICH, D_GHETTO, D_SLEEP, D_FOREST,
       D_TOWERS, D_PORT, D_COUNTRY, D_VILLAGE, D_AIRPORT, D_NUM };
static int g_city_ctx, g_dist_ctx;

static void add_lot(int x0, int y0, int x1, int y1, int style)
{
    if (g_nlots >= MAX_LOTS - 1) return;
    if (x1 - x0 < 1 || y1 - y0 < 1) return;                // smaller than 2x2 — don't build
    int id = ++g_nlots;
    lot_t *l = &g_lots[id];
    l->x0 = x0; l->y0 = y0; l->x1 = x1; l->y1 = y1; l->style = style; l->pad = 0;
    bool B = g_city_ctx == 2;
    switch (style) {
    case ST_HOUSE:     l->pal = B ? R_WARM1 + rnd() % 3 : g_dist_ctx == D_VILLAGE ? ((rnd() % 3) ? 1 + rnd() % 4 : R_FARM) : 1 + rnd() % 4; break;
    case ST_FLAT:      l->pal = B ? ((rnd() % 3) ? R_WARM1 + (rnd() & 1) * 2 : 7) : 5 + rnd() % 3; break;
    case ST_INDUSTRY:  l->pal = (rnd() % 3 == 0) ? R_WAREH : 8 + rnd() % 2; break;
    case ST_VILLA:     l->pal = B ? ((rnd() & 1) ? R_WARM3 : R_WARM2) : (rnd() & 1) ? 10 : 3; break;
    case ST_SHACK:     l->pal = (rnd() & 1) ? 11 : 2; break;
    case ST_TOWER:     l->pal = R_GLASS1 + rnd() % 4; break;
    case ST_CONTAINER: l->pal = R_CONT_R + rnd() % 5; break;
    case ST_BARN:      l->pal = R_BARN; break;
    case ST_HANGAR:    l->pal = R_HANGAR; break;
    default:           l->pal = (rnd() & 1) ? 12 : 7; break;
    }
    // height: houses 1 floor, downtown 2..5 floors (bigger lot = taller), warehouses 1.5, skyscrapers 15..30
    int area = (x1 - x0 + 1) * (y1 - y0 + 1);
    switch (style) {
    case ST_HOUSE:     l->h10 = 7 + rnd() % 3; break;
    case ST_FLAT:      l->h10 = 14 + rnd() % 10 + (area > 12 ? 8 : 0); break;
    case ST_INDUSTRY:  l->h10 = 10 + rnd() % 3; break;
    case ST_VILLA:     l->h10 = 9 + rnd() % 3; break;
    case ST_SHACK:     l->h10 = 6 + rnd() % 2; break;
    case ST_TOWER:     l->h10 = 54 + rnd() % 26 + (area > 16 ? 16 : 0); break;
    case ST_CONTAINER: l->h10 = 5 + rnd() % 2; break;
    case ST_BARN:      l->h10 = 10 + rnd() % 2; break;
    case ST_HANGAR:    l->h10 = 17 + rnd() % 3; break;
    default:           l->h10 = 34 + rnd() % 12; break;     // panel high-rises
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            g_kind[MI(x, y)] = T_BUILDING;
            g_lot[MI(x, y)] = id;
        }
}

// Splitting a block into lots (BSP)
static void split_area(int x0, int y0, int x1, int y1, int d, int yard)
{
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    int maxw, maxh, minw;
    if (d == D_RESID)         { maxw = 4; maxh = 4; minw = 2; }
    else if (d == D_DOWNTOWN) { maxw = 7; maxh = 6; minw = 3; }
    else if (d == D_TOWERS)   { maxw = 7; maxh = 7; minw = 4; }
    else if (d == D_RICH)     { maxw = 7; maxh = 5; minw = 4; }
    else if (d == D_GHETTO)   { maxw = 3; maxh = 3; minw = 2; }
    else if (d == D_SLEEP)    { maxw = 10; maxh = 7; minw = 4; }
    else if (d == D_VILLAGE)  { maxw = 6; maxh = 6; minw = 3; }
    else if (d == D_PORT)     { maxw = 9; maxh = 6; minw = 3; }
    else                      { maxw = 12; maxh = 4; minw = 3; }
    bool cut_x = w > maxw, cut_y = h > maxh;
    if (cut_x && cut_y) { if (w >= h) cut_y = false; else cut_x = false; }
    if (cut_x && w >= 2 * minw) {
        int s = x0 + minw + rnd() % (w - 2 * minw + 1);
        split_area(x0, y0, s - 1, y1, d, yard);
        split_area(s, y0, x1, y1, d, yard);
        return;
    }
    if (cut_y && h >= 2 * minw) {
        int s = y0 + minw + rnd() % (h - 2 * minw + 1);
        split_area(x0, y0, x1, s - 1, d, yard);
        split_area(x0, s, x1, y1, d, yard);
        return;
    }
    // port: rows of containers (2 tiles wide) or a warehouse
    if (d == D_PORT) {
        if (rnd() % 3 == 0 && w >= 4 && h >= 3) { split_area(x0, y0, x1, y1, D_INDUSTRY, yard); return; }
        for (int y = y0; y + 1 <= y1 - 1; y += 3)
            for (int x = x0; x + 2 <= x1 - 1;) {
                int L = 3 + rnd() % 2;
                if (x + L - 1 > x1 - 1) L = x1 - x;
                bool ok = L >= 2;
                for (int yy = y; yy <= y + 1 && ok; yy++) for (int xx = x; xx < x + L; xx++) if (g_kind[MI(xx, yy)] != yard) ok = false;
                if (ok) add_lot(x, y, x + L - 1, y + 1, ST_CONTAINER);
                x += L + 1;
            }
        return;
    }
    // leaf: building with a yard on the right/bottom
    int bx0 = x0, by0 = y0, bx1 = x1, by1 = y1;
    int style = ST_INDUSTRY;
    switch (d) {
    case D_RESID:    style = ST_HOUSE; if (w >= 3) bx1--; if (h >= 3) by1--; break;
    case D_VILLAGE:  style = ST_HOUSE; bx0++; by0++; if (w >= 4) bx1--; if (h >= 4) by1--; if (bx1 - bx0 > 2) bx1 = bx0 + 2; if (by1 - by0 > 2) by1 = by0 + 2; break;
    case D_DOWNTOWN: style = ST_FLAT;  if (w >= 4) bx1--; if (h >= 4) by1--; break;
    case D_TOWERS:   style = ST_TOWER; bx0++; by0++; if (w >= 4) bx1--; if (h >= 4) by1--; break;
    case D_GHETTO:   style = ST_SHACK; if (w >= 3) bx1--; if (h >= 3) by1--; break;
    case D_RICH:     style = ST_VILLA; if (w >= 5) bx1 -= 2; else if (w >= 3) bx1--; if (h >= 3) by1--; break;
    case D_SLEEP:    style = ST_BLOCK; by1 = y0 + 1; if (w >= 6) { bx0++; bx1--; } break;   // long house 2 tiles deep, yard below it
    default:         if (h >= 3) by1--; break;
    }
    // all building tiles must be free
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++)
            if (g_kind[MI(x, y)] != yard) return;
    add_lot(bx0, by0, bx1, by1, style);
    // villa pool
    if (d == D_RICH && w >= 5 && h >= 3) {
        bool ok = true;
        for (int y = y0; y <= y0 + 1; y++)
            for (int x = x1 - 1; x <= x1; x++) if (g_kind[MI(x, y)] != yard) ok = false;
        if (ok) for (int y = y0; y <= y0 + 1; y++) for (int x = x1 - 1; x <= x1; x++) g_kind[MI(x, y)] = T_WATER;
    }
}

#include "game_shore.inc"
#include "game_world.inc"

// Building shadow (light from top-left): shade the ground if a building stands 5 px to the northwest
static const uint8_t SHADE_OF[NPALC] = {
    [P_SIDEWALK] = P_SH_SIDEWALK, [P_SIDEWALK_J] = P_SH_SIDEWALK_J, [P_CURB] = P_SH_CURB,
    [P_PLAZA] = P_SH_PLAZA, [P_PLAZA_J] = P_SH_PLAZA_J, [P_GRASS1] = P_SH_GRASS1,
    [P_GRASS2] = P_SH_GRASS2, [P_FLOWER] = P_SH_FLOWER, [P_TREE3] = P_TREE1, [P_TREE1] = P_TREE2,
};

static inline int guse(int tx, int ty) { return g_use[MI(tx, ty)] & 0x3F; }

static uint8_t ground_pixel(int wx, int wy, int k)
{
    int tx = wx >> 4, ty = wy >> 4, lx = wx & 15, ly = wy & 15;
    int u = guse(tx, ty);
    // fountains
    for (int f = 0; f < nfount; f++) {
        int dx = wx - fount_x[f], dy = wy - fount_y[f], d2 = dx * dx + dy * dy;
        if (d2 < 22 * 22) {
            if (d2 < 3 * 3) return P_FOUNT;
            if (d2 < 6 * 6) return P_QUAY;
            if (d2 < 18 * 18) return ((dx * dx + dy * dy) % 37 < 5) ? P_FOUNT : P_WATER_L;
            return P_QUAY;
        }
    }
    if (k == T_SIDEWALK) {
        if ((ly < 2 && gk(tx, ty - 1) == T_ROAD) || (ly > 13 && gk(tx, ty + 1) == T_ROAD) ||
            (lx < 2 && gk(tx - 1, ty) == T_ROAD) || (lx > 13 && gk(tx + 1, ty) == T_ROAD)) return P_CURB;
        return ((lx & 7) == 0 || (ly & 7) == 0) ? P_SIDEWALK_J : P_SIDEWALK;
    }
    if (k == T_PLAZA) {
        if (u == U_APRON) {                                   // concrete apron slabs, yellow parking-stand lines
            if (ty >= 233 && ty <= 258 && (tx % 6) == 0 && (lx == 7 || lx == 8)) return P_LINE;
            return (lx == 0 || ly == 0) ? P_PLAZA_J : P_PLAZA;
        }
        if (u == U_LOT) {                                     // parking lot: asphalt and white lines
            if (lx == 0 && ly >= 2 && ly <= 13) return P_ZEBRA;
            return P_ASPHALT;
        }
        if (u == U_GAS) {                                     // gas station: islands with pumps
            if ((tx & 1) == 0 && (ty % 3) == 1 && lx >= 4 && lx <= 11 && ly >= 6 && ly <= 9) return (lx == 4 || ly == 6) ? P_AC : P_AC_D;
            return (lx == 0 || ly == 0) ? P_PLAZA_J : P_PLAZA;
        }
        if (u == U_YARD) {                                    // farm yard: trampled earth
            uint32_t g2 = hash2((lx >> 1) + (tx & 1) * 9, (ly >> 1) + (ty & 1) * 5);
            return (g2 & 3) == 0 ? P_PLAZA_J : P_PLAZA;
        }
        bool j = (lx == 0 || ly == 0 || (((lx >> 2) + (ly >> 2)) & 1 && (lx & 3) == 0 && (ly & 3) == 0));
        return j ? P_PLAZA_J : P_PLAZA;
    }
    // fields and forests
    if (u == U_FIELD_H || u == U_FIELD_V) {                // crop rows + occasional bald patches (2 tile variants)
        int a = u == U_FIELD_H ? ly : lx, b = u == U_FIELD_H ? lx : ly;
        if ((hash2(b >> 1, a + (hash2(tx, ty) & 1) * 16) & 63) == 0) return P_GRASS2;
        return ((a >> 1) & 1) ? P_GRASS1 : P_GRASS2;
    }
    if (u == U_PLOUGH_H) return (ly & 3) == 0 ? P_GRASS2 : P_GRASS1;
    if (u == U_PLOUGH_V) return (lx & 3) == 0 ? P_GRASS2 : P_GRASS1;
    if (u == U_FOREST && guse(tx - 1, ty) == U_FOREST && guse(tx + 1, ty) == U_FOREST && guse(tx, ty - 1) == U_FOREST && guse(tx, ty + 1) == U_FOREST) {
        // continuous forest canopy: crowns (light from top-left, shadow bottom-right), 4 tile variants
        int var = hash2(tx, ty) & 3, best = 999, bdx = 0, bdy = 0;
        for (int c = 0; c < 5; c++) {
            uint32_t h = hash2(c * 7 + var * 31, var * 13 + c);
            int cx = (c == 4) ? 8 : 3 + (c & 1) * 10, cy = (c == 4) ? 8 : 3 + (c >> 1) * 10;
            cx += (int)(h % 5) - 2; cy += (int)((h >> 4) % 5) - 2;
            int dx = lx - cx, dy = ly - cy, d2 = dx * dx + dy * dy - (int)((h >> 8) & 7);
            if (d2 < best) { best = d2; bdx = dx; bdy = dy; }
        }
        if (best > 30) return P_TREE2;                          // gap between crowns
        if (bdx + bdy <= -3 && best < 18) return P_TREE3;       // lit edge of a crown
        if (bdx + bdy >= 4) return P_TREE2;                     // shadow
        return P_TREE1;
    }
    // park / yard / meadow
    uint32_t h = hash2(tx, ty);
    int var = h & 1;             // 2 grass variants — fewer unique textures
    int dd = g_dist[MI(tx, ty)];
    bool city = (dd >> 5) != 0;
    bool wide = gk(tx - 1, ty) == T_PARK && gk(tx + 1, ty) == T_PARK && gk(tx, ty - 1) == T_PARK && gk(tx, ty + 1) == T_PARK;
    if (city && wide && (h >> 4) % 7 == 1 && lx >= 6 && lx <= 9 && ly >= 6 && ly <= 9 && ((lx + ly) & 1)) return P_FLOWER;
    if ((dd & 31) == D_GHETTO) {                      // ghetto: trampled earth and trash
        uint32_t g2 = hash2((lx >> 2) + var * 5, (ly >> 2) + var * 11);
        if ((g2 & 3) == 0) return P_SIDEWALK_J;
    }
    // hedge along the yard edge by the sidewalk (in the city)
    if (city && ((ly < 2 && gk(tx, ty - 1) == T_SIDEWALK) || (lx < 2 && gk(tx - 1, ty) == T_SIDEWALK))) return P_TREE2;
    return (hash2((lx >> 2) + var * 7, (ly >> 2) + var * 13) & 1) ? P_GRASS1 : P_GRASS2;
}

static uint8_t road_pixel(int wx, int wy, bool bridge)
{
    int tx = wx >> 4, ty = wy >> 4, lx = wx & 15, ly = wy & 15;
    int u = guse(tx, ty);
    if (u == U_RUNWAY) {
        int r = 0, tl = 0, tr = 0;
        while (r < 4 && guse(tx, ty - r - 1) == U_RUNWAY) r++;
        while (tl < 8 && guse(tx - tl - 1, ty) == U_RUNWAY) tl++;
        while (tr < 8 && guse(tx + tr + 1, ty) == U_RUNWAY) tr++;
        int py = r * 16 + ly, pe = tl * 16 + lx;
        if (tr * 16 + 15 - lx < pe) pe = tr * 16 + 15 - lx;
        if (py < 2 || py > 61) return P_EDGE;
        if (py < 4 || py > 59) return P_ZEBRA;
        if (pe < 3) return P_ZEBRA;
        if (pe < 44 && py > 7 && py < 57 && ((py >> 2) & 1) && pe > 8) return P_ZEBRA;   // "piano keys" at the ends
        if ((py == 31 || py == 32) && (tx % 3) != 0 && pe > 50) return P_ZEBRA;          // dashed center line
        if ((hash2((lx >> 1) + (tx & 1) * 8, (ly >> 1) + (ty & 1) * 8) & 15) == 0) return P_ASPH_D;
        return P_ASPHALT;
    }
    if (u == U_TAXI) {
        int rh = 0, rv = 0;                               // taxiway direction — by the length of the solid stretch
        for (int k = 1; k <= 3; k++) { rh += guse(tx - k, ty) == U_TAXI; rh += guse(tx + k, ty) == U_TAXI; rv += guse(tx, ty - k) == U_TAXI; rv += guse(tx, ty + k) == U_TAXI; }
        bool hz = rh > rv + 1, vt = rv > rh + 1;
        if ((ly == 0 && gk(tx, ty - 1) != T_ROAD) || (ly == 15 && gk(tx, ty + 1) != T_ROAD) ||
            (lx == 0 && gk(tx - 1, ty) != T_ROAD) || (lx == 15 && gk(tx + 1, ty) != T_ROAD)) return P_EDGE;
        if (hz && !vt) { if ((ly == 15 && guse(tx, ty + 1) == U_TAXI) || (ly == 0 && guse(tx, ty - 1) == U_TAXI)) return P_LINE; }
        else if (vt && !hz) { if ((lx == 15 && guse(tx + 1, ty) == U_TAXI) || (lx == 0 && guse(tx - 1, ty) == U_TAXI)) return P_LINE; }
        return P_ASPHALT;
    }
    int f = lanes[MI(tx, ty)];
    int rt = g_rt[MI(tx, ty)];
    bool h = f & (LF_E | LF_W), v = f & (LF_S | LF_N);
    // bridge railings
    if (bridge) {
        if ((ly < 2 && gk(tx, ty - 1) == T_WATER) || (ly > 13 && gk(tx, ty + 1) == T_WATER) ||
            (lx < 2 && gk(tx - 1, ty) == T_WATER) || (lx > 13 && gk(tx + 1, ty) == T_WATER))
            return (((lx + ly) & 3) == 0) ? P_QUAY : P_RAIL;
    }
    // roadway edge
    bool en = gk(tx, ty - 1) != T_ROAD, es = gk(tx, ty + 1) != T_ROAD, ew = gk(tx - 1, ty) != T_ROAD, ee = gk(tx + 1, ty) != T_ROAD;
    if ((ly == 0 && en) || (ly == 15 && es) || (lx == 0 && ew) || (lx == 15 && ee)) return P_EDGE;
    bool city = rt == RT_CITY || rt == RT_PORT;
    if (!city && ((ly == 2 && en && h) || (ly == 13 && es && h) || (lx == 2 && ew && v) || (lx == 13 && ee && v))) return P_ZEBRA;   // solid line along the road edge
    if (h && !v) {
        bool zl = (lanes[MI(tx - 1, ty)] & (LF_S | LF_N)) != 0, zr = (lanes[MI(tx + 1, ty)] & (LF_S | LF_N)) != 0;
        if (city && (zl || zr) && lx >= 2 && lx <= 13 && ((ly >> 1) & 1) == 0) return P_ZEBRA;
        if (((f & LF_W) && ly == 15) || ((f & LF_E) && ly == 0)) return ((lx >> 3) & 1) ? P_ASPHALT : P_LINE;
    } else if (v && !h) {
        bool zu = (lanes[MI(tx, ty - 1)] & (LF_E | LF_W)) != 0, zd = (lanes[MI(tx, ty + 1)] & (LF_E | LF_W)) != 0;
        if (city && (zu || zd) && ly >= 2 && ly <= 13 && ((lx >> 1) & 1) == 0) return P_ZEBRA;
        if (((f & LF_S) && lx == 15) || ((f & LF_N) && lx == 0)) return ((ly >> 3) & 1) ? P_ASPHALT : P_LINE;
    }
    if ((hash2((lx >> 1) + (tx & 1) * 8, (ly >> 1) + (ty & 1) * 8) & 15) == 0) return P_ASPH_D;
    return P_ASPHALT;
}

static uint8_t water_pixel(int wx, int wy)
{
    int tx = wx >> 4, ty = wy >> 4, lx = wx & 15, ly = wy & 15;
    // shore: 2 px stone embankment
    if ((ly < 2 && gk(tx, ty - 1) != T_WATER) || (ly > 13 && gk(tx, ty + 1) != T_WATER) ||
        (lx < 2 && gk(tx - 1, ty) != T_WATER) || (lx > 13 && gk(tx + 1, ty) != T_WATER)) return P_QUAY;
    if ((ly < 4 && gk(tx, ty - 1) != T_WATER) || (lx < 4 && gk(tx - 1, ty) != T_WATER)) return P_WATER_L;
    // waves
    if (((lx + (ly >> 2) * 5 + (tx & 1) * 7) & 15) < 3 && (ly & 3) == 1) return P_WATER_L;
    return P_WATER;
}

static uint8_t building_pixel(int wx, int wy)
{
    int tx = wx >> 4, ty = wy >> 4;
    const lot_t *l = &g_lots[g_lot[MI(tx, ty)]];
    int px = wx - l->x0 * 16, py = wy - l->y0 * 16;
    int W = (l->x1 - l->x0 + 1) * 16, H = (l->y1 - l->y0 + 1) * 16;
    int e = px < py ? px : py;
    if (W - 1 - px < e) e = W - 1 - px;
    if (H - 1 - py < e) e = H - 1 - py;
    if (e < 2) return P_ROOF_EDGE;
    bool horiz = W >= H;
    if (l->style == ST_CONTAINER) {                  // containers: corrugated roofs, seams every 3 tiles... every 40 px
        int b = horiz ? px : py;
        if (b % 40 == 39) return P_ROOF_EDGE;
        return (b & 1) ? P_ROOF : P_ROOF_LIGHT;
    }
    if (IS_HOUSELIKE(l->style)) {
        // gabled roof: light and dark slope, ridge
        int a = horiz ? py : px, L = horiz ? H : W;
        int mid = L / 2;
        if (a == mid || a == mid - 1) return P_ROOF_EDGE;
        // chimney (the barn has none)
        int cx = 6, cy = 5;
        if (l->style != ST_BARN && px >= cx && px < cx + 5 && py >= cy && py < cy + 5) return (px == cx || py == cy) ? P_ROOF_LIGHT : P_ROOF_DARK;
        if (a < mid) return ((a & 3) == 0) ? P_ROOF : P_ROOF_LIGHT;
        return ((a & 3) == 0) ? P_ROOF_DARK : P_ROOF;
    }
    if (l->style == ST_HANGAR) {                     // hangar: arched roof — light/shadow stripes across
        int a = horiz ? py : px, L = horiz ? H : W, b = horiz ? px : py;
        if (b % 24 == 0) return P_ROOF_DARK;
        if ((b & 3) == 0) return P_ROOF_LEDGE;
        int q = a * 6 / L;
        return q <= 1 ? P_ROOF_LIGHT : q >= 5 ? P_ROOF_DARK : P_ROOF;
    }
    if (e < 4) return P_ROOF_LEDGE;
    if (l->style == ST_TOWER) {                      // skyscraper: glass belt, mechanical floor, helipad
        if (e < 7) return P_GLASS_R;
        if (W >= 64 && H >= 64) {
            int dx = px - W / 2, dy = py - H / 2, d2 = dx * dx + dy * dy;
            if (d2 < 17 * 17 && d2 >= 15 * 15) return P_LINE;
            if (d2 < 15 * 15) {
                if ((abs(dx) <= 1 && abs(dy) <= 6) || (abs(dx - 5) <= 1 && abs(dy) <= 6) || (abs(dx + 5) <= 1 && abs(dy) <= 6)) {
                    if (abs(dx) <= 1 && abs(dy) > 1) return P_ROOF_DARK;
                    return P_ZEBRA;
                }
                return P_ROOF_DARK;
            }
        }
        if (((px - 7) % 24) == 23 || ((py - 7) % 24) == 23) return P_ROOF_DARK;
        int lx = wx & 15, ly = wy & 15;
        uint32_t hs = hash2(tx * 3 + l->x0, ty * 5 + l->y0);
        if ((hs & 7) == 1 && lx >= 3 && lx < 13 && ly >= 3 && ly < 13) return (lx == 3 || ly == 3) ? P_AC : P_AC_D;
        return P_ROOF;
    }
    if (l->style == ST_INDUSTRY) {
        int a = horiz ? py : px, b = horiz ? px : py, Lb = horiz ? W : H;
        if (b >= 7 && b < Lb - 7 && a >= 6 && ((a - 6) & 15) < 4) return (((a - 6) & 15) == 0) ? P_ROOF_LIGHT : P_GLASS_R;
        return (b & 1) ? P_ROOF : P_ROOF_DARK;
    }
    // flat roof: vents and exits, per tile
    int lx = wx & 15, ly = wy & 15;
    bool inner = tx > l->x0 && tx < l->x1 && ty > l->y0 && ty < l->y1;
    uint32_t hs = hash2(tx * 3 + l->x0, ty * 5 + l->y0);
    int det = inner ? ((hs & 3) == 0 ? 2 : (hs & 3) == 1 ? 1 : 0) : 0;
    if (det == 1 && lx >= 2 && lx < 14 && ly >= 3 && ly < 13) {
        if (lx == 2 || lx == 13 || ly == 3 || ly == 12) return P_AC_D;
        return ((lx - 2) % 3 == 1) ? P_AC_D : P_AC;
    }
    if (det == 2 && lx >= 3 && lx < 13 && ly >= 4 && ly < 12) return (lx == 3 || ly == 4) ? P_ROOF_LIGHT : P_ROOF_DARK;
    if (((px - 4) % 32) == 31 || ((py - 4) % 32) == 31) return P_ROOF_DARK;
    return P_ROOF;
}

// Reference pixel (palette index). Called only during atlas generation.
static uint8_t ref_pixel(int wx, int wy)
{
    wx &= WMASK; wy &= WMASK;
    int raw = g_kind[MI(wx >> 4, wy >> 4)];
    int k = raw & 0x7F;
    switch (k) {
    case T_ROAD:     return road_pixel(wx, wy, raw & 0x80);
    case T_WATER:    return water_pixel(wx, wy);
    case T_BUILDING: return building_pixel(wx, wy);
    default: {
        uint8_t p = ground_pixel(wx, wy, k);
        if (gkp(wx - 5, wy - 5) == T_BUILDING && SHADE_OF[p]) p = SHADE_OF[p];
        return p;
    }
    }
}

// Atlas: duplicate search via hash table
#define TEXH 4096
static int16_t *tex_ht;
static int tex_overflow = 0;
static int add_texture(const uint8_t *t, int kind)
{
    uint32_t h = 2166136261u ^ (uint32_t)kind;
    for (int i = 0; i < TILE * TILE; i++) h = (h ^ t[i]) * 16777619u;
    int s = (int)(h & (TEXH - 1));
    while (tex_ht[s]) {
        int i = tex_ht[s] - 1;
        if (tex_hash[i] == h && tex_kind[i] == kind && memcmp(tex_ptr[i], t, TILE * TILE) == 0) return i;
        s = (s + 1) & (TEXH - 1);
    }
    if (ntex >= MAX_TEX) return -1;
    memcpy(tex_w(ntex), t, TILE * TILE);
    tex_hash[ntex] = h;
    tex_kind[ntex] = (uint8_t)kind;
    tex_ht[s] = (int16_t)(ntex + 1);
    return ntex++;
}

// ---- Points of interest: shops and casino ----
enum { POI_GUNS = 0, POI_FOOD, POI_CARS, POI_CASINO, POI_BANK, NPOI };
static const char *POI_NAME[NPOI] = { "GUN SHOP", "CHIKKO", "AUTO SHOP", "CASINO", "BANK" };
static const uint8_t POI_PAL[NPOI] = { 15, 14, 6, 13, 12 };
static const uint8_t POI_RGB[NPOI][3] = { {150, 170, 90}, {240, 60, 50}, {90, 160, 255}, {220, 90, 255}, {80, 220, 140} };
typedef struct { float dx, dy; int lot; } poi_t;
static poi_t pois[NPOI];
static inline uint16_t poi_color(int k) { return rgb565(POI_RGB[k][0], POI_RGB[k][1], POI_RGB[k][2]); }

static void place_pois(void)
{
    for (int k = 0; k < NPOI; k++) {
        int best = 0;
        float bd = 1e9f;
        for (int i = 1; i <= g_nlots; i++) {
            const lot_t *l = &g_lots[i];
            if (l->style == ST_INDUSTRY) continue;
            float cx = (l->x0 + l->x1) * 0.5f, cy = (l->y0 + l->y1) * 0.5f;
            float d = (cx - LM_AT[LM_POI_GUNS + k][0]) * (cx - LM_AT[LM_POI_GUNS + k][0]) + (cy - LM_AT[LM_POI_GUNS + k][1]) * (cy - LM_AT[LM_POI_GUNS + k][1]);
            int area = (l->x1 - l->x0 + 1) * (l->y1 - l->y0 + 1);
            if (area < 4) d += 50;
            if (d < bd) { bd = d; best = i; }
        }
        lot_t *l = &g_lots[best];
        l->pal = POI_PAL[k];
        l->style = ST_FLAT;
        l->h10 = (k == POI_CASINO) ? 58 : (k == POI_BANK) ? 40 : (k == POI_FOOD) ? 12 : 18;
        pois[k].lot = best;
        // entrance — the sidewalk nearest to the building center
        int cx = (l->x0 + l->x1) / 2, cy = (l->y0 + l->y1) / 2, dxt = cx, dyt = cy;
        for (int r = 1; r < 12; r++) {
            bool found = false;
            for (int dy = -r; dy <= r && !found; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    if (gk(cx + dx, cy + dy) == T_SIDEWALK) { dxt = cx + dx; dyt = cy + dy; found = true; break; }
                }
            if (found) break;
        }
        pois[k].dx = (dxt & (MAP - 1)) * 16 + 8;
        pois[k].dy = (dyt & (MAP - 1)) * 16 + 8;
    }
}

static int *tree_row, *lamp_row, *xing_row;     // [MAP + 1] first object of a tile row (objects are sorted by y)
static int *lot_row;                  // [MAP + 1] first lot with y0 >= row (lots are sorted by y0)
static void build_lot_index(void)
{
    int k = 1;
    for (int r = 0; r <= MAP; r++) {
        while (k <= g_nlots && g_lots[k].y0 < r) k++;
        lot_row[r] = k;
    }
}

static void world_alloc(void)
{
    if (!map_v) {
        map_v = malloc(MAP * MAP * sizeof(uint16_t));
        lanes = malloc(MAP * MAP);
        navd = malloc(NAVW * NAVW);
        navq = malloc(NAVW * NAVW * sizeof(uint16_t));
        navf = malloc(NAVW * NAVW);
        tex_psram = malloc((MAX_TEX - TEX_SRAM) * TILE * TILE);
        g_lots = calloc(MAX_LOTS, sizeof(lot_t));
        for (int i = 0; i < MAX_TEX; i++) tex_ptr[i] = i < TEX_SRAM ? tex_sram[i] : tex_psram + (size_t)(i - TEX_SRAM) * TILE * TILE;
    }
    if (!lot_row) {
        lot_row = malloc((MAP + 1) * sizeof(int)); tree_row = malloc((MAP + 1) * sizeof(int));
        lamp_row = malloc((MAP + 1) * sizeof(int)); xing_row = malloc((MAP + 1) * sizeof(int));
        zgang = calloc(ZG_BYTES + 16, 1);
    }
    if (!map_v || !lanes || !navd || !navq || !navf || !tex_psram || !g_lots || !lot_row || !tree_row || !lamp_row || !xing_row || !zgang) { printf("world: out of memory\n"); abort(); }
    shore_alloc();
}

// lots in ascending y0 (for fast lookup of visible buildings), numbers in g_lot are renumbered
static int lot_cmp(const void *a, const void *b)
{
    const lot_t *p = a, *q = b;
    if (p->y0 != q->y0) return (int)p->y0 - (int)q->y0;
    return (int)p->x0 - (int)q->x0;
}
static void sort_lots(void)
{
    // can't store the old number in the pad field (8 bits) — sort (lot, old number) pairs
    typedef struct { lot_t l; uint16_t id; } lid_t;
    lid_t *t = malloc((g_nlots + 1) * sizeof(lid_t));
    for (int i = 1; i <= g_nlots; i++) { t[i - 1].l = g_lots[i]; t[i - 1].id = (uint16_t)i; }
    qsort(t, g_nlots, sizeof(lid_t), lot_cmp);         // lot_t is the first field of lid_t
    uint16_t *remap = malloc((g_nlots + 1) * sizeof(uint16_t));
    remap[0] = 0;
    for (int i = 0; i < g_nlots; i++) { g_lots[i + 1] = t[i].l; remap[t[i].id] = (uint16_t)(i + 1); }
    for (int i = 0; i < MAP * MAP; i++) if (g_lot[i]) g_lot[i] = remap[g_lot[i]];
    free(remap); free(t);
}

static __attribute__((unused)) void generate_city(void)
{
    const int N = MAP * MAP;
    world_alloc();
    g_kind = malloc(N); g_lot = calloc(N, sizeof(uint16_t));
    g_sea = calloc(N, 1); g_rock = calloc(N, 1);
    g_use = calloc(N, 1); g_rt = calloc(N, 1); g_dist = calloc(N, 1);
    g_q = malloc((size_t)N * sizeof(uint32_t));
    g_hr = malloc(MAX_ROADS * sizeof(road_t)); g_vr = malloc(MAX_ROADS * sizeof(road_t));
    tex_ht = calloc(TEXH, sizeof(int16_t));
    tex_hash = malloc(MAX_TEX * sizeof(uint32_t));
    if (!g_kind || !g_lot || !g_sea || !g_rock || !g_use || !g_rt || !g_dist || !g_q || !g_hr || !g_vr || !tex_ht) { printf("city: out of memory\n"); abort(); }
    uint32_t keep_rng = rng_state;
    rng_state = WORLD_SEED;                 // the world is the same for any game seed

    make_layout();
    sort_lots();
    place_pois();
    gen_props();
    build_shore();          // sea depth and shore bitmaps (after make_layout and gen_props)

    uint8_t tmp[TILE * TILE];
    ntex = 0;
    tex_overflow = 0;
    int pal_max = 0;
    for (int ty = 0; ty < MAP; ty++) {
        for (int tx = 0; tx < MAP; tx++) {
            for (int ly = 0; ly < TILE; ly++)
                for (int lx = 0; lx < TILE; lx++)
                    tmp[ly * TILE + lx] = ref_pixel(tx * TILE + lx, ty * TILE + ly);
            int k = gk(tx, ty);
            int id = add_texture(tmp, k);
            _Static_assert(NPAL <= 64, "palette rows must fit 6 bits");
            if (id < 0) {           // atlas overflow: take any texture of the same type
                tex_overflow++;
                id = 0;
                for (int i = 0; i < ntex; i++) if (tex_kind[i] == k) { id = i; break; }
            }
            int p = (k == T_BUILDING) ? g_lots[g_lot[MI(tx, ty)]].pal : (k == T_WATER) ? g_wrow[MI(tx, ty)] : tone_row(tile_tone(tx, ty));
            if (k == T_WATER && !p) p = 0;
            if (p > pal_max) pal_max = p;
            map_v[MI(tx, ty)] = (uint16_t)((p << 10) | id);
        }
    }
    // most frequent textures first (into internal SRAM)
    {
        uint32_t *cnt = calloc(MAX_TEX, sizeof(uint32_t));
        int ord[MAX_TEX], inv[MAX_TEX];
        for (int i = 0; i < N; i++) cnt[MV_TEX(map_v[i])]++;
        for (int i = 0; i < ntex; i++) ord[i] = i;
        for (int i = 1; i < ntex; i++) {                 // by insertion: ntex <= 512
            int v = ord[i], j = i - 1;
            while (j >= 0 && cnt[ord[j]] < cnt[v]) { ord[j + 1] = ord[j]; j--; }
            ord[j + 1] = v;
        }
        uint8_t *buf = malloc((size_t)ntex * TILE * TILE), kk[MAX_TEX];
        for (int i = 0; i < ntex; i++) { memcpy(buf + (size_t)i * 256, tex_ptr[ord[i]], 256); kk[i] = tex_kind[ord[i]]; inv[ord[i]] = i; }
        for (int i = 0; i < ntex; i++) { memcpy(tex_w(i), buf + (size_t)i * 256, 256); tex_kind[i] = kk[i]; }
        for (int i = 0; i < N; i++) { uint16_t v = map_v[i]; map_v[i] = (uint16_t)((v & ~1023) | inv[MV_TEX(v)]); }
        uint32_t sram_use = 0, all = 0;
        for (int i = 0; i < ntex; i++) { all += cnt[ord[i]]; if (i < TEX_SRAM) sram_use += cnt[ord[i]]; }
        printf("atlas: %d textures, %.1f%% of tiles use SRAM part\n", ntex, all ? 100.0 * sram_use / all : 0.0);
        free(buf); free(cnt);
    }
    printf("city: %d lots, %d unique tile textures (%d bytes), overflow %d, palette rows used %d/%d\n",
           g_nlots, ntex, ntex * TILE * TILE, tex_overflow, pal_max + 1, NPAL);
    free(g_sea); free(g_rock); free(g_wrow); g_sea = g_rock = g_wrow = NULL;
    free(g_kind); free(g_lot); g_kind = NULL; g_lot = NULL;          // keep the lots (g_lots) — volumetric buildings are drawn from them
    free(g_use); free(g_rt); free(g_dist); free(g_q); free(g_hr); free(g_vr); free(tex_ht); free(tex_hash); tex_hash = NULL;
    g_use = g_rt = g_dist = NULL; g_q = NULL; g_hr = g_vr = NULL; tex_ht = NULL;
    build_lot_index();
    rng_state = keep_rng;
}

static inline int tile_kind(int tx, int ty) { return tex_kind[MV_TEX(map_v[MI(tx, ty)])]; }
static inline int tile_at(float x, float y)
{
    int ix = ((int)floorf(x)) & WMASK, iy = ((int)floorf(y)) & WMASK;
    return tile_kind(ix >> 4, iy >> 4);
}
// solid — can't walk/drive through (buildings and water); wall — stops bullets and line of sight (buildings only)
static inline bool solid_at(float x, float y) { int k = tile_at(x, y); return k == T_BUILDING || k == T_WATER; }
static inline bool wall_at(float x, float y) { return tile_at(x, y) == T_BUILDING; }

// Are there buildings on the segment (rough visibility check)
static bool line_clear(float x0, float y0, float x1, float y1)
{
    float dx = wrapd(x1 - x0), dy = wrapd(y1 - y0);
    float d = sqrtf(dx * dx + dy * dy);
    int n = (int)(d / 8) + 1;
    for (int i = 1; i < n; i++) {
        float k = (float)i / n;
        if (wall_at(x0 + dx * k, y0 + dy * k)) return false;
    }
    return true;
}

// ================= Pathfinding (BFS from the player) =================
static int nav_sx[2], nav_sy[2];       // BFS source for navd / navf (256x256 window around it)
static inline int navget(const uint8_t *f, int tx, int ty)
{
    int k = (f == navf);
    int dx = ((tx - nav_sx[k] + MAP / 2) & (MAP - 1)) - MAP / 2, dy = ((ty - nav_sy[k] + MAP / 2) & (MAP - 1)) - MAP / 2;
    if (dx > 110 || dx < -110 || dy > 110 || dy < -110) return 255;
    return f[NI(tx, ty)];
}
static void bfs_from(uint8_t *navd, float px, float py)
{
    memset(navd, 255, NAVW * NAVW);
    int sx = ((int)px & WMASK) >> 4, sy = ((int)py & WMASK) >> 4;
    int k = (navd == navf);
    nav_sx[k] = sx; nav_sy[k] = sy;
    int head = 0, tail = 0;
    navd[NI(sx, sy)] = 0;
    navq[tail++] = (uint16_t)NI(sx, sy);
    while (head < tail) {
        int i = navq[head++];
        int d = navd[i];
        if (d >= 90) continue;                 // police need no path beyond 90 tiles (256 window — no overlaps)
        int x = sx + ((((i & (NAVW - 1)) - sx) + NAVW / 2) & (NAVW - 1)) - NAVW / 2;
        int y = sy + ((((i >> 8) - sy) + NAVW / 2) & (NAVW - 1)) - NAVW / 2;
        static const int8_t ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
        for (int q = 0; q < 4; q++) {
            int nx = x + ox[q], ny = y + oy[q], j = NI(nx, ny);
            if (navd[j] != 255) continue;
            int kk = tile_kind(nx & (MAP - 1), ny & (MAP - 1));
            if (kk == T_BUILDING || kk == T_WATER) continue;
            navd[j] = (uint8_t)(d + 1);
            navq[tail++] = (uint16_t)j;
        }
    }
}

static void nav_update(float px, float py) { bfs_from(navd, px, py); }

// Next path point: steps steps along decreasing distance in the navd field. false — no path.
static bool nav_next_f(const uint8_t *navd, float x, float y, int steps, bool prefer_road, float *ox, float *oy)
{
    int tx = ((int)x & WMASK) >> 4, ty = ((int)y & WMASK) >> 4;
    if (navget(navd, tx, ty) == 255) return false;
    static const int8_t dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int s = 0; s < steps; s++) {
        int best = -1, bd = navget(navd, tx, ty), broad = 0;
        for (int k = 0; k < 4; k++) {
            int nx = tx + dx4[k], ny = ty + dy4[k];
            int d = navget(navd, nx, ny);
            int road = prefer_road && tile_kind(nx, ny) == T_ROAD;
            if (d < bd || (d == bd && best >= 0 && road > broad)) { bd = d; best = k; broad = road; }
        }
        if (best < 0) break;
        tx += dx4[best]; ty += dy4[best];
    }
    *ox = tx * 16 + 8;
    *oy = ty * 16 + 8;
    return true;
}
static bool nav_next(float x, float y, int steps, bool prefer_road, float *ox, float *oy)
{
    return nav_next_f(navd, x, y, steps, prefer_road, ox, oy);
}

// ================= Entities =================
enum { CAR_AI = 0, CAR_PARKED, CAR_PLAYER, CAR_COP, CAR_WRECK, CAR_OFF, CAR_FIRE };
typedef struct {
    float x, y, ang, speed, blocked_t, burn_t, stuck_t, rev_t, timer;
    int16_t hp;
    uint8_t mode, dir, police, crew, hit_by_player, flat;   // flat — tires shot out (spikes)
    uint8_t col;            // color: 0..7 regular, 8 police, 9 gold (mission), 10 fire truck, 11 VORTEX GT, 12 sports car
} car_t;

enum { A_NONE = 0, A_CIV, A_COP, A_GANG };
// person flags
#define AF_TARGET 1     // contract target
#define AF_ROBBER 2     // robber
#define AF_VICTIM 4     // robbery victim
#define AF_DUTY   8     // cop chasing the robber
#define AF_CUFFED 16    // robber in handcuffs
#define AF_SWAT   32    // SWAT (5+ stars)
#define AF_FARE   64    // hailing a taxi
#define AF_BOSS   128   // gang boss (story)
enum { S_WALK = 0, S_FLEE, S_CHASE, S_DEAD, S_IDLE };
typedef struct {
    float x, y, ang, timer, cd;
    int16_t hp;
    uint8_t kind, state, dir, flags;     // flags — AF_*
    uint8_t gang;                        // 1 green, 2 purple
    uint16_t color;
} actor_t;

typedef struct { float x, y, vx, vy, life; int8_t dmg; uint8_t owner; int8_t src; } bullet_t;   // owner: 0 player, 1 police, 2 gangs; src — shooter
typedef struct { float x, y, t; uint8_t kind; } fx_t;                                // kind: 0 explosion, 1 spark
enum { PK_PISTOL = 0, PK_UZI, PK_SHOTGUN, PK_HEALTH, PK_COUNT, PK_FRENZY = PK_COUNT, PK_PACKAGE };
typedef struct { float x, y, timer; uint8_t kind, active; } pickup_t;

typedef struct { const char *name; float cd, speed, spread; int8_t dmg, pellets; int16_t give; } weapon_t;
enum { W_FIST = 0, W_PISTOL, W_UZI, W_SHOTGUN, W_COUNT };
static const weapon_t WEAP[W_COUNT] = {
    { "FIST",    0.35f, 0,    0,     50, 1, 0 },
    { "PISTOL",  0.28f, 400,  0.03f, 34, 1, 36 },
    { "UZI",     0.08f, 430,  0.10f, 18, 1, 120 },
    { "SHOTGUN", 0.75f, 380,  0.28f, 26, 5, 16 },
};

static const float DXv[4] = {1, 0, -1, 0};
static const float DYv[4] = {0, 1, 0, -1};

static car_t cars[NCARS];
static actor_t act[NACT];
static bullet_t bul[NBUL];
static fx_t fx[NFX];
static pickup_t pick[NPICK];

// prototypes for the "living city" (implementation below, next to the renderer)
static bool must_stop_at_light(const car_t *c);
static void add_decal(float x, float y, int type);
static void emit(float x, float y, float vx, float vy, float life, int type);
static void emit_burst(float x, float y, int n, float speed, float life, int type);
static void update_world_extras(float dt);
static void init_boats(void);
static bool on_screen(float x, float y, float margin);
static void reset_police(void);
// events: gangs, robberies, fires
static float gang_hate[2] = { 0, 0 };          // > 0 — the gang hunts the player
enum { RB_NONE = 0, RB_APPROACH, RB_ROB, RB_FLEE, RB_CUFFED };
static int rob_state = RB_NONE, rob_robber = -1, rob_victim = -1, rob_cop = -1;
static float rob_t = 0;
static int fire_stage = 0, fire_lot = 0;
static float fire_x, fire_y, fire_rx, fire_ry, fire_hp = 0, fire_t = 0;

// time of day and weather
#define DAY_LEN 300.0f                 // game day — 5 minutes
static float day_t = DAY_LEN * 10 / 24; // start at 10:00
static bool raining = false;
static float weather_t = 70;            // until the weather changes
static float tint_timer = 0;

static float game_hour(void) { return day_t / DAY_LEN * 24.0f; }
static void find_curb(int tx, int ty, float *px, float *py, float *cx, float *cy, int *cdir);
static bool find_near(int tx, int ty, int kind, int *ox, int *oy);

// Player
static float plx, ply, pang = 0;
static int php = 100;
static int pcar = -1;
static int weapon = W_PISTOL;
static int ammo[W_COUNT];
static float fire_cd = 0, flash_t = 0, hurt_t = 0, invuln_t = 0;
static bool p_moving = false;
static int armor = 0;
// Arrest builds up rather than triggering on a single touch: keep your distance — the meter decays
#define ARREST_TIME     1.6f    // seconds a cop must "hold" the player
#define COP_EXIT_DELAY  1.0f    // cop stands still after exiting the car and can't arrest
#define COP_STAGGER     0.6f    // cop "staggers" after being hit/wounded
static float arrest_t = 0;
static bool arrest_touch = false, arrest_car = false;
static bool can_act = false;

// Wanted level
static float heat = 0, lose_t = 0, cop_spawn_t = 0, minor_cd = 0;
static int stars = 0;
static const float STAR_T[7] = { 0, 0.9f, 3.0f, 6.0f, 10.0f, 15.0f, 21.0f };

// Game state
enum { G_TITLE = 0, G_PLAY, G_PAUSE, G_WASTED, G_BUSTED };
static int gstate = G_TITLE;
static float state_t = 0, g_time = 0;
static uint16_t prev_held = 0, pressed = 0;
static game_input_t cur_in;

static int32_t money = 0, kills = 0, deliveries = 0, best_wanted = 0;
static bool save_req = false, shot_req = false;
static bool st_pad = false, st_sd = false;
static char st_pad_name[24] = "";

static int stage = 0, reward = 0;
// Missions: delivery, contract (kill a man in black), steal the gold car
enum { M_DELIVER = 0, M_HIT, M_STEAL, M_COUNT, M_ESCAPE = M_COUNT, M_RACE, M_BANK, M_BOSS, M_GANGWAR };
static int mtype = M_DELIVER, mtarget = -1;
// Pay'n'Spray, "massacre", packages
static float spray_x, spray_y, spray_cd = 0;
static float frenzy_t = 0;
static int frenzy_kills = 0, packages = 0;
#define NPACKAGES 10
static float mis_x, mis_y;

static char msg[40] = "";
static float msg_t = 0;

static float camX, camY, lead_x = 0, lead_y = 0;
static int cam_ix, cam_iy;

// ---- extensions (game_plus.inc) ----
static void plus_init(void);
static void plus_load(int32_t flags);
static int32_t plus_save_flags(void);
static void plus_update(float dt);
// wildlife (game_fauna.inc)
static bool goosed = false;               // player was pecked to death by geese — print GOOSED instead of WASTED
static void fauna_update(float dt);
static void fauna_draw(void);
static void fauna_draw_air(void);
static void fauna_noise(float x, float y, float r, bool by_player);
static bool fauna_bullet(float x, float y, bool by_player);
static void fauna_explode(float x, float y, bool by_player);
static void fauna_reset(void);
static void plus_record(float dt);
static void plus_on_respawn(void);
static void plus_on_kill(int i, bool by_player);
static void plus_dead_tick(actor_t *a, float dt);
static void plus_shake(float a);
static void plus_mission_start(int type);
static void plus_mission_update(void);
static const char *plus_mission_text(void);
static int plus_mission_done(int pay);
static void heist_start(void);
static void plus_radio_next(void);
static void replay_tick(float dt);
static void replay_start(void);
static void plus_pause_input(void);
static void plus_draw_lights(int k);
static bool plus_is_roadblock(int i);
static bool plus_heist_busy(void);
static void plus_map_markers(int x0, int y0, int w, int h, float cwx, float cwy, int num, int den);
static void plus_draw_pause(int rx, int ry);
static bool replay, cinema;
static bool in_heli;                    // player is at the helicopter controls
#define ON_FOOT (pcar < 0 && !in_heli)
static void plus_heli_control(const game_input_t *in, float dt);
static bool plus_heli_action(void);
static void plus_heli_fire(void);
static void plus_heli_damage(int dmg);
static void plus_cam_lead(float *tlx, float *tly);
static float knock_ang, knock_sp;

void game_message(const char *text, float seconds)
{
    strncpy(msg, text, sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = 0;
    msg_t = seconds;
}

// ================= Wanted level =================
static int stars_for(float h)
{
    int s = 0;
    for (int i = 1; i <= 6; i++) if (h >= STAR_T[i]) s = i;
    return s;
}

static void add_heat(float h)
{
    if (h <= 0) return;
    heat += h;
    if (heat > 26) heat = 26;
    lose_t = 0;
    int ns = stars_for(heat);
    if (ns > stars) {
        if (ns > best_wanted) { best_wanted = ns; save_req = true; }
    }
    stars = ns;
}

// minor offenses: at most once per 0.8 s, so a queue/series of hits doesn't instantly give stars
static void add_heat_minor(float h)
{
    if (minor_cd > 0) return;
    minor_cd = 0.8f;
    add_heat(h);
}

static bool cop_near(float x, float y, float r)
{
    float r2 = r * r;
    for (int i = 0; i < NACT; i++)
        if (act[i].kind == A_COP && act[i].state != S_DEAD && dist2w(x, y, act[i].x, act[i].y) < r2) return true;
    for (int i = NCIVCARS; i < POLICE_END; i++)
        if (cars[i].mode == CAR_COP && dist2w(x, y, cars[i].x, cars[i].y) < r2) return true;
    return false;
}

// Do the police see the crime (a patrol counts too)
static bool police_witness(float x, float y, float r)
{
    if (cop_near(x, y, r)) return true;
    for (int i = NCIVCARS; i < POLICE_END; i++)
        if (cars[i].mode == CAR_AI && dist2w(x, y, cars[i].x, cars[i].y) < r * r) return true;
    return false;
}

// ================= Spawn points =================
static void random_tile_point(int type, float *x, float *y)
{
    // random point in one of the cities (70% old city A, 30% port city B)
    for (int k = 0; k < 4000; k++) {
        bool a = (rnd() % 10) < 7;
        int tx = a ? CA_X0 + rnd() % (CA_X1 - CA_X0) : CB_X0 + rnd() % (CB_X1 - CB_X0);
        int ty = a ? CA_Y0 + rnd() % (CA_Y1 - CA_Y0) : CB_Y0 + rnd() % (CB_Y1 - CB_Y0);
        if (tile_kind(tx, ty) == type) {
            *x = tx * 16 + 8;
            *y = ty * 16 + 8;
            return;
        }
    }
    *x = LM_AT[LM_START][0] * 16 + 8; *y = LM_AT[LM_START][1] * 16 + 8;
}

static bool on_screen(float x, float y, float margin)
{
    return fabsf(wrapd(x - camX)) < SW / 2 + margin && fabsf(wrapd(y - camY)) < SH / 2 + margin;
}

static void offscreen_point(int type, float dmin, float dmax, float *x, float *y)
{
    for (int k = 0; k < 300; k++) {
        float a = frnd() * 2 * PIf, d = dmin + frnd() * (dmax - dmin);
        float px = wrapf(plx + cosf(a) * d), py = wrapf(ply + sinf(a) * d);
        int tx = ((int)px) >> 4, ty = ((int)py) >> 4;
        if (tile_kind(tx, ty) == type && !on_screen(px, py, 20)) {
            *x = tx * 16 + 8; *y = ty * 16 + 8;
            return;
        }
    }
    random_tile_point(type, x, y);
}

// ================= Cars =================
static int lane_dir(int f)
{
    switch (f) {
    case LF_E: return 0;
    case LF_S: return 1;
    case LF_W: return 2;
    case LF_N: return 3;
    }
    return -1;    // intersection or not a road
}

static void car_reset(car_t *c)
{
    c->flat = 0;
    c->speed = AI_SPEED;
    c->mode = CAR_AI;
    c->blocked_t = c->burn_t = c->stuck_t = c->rev_t = c->timer = 0;
    c->hp = c->police ? 140 : 100;
    c->crew = c->police ? 2 : 0;
    c->hit_by_player = 0;
}

// Place a car on a lane (not on an intersection). near — in a 180..480 px ring around the player.
static void spawn_ai_at(car_t *c, bool near)
{
    for (int k = 0; k < 400; k++) {
        int tx, ty;
        if (near) {
            float a = frnd() * 2 * PIf, d = 180 + frnd() * 300;
            tx = (((int)(plx + cosf(a) * d)) & WMASK) >> 4;
            ty = (((int)(ply + sinf(a) * d)) & WMASK) >> 4;
        } else {
            tx = rnd() % MAP; ty = rnd() % MAP;
        }
        int dir = lane_dir(lanes[MI(tx, ty)]);
        if (dir < 0) continue;
        float x = tx * 16 + 8, y = ty * 16 + 8;
        if (near && on_screen(x, y, 40)) continue;
        bool busy = false;
        for (int j = 0; j < NCARS; j++)
            if (&cars[j] != c && dist2w(x, y, cars[j].x, cars[j].y) < 30 * 30) { busy = true; break; }
        if (busy) continue;
        c->x = x; c->y = y;
        c->dir = dir;
        c->ang = dir * PIf / 2;
        car_reset(c);
        return;
    }
    car_reset(c);
}

static void spawn_ai(car_t *c) { spawn_ai_at(c, false); }
static void respawn_offscreen(car_t *c) { spawn_ai_at(c, true); }

static bool check_ahead(const car_t *c, float ox, float oy)
{
    float dvx = DXv[c->dir], dvy = DYv[c->dir];
    float dx = wrapd(ox - c->x), dy = wrapd(oy - c->y);
    float along = dx * dvx + dy * dvy;
    float lat = fabsf(dx * dvy - dy * dvx);
    return along > 2 && along < 28 && lat < 10;
}

// Decision at tile center: where to drive next along the lanes
static void ai_choose(car_t *c, int tx, int ty)
{
    static const int8_t dx4[4] = {1, 0, -1, 0}, dy4[4] = {0, 1, 0, -1};
    int f = lanes[MI(tx, ty)];
    int cand[4], n = 0;
    bool straight = false;
    for (int d = 0; d < 4; d++) {
        if (!(f & (1 << d)) || d == ((c->dir + 2) & 3)) continue;
        if (!(lanes[MI(tx + dx4[d], ty + dy4[d])] & (1 << d))) continue;
        cand[n++] = d;
        if (d == c->dir) straight = true;
    }
    if (n == 0) {
        // dead end: U-turn onto the opposite lane (it's on the left in the direction of travel)
        int ld = (c->dir + 3) & 3;
        c->x = wrapf(tx * 16 + 8 + dx4[ld] * 16);
        c->y = wrapf(ty * 16 + 8 + dy4[ld] * 16);
        c->dir = (c->dir + 2) & 3;
        return;
    }
    int nd = c->dir;
    if (!straight || (n > 1 && rnd() % 3 == 0)) nd = cand[rnd() % n];
    if (nd != c->dir) {
        c->x = tx * 16 + 8;
        c->y = ty * 16 + 8;
        c->dir = nd;
    }
}

static void update_ai(int i, float dt)
{
    car_t *c = &cars[i];
    bool blocked = false;
    for (int j = 0; j < NCARS && !blocked; j++)
        if (j != i && check_ahead(c, cars[j].x, cars[j].y)) blocked = true;
    if (ON_FOOT && check_ahead(c, plx, ply)) blocked = true;
    for (int j = 0; j < NACT && !blocked; j++)
        if (act[j].kind && act[j].state != S_DEAD && check_ahead(c, act[j].x, act[j].y)) blocked = true;

    if (blocked) {
        c->blocked_t += dt;
        if (c->blocked_t < 12.0f) return;         // long traffic jam — drive "through"
    } else {
        c->blocked_t = 0;
    }
    if (must_stop_at_light(c)) { c->speed = 0; return; }   // red / yellow

    float step = AI_SPEED * dt;
    // did we cross the tile center along the axis of motion
    int dir0 = c->dir;
    float p0 = (dir0 & 1) ? c->y : c->x;
    float p1 = p0 + ((dir0 == 0 || dir0 == 1) ? step : -step);
    int t0 = (int)floorf((p0 - 8) / 16.0f), t1 = (int)floorf((p1 - 8) / 16.0f);
    if (dir0 == 0 || dir0 == 1) {
        if (dir0 & 1) c->y = p1; else c->x = p1;
        if (t1 > t0) {
            int tc = t1;
            int tx = (dir0 & 1) ? (((int)c->x) >> 4) : tc, ty = (dir0 & 1) ? tc : (((int)c->y) >> 4);
            ai_choose(c, tx & (MAP - 1), ty & (MAP - 1));
        }
    } else {
        if (dir0 & 1) c->y = p1; else c->x = p1;
        int c0 = (int)ceilf((p0 - 8) / 16.0f), c1 = (int)ceilf((p1 - 8) / 16.0f);
        if (c1 < c0) {
            int tc = c1;
            int tx = (dir0 & 1) ? (((int)c->x) >> 4) : tc, ty = (dir0 & 1) ? tc : (((int)c->y) >> 4);
            ai_choose(c, tx & (MAP - 1), ty & (MAP - 1));
        }
    }
    (void)t0;
    c->x = wrapf(c->x);
    c->y = wrapf(c->y);
    c->ang = c->dir * PIf / 2;
    c->speed = AI_SPEED;
    // far from the player — move it closer so the city around stays alive
    if (dist2w(c->x, c->y, plx, ply) > 700.0f * 700.0f) respawn_offscreen(c);
}

// Collision of car self at position (x, y): -2 — wall, car index or -1
static int car_hits(int self, float x, float y, float ang)
{
    float ca = cosf(ang), sa = sinf(ang);
    static const float pts[4][2] = { {11, 5}, {11, -5}, {-11, 5}, {-11, -5} };
    for (int i = 0; i < 4; i++) {
        float wx = x + pts[i][0] * ca - pts[i][1] * sa;
        float wy = y + pts[i][0] * sa + pts[i][1] * ca;
        if (solid_at(wx, wy)) return -2;
    }
    for (int j = 0; j < NCARS; j++) {
        if (j == self) continue;
        float d2 = dist2w(x, y, cars[j].x, cars[j].y);
        float o2 = dist2w(cars[self].x, cars[self].y, cars[j].x, cars[j].y);
        if (d2 < 18 * 18 && d2 < o2) return j;
    }
    return -1;
}

static void damage_car(int i, int dmg, bool by_player)
{
    car_t *c = &cars[i];
    if (c->mode == CAR_WRECK || c->hp <= 0) return;
    c->hp -= dmg;
    if (by_player) {
        c->hit_by_player = 1;
        if (c->police) add_heat_minor(0.3f);
    }
    if (c->hp <= 0) {
        c->hp = 0;
        c->burn_t = 2.5f;
        if (i == pcar) game_message("GET OUT!", 1.5f);
    }
}

// Physics of a "free" car (player and police)
static void drive_free(int i, float target_ang, float target_speed, float steer_k, float dt)
{
    car_t *c = &cars[i];
    float diff = angle_diff(target_ang, c->ang);
    float k = fabsf(c->speed) / 70.0f;
    if (k > 1) k = 1;
    if (k < steer_k) k = steer_k;
    float turn = CAR_TURN * dt * k;
    if (c->speed < 0) diff = -diff;
    if (diff > turn) diff = turn;
    if (diff < -turn) diff = -turn;
    c->ang = angle_diff(c->ang + diff, 0);

    float ds = target_speed - c->speed;
    bool accelerating = (c->speed >= 0) ? (ds > 0) : (ds < 0);
    float accm = (c->col == 11) ? 1.8f : (c->col == 12) ? 1.35f : 1.0f;
    float lim = (accelerating ? CAR_ACC * accm : CAR_BRAKE) * dt;
    if (ds > lim) ds = lim;
    if (ds < -lim) ds = -lim;
    c->speed += ds;

    float nx = c->x + cosf(c->ang) * c->speed * dt;
    float ny = c->y + sinf(c->ang) * c->speed * dt;
    int h = car_hits(i, nx, ny, c->ang);
    if (h != -1) {
        float imp = fabsf(c->speed);
        if (imp > 70) {
            bool byp = (i == pcar);
            damage_car(i, (int)(imp * 0.06f), false);
            if (h >= 0) {
                damage_car(h, (int)(imp * 0.08f), byp);
                // push
                cars[h].x = wrapf(cars[h].x + cosf(c->ang) * 3);
                cars[h].y = wrapf(cars[h].y + sinf(c->ang) * 3);
                if (cars[h].mode == CAR_AI) cars[h].blocked_t = 0;
            }
        }
        c->speed *= -0.3f;
        c->stuck_t += dt;
    } else {
        c->x = wrapf(nx);
        c->y = wrapf(ny);
        c->stuck_t = 0;
    }
}

static void update_player_car(const game_input_t *in, float dt)
{
    // controls relative to the car: left/right — steering, up — gas, down — brake/reverse
    car_t *c = &cars[pcar];
    float steer = in->mx, gas = -in->my;
    if (fabsf(steer) < 0.12f) steer = 0;
    if (c->hp <= 0) gas = 0;
    float vmax = CAR_MAX * ((c->col == 11) ? 1.45f : (c->col == 12) ? 1.2f : 1.0f);
    float target_speed = 0;
    if (gas > 0.05f) target_speed = vmax * gas;
    else if (gas < -0.05f) target_speed = (c->speed > 20) ? 0 : CAR_MAX * 0.35f * gas;
    if (c->speed > 20 && gas < -0.05f) target_speed = -1;       // braking
    float av = fabsf(c->speed);
    float vk = av / 40.0f; if (vk > 1) vk = 1;
    float hi = av / vmax; if (hi > 1) hi = 1;
    float rate = CAR_TURN * 0.85f * vk * (1.0f - 0.45f * hi);   // at speed the steering is "heavier"
    if (c->speed < 0) rate = -rate;
    c->ang = angle_diff(c->ang + steer * rate * dt, 0);
    float target = c->ang;
    if (c->flat) target_speed *= 0.5f;                  // tires shot out
    if (in->held & BTN_RUN) target_speed = 0;           // handbrake
    float a0 = c->ang, s0 = c->speed;
    drive_free(pcar, target, target_speed, 0.0f, dt);
    if (fabsf(s0) - fabsf(c->speed) > 70) plus_shake(fabsf(s0) / 45);      // hit a wall/car
    // tire marks: sharp turn at speed, handbrake or full braking
    float turn = fabsf(angle_diff(c->ang, a0)) / (dt + 1e-4f);
    bool skid = fabsf(c->speed) > 70 && (turn > 2.2f || (in->held & BTN_RUN) || (s0 - c->speed) * s0 > 300 * dt * fabsf(s0) / (fabsf(s0) + 1));
    static float lsx = 0, lsy = 0;
    if (skid && dist2w(c->x, c->y, lsx, lsy) > 9) {
        float ca = cosf(c->ang), sa = sinf(c->ang);
        add_decal(c->x - ca * 8 - sa * 5, c->y - sa * 8 + ca * 5, 0);
        add_decal(c->x - ca * 8 + sa * 5, c->y - sa * 8 - ca * 5, 0);
        lsx = c->x; lsy = c->y;
        if (rnd() % 3 == 0) emit(c->x - ca * 10, c->y - sa * 10, frnd() * 10 - 5, frnd() * 10 - 5, 0.6f, PT_SMOKE);
    }
}

// ================= Pedestrians and police =================
static void spawn_civ(actor_t *a, bool off)
{
    a->kind = A_CIV;
    a->state = S_WALK;
    a->flags = 0;
    a->hp = 40;
    if (off) offscreen_point(T_SIDEWALK, 180, 380, &a->x, &a->y);
    else {
        a->x = a->y = -1;
        for (int k = 0; k < 300; k++) {
            float ang = frnd() * 2 * PIf, d = 30 + frnd() * 300;
            int tx = (((int)(plx + cosf(ang) * d)) & WMASK) >> 4, ty = (((int)(ply + sinf(ang) * d)) & WMASK) >> 4;
            if (tile_kind(tx, ty) == T_SIDEWALK) { a->x = tx * 16 + 8; a->y = ty * 16 + 8; break; }
        }
        if (a->x < 0) random_tile_point(T_SIDEWALK, &a->x, &a->y);
    }
    a->dir = rnd() % 4;
    a->timer = 1;
    a->cd = 0;
    a->ang = a->dir * PIf / 2;
    a->color = rnd() % 6;
}

static int free_actor(void)
{
    for (int i = 0; i < NACT; i++) if (act[i].kind == A_NONE) return i;
    // replace the farthest corpse
    int best = -1; float bd = 0;
    for (int i = 0; i < NACT; i++) {
        if (act[i].state != S_DEAD) continue;
        float d = dist2w(plx, ply, act[i].x, act[i].y);
        if (d > bd) { bd = d; best = i; }
    }
    return best;
}

static int count_kind(int kind, bool alive_only)
{
    int n = 0;
    for (int i = 0; i < NACT; i++)
        if (act[i].kind == kind && (!alive_only || act[i].state != S_DEAD)) n++;
    return n;
}

static int spawn_cop(float x, float y, float delay)
{
    int i = free_actor();
    if (i < 0) return -1;
    actor_t *a = &act[i];
    a->kind = A_COP;
    a->state = S_CHASE;
    a->flags = 0;
    a->hp = 70;
    a->x = x; a->y = y;
    a->timer = delay;           // delay: exiting a car / just spawned
    a->cd = 1.0f + frnd();
    a->ang = 0;
    a->color = 6;
    return i;
}

static void scare_civs(float x, float y, float r)
{
    float r2 = r * r;
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (a->kind == A_CIV && a->state != S_DEAD && dist2w(x, y, a->x, a->y) < r2) {
            a->state = S_FLEE;
            a->timer = 4 + frnd() * 3;
        }
    }
}

static void spawn_fx(float x, float y, int kind)
{
    int best = 0; float bt = -1;
    for (int i = 0; i < NFX; i++) {
        if (fx[i].t <= 0) { best = i; break; }
        if (fx[i].t > bt) { bt = fx[i].t; best = i; }
    }
    fx[best].x = x; fx[best].y = y; fx[best].t = (kind == 0) ? 0.001f : 0.001f; fx[best].kind = kind;
}

static void kill_actor(int i, bool by_player)
{
    actor_t *a = &act[i];
    if (a->state == S_DEAD) return;
    a->state = S_DEAD;
    a->hp = 0;
    a->timer = 15;
    plus_on_kill(i, by_player);
    scare_civs(a->x, a->y, 120);
    if (by_player && (a->flags & AF_ROBBER)) {
        money += 100;
        game_message("HERO! ROBBER DOWN +$100", 2.5f);
        save_req = true;
        return;
    }
    if (by_player && a->kind == A_GANG) {
        kills++;
        money += 15;
        gang_hate[a->gang - 1] = 60;
        add_heat(0.3f);
        if (frenzy_t > 0) { frenzy_kills++; money += 50; }
        save_req = true;
        return;
    }
    if (by_player) {
        kills++;
        money += (a->kind == A_COP) ? 25 : 10;
        if (frenzy_t > 0) { frenzy_kills++; money += 50; }
        add_heat(a->kind == A_COP ? 2.0f : 0.7f);
        save_req = true;
    }
}

static void hurt_player(int dmg)
{
    if (gstate != G_PLAY || invuln_t > 0) return;
    if (armor > 0) { armor -= dmg; dmg = armor < 0 ? -armor : 0; if (armor < 0) armor = 0; }
    php -= dmg;
    hurt_t = 0.25f;
    plus_shake(dmg > 20 ? 4 : 2);
    if (php <= 0) {
        php = 0;
        gstate = G_WASTED;
        state_t = 0;
        game_message("", 0);
    }
}

static void busted(void)
{
    if (gstate != G_PLAY || invuln_t > 0) return;
    arrest_t = 0;
    gstate = G_BUSTED;
    state_t = 0;
}

// Movement with sliding along walls
static void move_slide(float *x, float *y, float vx, float vy, float dt)
{
    float nx = *x + vx * dt;
    if (!solid_at(nx, *y)) *x = wrapf(nx);
    float ny = *y + vy * dt;
    if (!solid_at(*x, ny)) *y = wrapf(ny);
}

static void update_civ(actor_t *a, float dt)
{
    // fleeing from the player's fast car
    if (pcar >= 0 && a->state != S_FLEE) {
        car_t *c = &cars[pcar];
        if (fabsf(c->speed) > 40 && dist2w(a->x, a->y, c->x, c->y) < 45 * 45) {
            a->state = S_FLEE;
            a->timer = 1.5f;
        }
    }
    if (a->state == S_IDLE) {                       // standing, chatting
        a->timer -= dt;
        if (a->timer <= 0) { a->state = S_WALK; a->timer = 0; }
        return;
    }
    if (a->state == S_FLEE) {
        a->timer -= dt;
        if (a->timer <= 0) { a->state = S_WALK; a->timer = 0; return; }
        float dx = wrapd(a->x - plx), dy = wrapd(a->y - ply);
        float d = sqrtf(dx * dx + dy * dy) + 0.01f;
        float vx = dx / d * 78, vy = dy / d * 78;
        // a bit of sideways "panic"
        float wob = sinf(g_time * 5 + a->color) * 30;
        vx += -dy / d * wob; vy += dx / d * wob;
        move_slide(&a->x, &a->y, vx, vy, dt);
        a->ang = atan2f(vy, vx);
        return;
    }
    if (a->kind == A_CIV && rnd() % 500 == 0) { a->state = S_IDLE; a->timer = 2 + frnd() * 5; return; }
    a->timer -= dt;
    float nx = a->x + DXv[a->dir] * PED_SPEED * dt;
    float ny = a->y + DYv[a->dir] * PED_SPEED * dt;
    int here = tile_at(a->x, a->y), next = tile_at(nx, ny);
    bool ok = (next == T_SIDEWALK || next == T_PARK || next == T_PLAZA || (here == T_ROAD && next != T_BUILDING && next != T_WATER));
    if (a->timer <= 0 || !ok) {
        a->dir = rnd() % 4;
        a->timer = 1.0f + (rnd() % 300) / 100.0f;
        return;
    }
    a->x = wrapf(nx);
    a->y = wrapf(ny);
    a->ang = a->dir * PIf / 2;
}

static int bullet_src = -1;
static void fire_bullet(float x, float y, float ang, float speed, int dmg, int owner)
{
    for (int i = 0; i < NBUL; i++) {
        if (bul[i].life > 0) continue;
        bul[i].x = x; bul[i].y = y;
        bul[i].vx = cosf(ang) * speed; bul[i].vy = sinf(ang) * speed;
        bul[i].life = 0.6f;
        bul[i].dmg = dmg;
        bul[i].owner = owner;
        bul[i].src = (int8_t)bullet_src;
        return;
    }
}

static void update_cop(actor_t *a, float dt)
{
    if (a->flags & AF_DUTY) {                       // chasing a robber, leaves the player alone
        if (a->timer > 0) { a->timer -= dt; return; }
        if (rob_robber < 0 || act[rob_robber].kind == A_NONE || act[rob_robber].state == S_DEAD) { a->flags &= ~AF_DUTY; return; }
        actor_t *r = &act[rob_robber];
        if (r->flags & AF_CUFFED) return;
        float dx = wrapd(r->x - a->x), dy = wrapd(r->y - a->y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        a->ang = atan2f(dy, dx);
        a->state = S_CHASE;
        if (d < 10) {
            r->flags |= AF_CUFFED; r->state = S_IDLE; r->timer = 99;
            rob_state = RB_CUFFED; rob_t = 4;
            a->timer = 4;
            if (on_screen(a->x, a->y, 40)) game_message("ROBBER ARRESTED!", 2.5f);
            return;
        }
        move_slide(&a->x, &a->y, dx / d * 84, dy / d * 84, dt);
        return;
    }
    if (stars == 0) {
        // wanted level cleared — leave and vanish off-screen
        a->state = S_WALK;
        update_civ(a, dt);
        if (!on_screen(a->x, a->y, 30)) a->kind = A_NONE;
        return;
    }
    a->state = S_CHASE;
    float tx = plx, ty = ply;
    float dx = wrapd(tx - a->x), dy = wrapd(ty - a->y);
    float d = sqrtf(dx * dx + dy * dy) + 0.01f;
    a->ang = atan2f(dy, dx);

    // exiting a car / stunned — stands still and doesn't arrest
    if (a->timer > 0) { a->timer -= dt; return; }

    // arrest is not instant: while a cop is near, the meter builds (see update_world)
    if (ON_FOOT && d < 11) arrest_touch = true;
    if (pcar >= 0 && d < 18 && fabsf(cars[pcar].speed) < 20) { arrest_touch = true; arrest_car = true; }
    bool see = line_clear(a->x, a->y, tx, ty);
    float stop = (stars >= 3 && d < 90 && see) ? 50 : 6;
    if (d > stop) {
        float sp = COP_SPEED * (stars >= 2 ? 1.1f : 1.0f);
        float gx = dx, gy = dy, gd = d;
        if (!see || d > 60) {          // around buildings and water — follow the path map
            float nx, ny;
            if (nav_next(a->x, a->y, 2, false, &nx, &ny)) {
                gx = wrapd(nx - a->x); gy = wrapd(ny - a->y);
                gd = sqrtf(gx * gx + gy * gy) + 0.01f;
            }
        }
        move_slide(&a->x, &a->y, gx / gd * sp, gy / gd * sp, dt);
    }
    // shooting at 3+ stars
    a->cd -= dt;
    if (stars >= 3 && d < 130 && a->cd <= 0 && line_clear(a->x, a->y, tx, ty)) {
        float sp = (frnd() - 0.5f) * 0.25f;
        bool swat = (a->flags & AF_SWAT) != 0;
        fire_bullet(a->x, a->y, a->ang + sp * (swat ? 0.6f : 1), swat ? 340 : 300, swat ? 9 : 7, 1);
        a->cd = swat ? 0.3f + frnd() * 0.35f : 1.1f + frnd() * 0.6f;
        emit(a->x + cosf(a->ang) * 7, a->y + sinf(a->ang) * 7, -sinf(a->ang) * 40, cosf(a->ang) * 40, 0.4f, PT_CASING);
    }
    if (d > 520) a->kind = A_NONE;     // fell too far behind
}

// ================= Police cars =================
static void cop_car_start_chase(int i)
{
    car_t *c = &cars[i];
    c->mode = CAR_COP;
    c->stuck_t = 0;
    c->rev_t = 0;
}

static void spawn_cop_car_chasing(int i)
{
    car_t *c = &cars[i];
    // onto an off-screen road that has a path
    for (int k = 0; k < 200; k++) {
        float a = frnd() * 2 * PIf, d = 200 + frnd() * 220;
        int tx = (((int)(plx + cosf(a) * d)) & WMASK) >> 4, ty = (((int)(ply + sinf(a) * d)) & WMASK) >> 4;
        if (!lanes[MI(tx, ty)] || navget(navd, tx, ty) == 255) continue;
        float x = tx * 16 + 8, y = ty * 16 + 8;
        if (on_screen(x, y, 30)) continue;
        c->x = x; c->y = y;
        break;
    }
    c->ang = atan2f(wrapd(ply - c->y), wrapd(plx - c->x));
    c->speed = 0;
    c->hp = 140;
    c->crew = 2;
    c->burn_t = 0;
    c->hit_by_player = 0;
    cop_car_start_chase(i);
}

static void update_cop_car(int i, float dt)
{
    car_t *c = &cars[i];
    float pdx = wrapd(plx - c->x), pdy = wrapd(ply - c->y);
    float pd = sqrtf(pdx * pdx + pdy * pdy);

    // cops get out if the player is on foot nearby
    if (ON_FOOT && pd < 70 && c->crew > 0 && fabsf(c->speed) < 60) {
        float nx = -sinf(c->ang), ny = cosf(c->ang);
        int cops = count_kind(A_COP, true);
        int cap = (stars >= 4) ? 8 : stars * 2;
        // exit from the side farthest from the player, the second one slightly behind the first
        float far = (nx * pdx + ny * pdy > 0) ? -1.0f : 1.0f;
        float ca = cosf(c->ang), sa = sinf(c->ang);
        int k = 0;
        while (c->crew > 0 && cops < cap) {
            float along = (k == 0) ? 4.0f : -8.0f;
            float ex = wrapf(c->x + nx * 14 * far + ca * along), ey = wrapf(c->y + ny * 14 * far + sa * along);
            if (solid_at(ex, ey)) { ex = wrapf(c->x - nx * 14 * far); ey = wrapf(c->y - ny * 14 * far); }
            if (solid_at(ex, ey)) { ex = c->x; ey = c->y; }
            spawn_cop(ex, ey, COP_EXIT_DELAY + k * 0.3f);
            c->crew--; cops++; k++;
        }
        c->crew = 0;
        c->mode = CAR_PARKED;
        c->speed = 0;
        return;
    }

    float tx, ty;
    if (pd < 110 && line_clear(c->x, c->y, plx, ply)) {
        tx = plx; ty = ply;
    } else {
        // path via the map (BFS from the player), preferring roads
        if (!nav_next(c->x, c->y, 3, true, &tx, &ty)) { tx = plx; ty = ply; }
    }
    float ta = atan2f(wrapd(ty - c->y), wrapd(tx - c->x));
    float diff = fabsf(angle_diff(ta, c->ang));
    float ts = (diff < 0.5f) ? COP_CAR_MAX : (diff < 1.4f ? 80 : 45);
    if (stars >= 4) ts *= 1.2f;

    if (c->rev_t > 0) {
        c->rev_t -= dt;
        drive_free(i, c->ang + 1.0f, -70, 0.6f, dt);
    } else {
        drive_free(i, ta, ts, 0.5f, dt);
        if (c->stuck_t > 0.5f) { c->rev_t = 0.7f; c->stuck_t = 0; }
    }
    // ramming the player's car
    if (pcar >= 0 && pd < 22 && fabsf(c->speed) > 50) {
        damage_car(pcar, 1, false);
    }
    // running over the player on foot
    if (ON_FOOT && pd < 11 && fabsf(c->speed) > 50) {
        hurt_player(15);
        c->speed *= -0.3f;
    }
}

static void update_police(float dt)
{
    static float nav_t = 0;
    nav_t -= dt;
    if (stars > 0 && nav_t <= 0) { nav_update(plx, ply); nav_t = 0.4f; }
    int want = (stars == 0) ? 0 : (stars == 1 ? 1 : (stars <= 3 ? 2 : 3));   // 2 more cars — for the roadblock
    int chasing = 0;
    for (int i = NCIVCARS; i < POLICE_END; i++)
        if (cars[i].mode == CAR_COP && i != pcar) {
            chasing++;
            // fell behind on the other side of the map — an "interceptor" appears from off-screen nearby
            if (dist2w(cars[i].x, cars[i].y, plx, ply) > 1100.0f * 1100.0f && !on_screen(cars[i].x, cars[i].y, 40)) spawn_cop_car_chasing(i);
        }

    if (stars == 0) {
        // call-off: chasing cars turn into patrols
        for (int i = NCIVCARS; i < POLICE_END; i++) {
            if (cars[i].mode == CAR_COP) {
                cars[i].mode = CAR_PARKED;
                cars[i].speed = 0;
            }
        }
    } else if (chasing < want) {
        cop_spawn_t -= dt;
        if (cop_spawn_t <= 0) {
            // take a police car: patrol (AI), parked/wrecked off-screen
            for (int i = NCIVCARS; i < POLICE_END; i++) {
                car_t *c = &cars[i];
                if (i == pcar || c->mode == CAR_COP || plus_is_roadblock(i)) continue;
                if (c->mode == CAR_AI && on_screen(c->x, c->y, 40)) { cop_car_start_chase(i); break; }
                if (!on_screen(c->x, c->y, 40)) { spawn_cop_car_chasing(i); break; }
            }
            cop_spawn_t = 2.5f;
        }
    }
    // foot cops at 2+ stars arrive from off-screen
    if (stars >= 2) {
        static float foot_t = 0;
        foot_t -= dt;
        int cap = (stars >= 4) ? 6 : stars;
        if (foot_t <= 0 && count_kind(A_COP, true) < cap) {
            float x, y;
            offscreen_point(T_SIDEWALK, 190, 280, &x, &y);
            spawn_cop(x, y, 0.3f);
            foot_t = 4.0f - stars * 0.5f;
        }
    }
    // losing the wanted level
    if (minor_cd > 0) minor_cd -= dt;
    if (stars == 0 && heat > 0 && !cop_near(plx, ply, 170)) { heat -= 0.15f * dt; if (heat < 0) heat = 0; }   // minor offenses get forgotten
    if (stars > 0) {
        if (cop_near(plx, ply, 170)) lose_t = 0;
        else {
            lose_t += dt;
            heat -= 0.15f * dt;                          // unearned progress toward the next star fades too
            if (heat < STAR_T[stars]) heat = STAR_T[stars];
        }
        if (lose_t > 10.0f) {
            stars--;
            heat = STAR_T[stars] * 0.999f;
            if (stars == 0) heat = 0;
            lose_t = 0;
            game_message(stars ? "LOSING THEM" : "LOST THEM!", 1.5f);
        }
    }
}

// ================= Bullets, explosions, pickups =================
static void explode(float x, float y, bool by_player)
{
    spawn_fx(x, y, 0);
    add_decal(x, y, 1);
    emit_burst(x, y, 12, 120, 0.8f, PT_DEBRIS);
    emit_burst(x, y, 8, 40, 2.0f, PT_SMOKE);
    emit_burst(x, y, 10, 160, 0.4f, PT_SPARK);
    scare_civs(x, y, 160);
    fauna_explode(x, y, by_player);
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (a->kind && a->state != S_DEAD && dist2w(x, y, a->x, a->y) < 36 * 36) { knock_ang = atan2f(wrapd(a->y - y), wrapd(a->x - x)); knock_sp = 150; kill_actor(i, by_player); }
    }
    for (int i = 0; i < NCARS; i++) {
        if (cars[i].mode == CAR_WRECK) continue;
        float d2 = dist2w(x, y, cars[i].x, cars[i].y);
        if (d2 > 1 && d2 < 44 * 44) damage_car(i, 70, by_player);
    }
    if (ON_FOOT) {
        float d2 = dist2w(x, y, plx, ply);
        if (d2 < 40 * 40) hurt_player(d2 < 20 * 20 ? 100 : 45);
    }
    {   // camera shake from a nearby explosion
        float d = sqrtf(dist2w(x, y, plx, ply));
        if (d < 260) plus_shake(7 * (1 - d / 260));
    }
}

static void update_cars_damage(float dt)
{
    for (int i = 0; i < NCARS; i++) {
        car_t *c = &cars[i];
        if (c->mode == CAR_WRECK) {
            c->timer += dt;
            if (c->timer > 20 && !on_screen(c->x, c->y, 60)) respawn_offscreen(c);
            continue;
        }
        if (c->hp <= 0) {
            c->speed *= 0.97f;
            c->burn_t -= dt;
            if (c->burn_t <= 0) {
                bool byp = c->hit_by_player;
                if (i == pcar) { pcar = -1; hurt_player(200); }
                c->mode = CAR_WRECK;
                c->speed = 0;
                c->timer = 0;
                explode(c->x, c->y, byp);
                if (byp) { money += 20; add_heat(c->police ? 1.0f : (police_witness(plx, ply, 200) ? 0.4f : 0.0f)); }
            }
        }
    }
}

static void update_bullets(float dt)
{
    for (int i = 0; i < NBUL; i++) {
        bullet_t *b = &bul[i];
        if (b->life <= 0) continue;
        b->life -= dt;
        // 3 substeps so fast bullets don't pass through targets
        for (int s = 0; s < 3 && b->life > 0; s++) {
            b->x = wrapf(b->x + b->vx * dt / 3);
            b->y = wrapf(b->y + b->vy * dt / 3);
            if (b->src != -2 && wall_at(b->x, b->y)) { emit_burst(b->x - b->vx * 0.01f, b->y - b->vy * 0.01f, 3, 60, 0.25f, PT_SPARK); b->life = 0; break; }
            if (b->owner == 0 || b->owner == 2) {
                bool byp = b->owner == 0;
                for (int j = 0; j < NACT; j++) {
                    actor_t *a = &act[j];
                    if (!a->kind || a->state == S_DEAD || j == b->src) continue;
                    if (dist2w(b->x, b->y, a->x, a->y) < 5 * 5) {
                        a->hp -= b->dmg;
                        emit_burst(a->x, a->y, 4, 50, 0.4f, PT_BLOOD);
                        if (a->kind == A_GANG && byp) gang_hate[a->gang - 1] = 60;
                        if (a->hp <= 0) { knock_ang = atan2f(b->vy, b->vx); knock_sp = 70; add_decal(a->x, a->y, 2); kill_actor(j, byp); }
                        else if (a->kind == A_CIV) { if (!(a->flags & AF_ROBBER)) { a->state = S_FLEE; a->timer = 5; } }
                        else if (a->kind == A_COP) { if (byp) add_heat_minor(0.2f); a->timer = COP_STAGGER; }
                        b->life = 0;
                        break;
                    }
                }
            }
            if (b->life > 0 && b->owner != 1 && fauna_bullet(b->x, b->y, b->owner == 0)) { b->life = 0; break; }
            if (b->life > 0 && b->owner != 0 && ON_FOOT && dist2w(b->x, b->y, plx, ply) < 5 * 5) {
                hurt_player(b->dmg);
                b->life = 0;
            }
            if (b->life > 0 && b->owner != 0 && in_heli && dist2w(b->x, b->y, plx, ply) < 10 * 10) {
                plus_heli_damage(b->dmg);
                emit_burst(b->x, b->y, 3, 60, 0.25f, PT_SPARK);
                b->life = 0;
            }
            if (b->life <= 0) break;
            for (int j = 0; j < NCARS; j++) {
                car_t *c = &cars[j];
                if (c->mode == CAR_WRECK) continue;
                if (b->owner == 0 && j == pcar) continue;
                float dx = wrapd(b->x - c->x), dy = wrapd(b->y - c->y);
                if (dx * dx + dy * dy > 15 * 15) continue;
                float ca = cosf(c->ang), sa = sinf(c->ang);
                float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
                if (fabsf(u) < 12 && fabsf(v) < 6) {
                    damage_car(j, b->dmg / 2 + 2, b->owner == 0);
                    emit_burst(b->x, b->y, 4, 70, 0.3f, PT_SPARK);
                    b->life = 0;
                    break;
                }
            }
            if (b->life <= 0) break;
        }
    }
    for (int i = 0; i < NFX; i++) if (fx[i].t > 0) {
        fx[i].t += dt;
        if (fx[i].t > (fx[i].kind == 0 ? 0.9f : 0.15f)) fx[i].t = 0;
    }
}

static int find_target(float ang, float range, float cone)
{
    int best = -1;
    float bs = 1e9f;
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (!a->kind || a->state == S_DEAD) continue;
        float dx = wrapd(a->x - plx), dy = wrapd(a->y - ply);
        float d = sqrtf(dx * dx + dy * dy);
        if (d > range || d < 1) continue;
        float da = fabsf(angle_diff(atan2f(dy, dx), ang));
        if (da > cone) continue;
        float score = d * (1 + da * 2) * (a->kind == A_COP ? 0.7f : 1.0f);
        if (score < bs && line_clear(plx, ply, a->x, a->y)) { bs = score; best = i; }
    }
    return best;
}

static void player_fire(void)
{
    const weapon_t *w = &WEAP[weapon];
    if (fire_cd > 0) return;
    if (in_heli) { plus_heli_fire(); return; }
    if (pcar == FIRE_IDX) return;                    // in the fire truck "fire" is the water cannon
    if (weapon != W_FIST && ammo[weapon] <= 0) {
        game_message("NO AMMO", 0.8f);
        weapon = W_FIST;
        return;
    }
    fire_cd = w->cd;

    if (pcar >= 0) {                     // shooting forward from the car
        if (weapon == W_FIST) return;
        car_t *c = &cars[pcar];
        float ox = c->x + cosf(c->ang) * 15, oy = c->y + sinf(c->ang) * 15;
        for (int p = 0; p < w->pellets; p++)
            fire_bullet(ox, oy, c->ang + (frnd() - 0.5f) * w->spread, w->speed + fabsf(c->speed), w->dmg, 0);
        ammo[weapon]--;
        flash_t = 0.06f;
    } else if (weapon == W_FIST) {
        int t = find_target(pang, 14, 1.0f);
        if (t >= 0) {
            act[t].hp -= w->dmg;
            if (act[t].hp <= 0) kill_actor(t, true);
            else if (act[t].kind == A_GANG) gang_hate[act[t].gang - 1] = 60;
            else if (act[t].kind == A_COP) {
                add_heat_minor(0.4f);
                act[t].timer = COP_STAGGER * 2;            // punch — knocks the cop back
                float kx = wrapd(act[t].x - plx), ky = wrapd(act[t].y - ply), kl = sqrtf(kx * kx + ky * ky) + 0.01f;
                move_slide(&act[t].x, &act[t].y, kx / kl * 300, ky / kl * 300, 0.05f);
            }
            else { act[t].state = S_FLEE; act[t].timer = 5; add_heat_minor(0.15f); }
        }
        flash_t = 0.1f;
        return;
    } else {
        int t = find_target(pang, 170, 0.45f);      // auto-aim
        float aim = pang;
        if (t >= 0) aim = atan2f(wrapd(act[t].y - ply), wrapd(act[t].x - plx));
        float ox = plx + cosf(aim) * 6, oy = ply + sinf(aim) * 6;
        for (int p = 0; p < w->pellets; p++)
            fire_bullet(ox, oy, aim + (frnd() - 0.5f) * w->spread, w->speed * (0.9f + frnd() * 0.2f), w->dmg, 0);
        ammo[weapon]--;
        flash_t = 0.06f;
        pang = aim;
        // shell casing flies out to the right, shotgun kicks the camera
        emit(plx + cosf(aim) * 3, ply + sinf(aim) * 3, -sinf(aim) * 55 + frnd() * 10, cosf(aim) * 55 + frnd() * 10, 0.6f, PT_CASING);
        if (weapon == W_SHOTGUN) plus_shake(2.5f);
    }
    scare_civs(plx, ply, 130);
    fauna_noise(plx, ply, 230, true);
    if (police_witness(plx, ply, 150)) add_heat_minor(0.2f);
}

static void next_weapon(int step)
{
    for (int k = 0; k < W_COUNT; k++) {
        weapon = (weapon + step + W_COUNT) % W_COUNT;
        if (weapon == W_FIST || ammo[weapon] > 0) break;
    }
    game_message(WEAP[weapon].name, 0.8f);
}

static void update_pickups(float dt)
{
    for (int i = 0; i < NPICK; i++) {
        pickup_t *p = &pick[i];
        if (!p->active) {
            p->timer -= dt;
            if (p->timer <= 0) p->active = 1;
            continue;
        }
        if (!in_heli && dist2w(p->x, p->y, plx, ply) < 12 * 12) {
            p->active = 0;
            p->timer = 25;
            switch (p->kind) {
            case PK_PISTOL:  ammo[W_PISTOL] += WEAP[W_PISTOL].give; weapon = W_PISTOL; game_message("PISTOL", 1); break;
            case PK_UZI:     ammo[W_UZI] += WEAP[W_UZI].give; weapon = W_UZI; game_message("UZI", 1); break;
            case PK_SHOTGUN: ammo[W_SHOTGUN] += WEAP[W_SHOTGUN].give; weapon = W_SHOTGUN; game_message("SHOTGUN", 1); break;
            case PK_HEALTH:  php += 50; if (php > 100) php = 100; game_message("HEALTH", 1); break;
            case PK_FRENZY:
                frenzy_t = 30; frenzy_kills = 0; ammo[W_UZI] += 150; weapon = W_UZI; p->timer = 90;
                game_message("KILL FRENZY! 30 SEC", 2.5f);
                break;
            case PK_PACKAGE: {
                packages++; money += 250; p->timer = 1e9f; save_req = true;
                char s2[48];
                snprintf(s2, sizeof(s2), "PACKAGE %d/%d +$250", packages % 100, NPACKAGES);
                game_message(s2, 2.5f);
                break;
            }
            }
        }
    }
}

// ================= Car: enter / exit =================
static int nearest_car(void)
{
    int best = -1;
    float bd = 26 * 26;
    for (int i = 0; i < NCARS; i++) {
        if (cars[i].mode == CAR_PLAYER || cars[i].mode == CAR_WRECK || cars[i].hp <= 0) continue;
        float d2 = dist2w(cars[i].x, cars[i].y, plx, ply);
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}

static void action(void)
{
    if (plus_heli_action()) return;
    if (ON_FOOT) {
        int i = nearest_car();
        if (i < 0) return;
        car_t *c = &cars[i];
        if (c->mode == CAR_AI || c->mode == CAR_COP) {
            // carjacking: the driver jumps out and runs away
            int j = free_actor();
            if (j >= 0) {
                spawn_civ(&act[j], false);
                float nx = -sinf(c->ang), ny = cosf(c->ang);
                act[j].x = wrapf(c->x + nx * 14); act[j].y = wrapf(c->y + ny * 14);
                if (solid_at(act[j].x, act[j].y)) { act[j].x = c->x; act[j].y = c->y; }
                if (c->police) act[j].kind = A_COP, act[j].color = 6, act[j].state = S_CHASE, act[j].hp = 70;
                else { act[j].state = S_FLEE; act[j].timer = 4; }
            }
            add_heat(c->police ? 1.0f : (police_witness(plx, ply, 200) ? 0.6f : 0.0f));
        }
        if (c->police && c->mode != CAR_COP) add_heat(0.5f);
        pcar = i;
        c->mode = CAR_PLAYER;
        c->crew = 0;
        return;
    }
    car_t *c = &cars[pcar];
    if (fabsf(c->speed) > 60) return;            // can't jump out while moving
    float nx = -sinf(c->ang), ny = cosf(c->ang);
    for (int side = -1; side <= 1; side += 2) {
        float ex = c->x + nx * 16 * side, ey = c->y + ny * 16 * side;
        if (!solid_at(ex, ey)) {
            plx = wrapf(ex);
            ply = wrapf(ey);
            c->mode = CAR_PARKED;
            c->speed = 0;
            pcar = -1;
            return;
        }
    }
}

// ================= Mission (delivery) =================
static void pick_far_point(float fx_, float fy, float min_d, float *ox, float *oy)
{
    float best = 1e9f;
    for (int tries = 0; tries < 200; tries++) {
        float x, y;
        random_tile_point(T_SIDEWALK, &x, &y);
        float d2 = dist2w(x, y, fx_, fy);
        if (d2 >= min_d * min_d && d2 <= (min_d * 3) * (min_d * 3)) { *ox = x; *oy = y; return; }
        if (fabsf(d2 - min_d * min_d) < best) { best = fabsf(d2 - min_d * min_d); *ox = x; *oy = y; }
    }
}

static void mission_start(int type)
{
    if (type >= M_COUNT) { plus_mission_start(type); return; }
    mtype = type;
    stage = 0;
    mtarget = -1;
    if (type == M_DELIVER) {
        pick_far_point(plx, ply, 200, &mis_x, &mis_y);
        game_message("JOB: PICK UP THE PACKAGE", 2.5f);
    } else if (type == M_HIT) {
        int j = free_actor();
        if (j < 0) { mission_start(M_DELIVER); return; }
        spawn_civ(&act[j], false);
        offscreen_point(T_SIDEWALK, 300, 600, &act[j].x, &act[j].y);
        act[j].color = 7;            // suit
        act[j].flags = 1;
        act[j].hp = 60;
        mtarget = j;
        game_message("JOB: KILL THE MAN IN BLACK", 2.5f);
    } else {
        // gold car is parked 400-700 px away
        int k = 1;
        for (int tries = 0; tries < 20; tries++) { k = 1 + rnd() % (NCIVCARS - 1); if (k != pcar) break; }
        if (k == pcar) { mission_start(M_DELIVER); return; }
        float a = frnd() * 2 * PIf, d = 400 + frnd() * 300;
        int tx = (((int)(plx + cosf(a) * d)) & WMASK) >> 4, ty = (((int)(ply + sinf(a) * d)) & WMASK) >> 4;
        float px, py, cx, cy;
        int dir = 0;
        find_curb(tx, ty, &px, &py, &cx, &cy, &dir);
        car_t *c = &cars[k];
        car_reset(c);
        c->x = cx; c->y = cy; c->ang = dir * PIf / 2; c->dir = dir;
        c->mode = CAR_PARKED; c->speed = 0; c->col = 9;
        mtarget = k;
        game_message("JOB: STEAL THE GOLD CAR", 2.5f);
    }
}

static void mission_done(int pay, const char *what)
{
    int next = plus_mission_done(pay);
    money += pay;
    deliveries++;
    save_req = true;
    char s2[32];
    snprintf(s2, sizeof(s2), "%s +$%d", what, pay);
    game_message(s2, 2.5f);
    if (mtype == M_HIT && mtarget >= 0) act[mtarget].flags = 0;
    mission_start(next);
}

static const char *mission_text(void)
{
    if (mtype >= M_COUNT) return plus_mission_text();
    switch (mtype) {
    case M_DELIVER: return stage == 0 ? "PICK UP THE PACKAGE" : "DELIVER THE PACKAGE";
    case M_HIT:     return "KILL THE MAN IN BLACK";
    default:        return stage == 0 ? "STEAL THE GOLD CAR" : "BRING IT TO THE GARAGE";
    }
}

static void update_mission(void)
{
    if (mtype >= M_COUNT) { plus_mission_update(); return; }
    if (mtype == M_DELIVER) {
        if (dist2w(mis_x, mis_y, plx, ply) > 24 * 24) return;
        if (stage == 0) {
            stage = 1;
            pick_far_point(plx, ply, 300, &mis_x, &mis_y);
            reward = 30 + (int)(sqrtf(dist2w(mis_x, mis_y, plx, ply)) / 5);
            game_message("DELIVER TO YELLOW", 2);
        } else mission_done(reward, "DELIVERED");
    } else if (mtype == M_HIT) {
        actor_t *t = &act[mtarget];
        if (mtarget < 0 || t->kind == A_NONE || !(t->flags & 1)) { mission_start(M_HIT); return; }
        mis_x = t->x; mis_y = t->y;
        if (t->state == S_DEAD) mission_done(150, "TARGET DOWN");
    } else {
        car_t *c = &cars[mtarget];
        if (c->col != 9 || c->mode == CAR_WRECK || c->hp <= 0) {
            game_message("JOB FAILED", 2);
            if (c->col == 9) c->col = mtarget % 8;
            mission_start(M_DELIVER);
            return;
        }
        if (stage == 0) {
            mis_x = c->x; mis_y = c->y;
            if (pcar == mtarget) {
                stage = 1;
                pick_far_point(plx, ply, 350, &mis_x, &mis_y);
                game_message("TO THE GARAGE!", 2);
            }
        } else if (pcar == mtarget && dist2w(mis_x, mis_y, plx, ply) < 28 * 28 && fabsf(c->speed) < 60) {
            c->col = mtarget % 8;
            mission_done(250, "CAR DELIVERED");
        }
    }
}

// ================= Shops and casino =================
typedef struct { const char *name; int price; } item_t;
static const item_t SHOP_ITEMS[NPOI][4] = {
    { {"PISTOL AMMO", 50}, {"UZI", 200}, {"SHOTGUN", 300}, {"BODY ARMOR", 150} },
    { {"CHICKEN MEAL", 10}, {"FAMILY BUCKET", 25}, {0, 0}, {0, 0} },
    { {"BUY SEDAN", 300}, {"BUY SPORTS CAR", 1200}, {0, 0}, {0, 0} },
    { {"SLOTS", 100}, {"BIG SLOTS", 500}, {"ROULETTE RED", 200}, {"ROULETTE BLACK", 200} },
    { {"ROB THE VAULT", 0}, {0, 0}, {0, 0}, {0, 0} },
};
static int poi_near = -1, shop_item = 0;
static float spin_t = 0;            // > 0 — reels are spinning
static int spin_bet = 0;
static char spin_sym[4] = "777";
static int spin_kind = 0;                 // 0 = slot machine, 1 = roulette
static float spin_len = 1.8f;
static int pend_win = 0;                  // winnings are paid out when the reels stop
static bool pend_jackpot = false;
static float wheel_ang = 0, wheel_v = 0.6f, ball_ang = 0, ball_v = 0, ball_r = 50;
static int roul_pocket = -1;
static bool roul_red = true;
static bool super_won = false;

// park car k at the curb near point (x, y)
static void park_car_near(int k, float x, float y, int col)
{
    float px, py, cx, cy;
    int dir = 0;
    find_curb(((int)x) >> 4, ((int)y) >> 4, &px, &py, &cx, &cy, &dir);
    car_t *c = &cars[k];
    car_reset(c);
    c->x = cx; c->y = cy; c->dir = dir; c->ang = dir * PIf / 2;
    c->mode = CAR_PARKED; c->speed = 0; c->col = col; c->police = 0;
    c->hp = (col == 11) ? 160 : 100;
}

static void casino_result(void);
static void shop_buy(void)
{
    const item_t *it = &SHOP_ITEMS[poi_near][shop_item];
    if (spin_t > 0) return;
    if (money < it->price) { game_message("NOT ENOUGH MONEY", 1.5f); return; }
    money -= it->price;
    save_req = true;
    switch (poi_near) {
    case POI_GUNS:
        if (shop_item == 0) { ammo[W_PISTOL] += 36; weapon = W_PISTOL; }
        else if (shop_item == 1) { ammo[W_UZI] += 150; weapon = W_UZI; }
        else if (shop_item == 2) { ammo[W_SHOTGUN] += 20; weapon = W_SHOTGUN; }
        else armor = 100;
        game_message(it->name, 1.5f);
        break;
    case POI_FOOD:
        php += shop_item ? 100 : 30;
        if (php > 100) php = 100;
        game_message("YUM!", 1.5f);
        break;
    case POI_CARS: {
        // free civilian car far from the player
        int k = -1;
        for (int i = 1; i < NCIVCARS; i++)
            if (i != pcar && !(mtype == M_STEAL && i == mtarget) && !on_screen(cars[i].x, cars[i].y, 40)) { k = i; break; }
        if (k < 0) { money += it->price; game_message("NO CARS TODAY", 1.5f); break; }
        park_car_near(k, pois[POI_CARS].dx, pois[POI_CARS].dy, shop_item ? 12 : (int)(rnd() % 8));
        game_message(shop_item ? "SPORTS CAR OUTSIDE!" : "YOUR CAR IS OUTSIDE", 2);
        break;
    }
    case POI_BANK:
        heist_start();
        break;
    case POI_CASINO:
        spin_bet = it->price;
        if (shop_item < 2) { spin_kind = 0; spin_len = spin_t = 1.9f; casino_result(); }
        else {
            spin_kind = 1; spin_len = spin_t = 4.0f;
            roul_red = (shop_item == 2);
            wheel_v = 5 + frnd() * 2; ball_v = -(10 + frnd() * 3); ball_r = 50; roul_pocket = -1;
        }
        break;
    }
}

static void casino_result(void)
{
    pend_win = 0; pend_jackpot = false;
    int r = rnd() % 100;
    bool big = spin_bet >= 500;
    int win = 0;
    if (big && r < 8) {
        strcpy(spin_sym, "777");
        if (!super_won) { pend_jackpot = true; return; }
        win = 5000;
    } else if (r < (big ? 16 : 3))  { strcpy(spin_sym, "$$$"); win = spin_bet * (big ? 4 : 10); }
    else if (r < (big ? 36 : 12))   { strcpy(spin_sym, "***"); win = spin_bet * (big ? 2 : 3); }
    else if (r < (big ? 36 : 32))   { strcpy(spin_sym, "xx*"); win = spin_bet * 2; }
    else { static const char *lose[4] = { "7$*", "x7$", "*x7", "$*x" }; strcpy(spin_sym, lose[rnd() & 3]); }
    pend_win = win;
}

static void slot_payout(void)
{
    if (pend_jackpot) {
        super_won = true;
        park_car_near(SUPER_IDX, pois[POI_CASINO].dx, pois[POI_CASINO].dy, 11);
        game_message("JACKPOT! VORTEX GT IS YOURS!", 4);
    } else {
        money += pend_win;
        char s2[32];
        if (pend_win) snprintf(s2, sizeof(s2), "YOU WIN $%d!", pend_win); else snprintf(s2, sizeof(s2), "NO LUCK");
        game_message(s2, 2);
    }
    save_req = true;
}

#define ROUL_N 37
static void roulette_payout(void)
{
    float rel = fmodf(ball_ang - wheel_ang, 2 * PIf);
    if (rel < 0) rel += 2 * PIf;
    roul_pocket = (int)(rel / (2 * PIf / ROUL_N)) % ROUL_N;
    bool red = roul_pocket != 0 && (roul_pocket & 1), black = roul_pocket != 0 && !(roul_pocket & 1);
    int win = ((roul_red && red) || (!roul_red && black)) ? spin_bet * 2 : 0;
    money += win;
    char s2[40];
    snprintf(s2, sizeof(s2), "%d %s - %s", roul_pocket, roul_pocket == 0 ? "GREEN" : red ? "RED" : "BLACK", win ? "YOU WIN!" : "NO LUCK");
    game_message(s2, 3);
    save_req = true;
}

static void update_shops(float dt)
{
    poi_near = -1;
    if (ON_FOOT)
        for (int k = 0; k < NPOI; k++)
            if (dist2w(pois[k].dx, pois[k].dy, plx, ply) < 14 * 14) { poi_near = k; break; }
    static int last_poi = -1;
    if (poi_near != last_poi) shop_item = 0;      // entered another shop — start from the first item
    last_poi = poi_near;
    if (poi_near >= 0 && !SHOP_ITEMS[poi_near][shop_item].name) shop_item = 0;
    // roulette: the wheel always spins while you're in the casino; the ball — during a bet
    wheel_ang += wheel_v * dt;
    if (spin_kind == 1 && spin_t > 0) {
        wheel_v += (0.6f - wheel_v) * dt * 0.5f;
        ball_ang += ball_v * dt;
        ball_v *= expf(-dt * 0.9f);
        if (ball_r > 43) ball_r -= dt * 3;
        if (spin_t < 0.9f) {                        // the ball drops into a pocket and rides with the wheel
            if (roul_pocket < 0) {
                float rel = fmodf(ball_ang - wheel_ang, 2 * PIf);
                if (rel < 0) rel += 2 * PIf;
                roul_pocket = (int)(rel / (2 * PIf / ROUL_N)) % ROUL_N;
            }
            ball_ang = wheel_ang + (roul_pocket + 0.5f) * (2 * PIf / ROUL_N);
            ball_r = 41;
        }
    } else wheel_v += (0.6f - wheel_v) * dt;
    if (spin_t > 0) {
        spin_t -= dt;
        if (spin_t <= 0) { if (spin_kind == 0) slot_payout(); else roulette_payout(); }
    }
    // VORTEX GT: if wrecked and far away — waits by the casino again
    car_t *sc = &cars[SUPER_IDX];
    if (super_won && SUPER_IDX != pcar && (sc->mode == CAR_WRECK || sc->mode == CAR_OFF) && !on_screen(sc->x, sc->y, 60))
        park_car_near(SUPER_IDX, pois[POI_CASINO].dx, pois[POI_CASINO].dy, 11);
}

// ================= City events =================
static bool walkable_xy(float x, float y) { int k = tile_at(x, y); return k != T_BUILDING && k != T_WATER; }

static bool point_near(float cx, float cy, float rmin, float rmax, float *ox, float *oy)
{
    for (int k = 0; k < 40; k++) {
        float a = frnd() * 2 * PIf, d = rmin + frnd() * (rmax - rmin);
        float x = wrapf(cx + cosf(a) * d), y = wrapf(cy + sinf(a) * d);
        if (walkable_xy(x, y) && tile_at(x, y) != T_ROAD) { *ox = x; *oy = y; return true; }
    }
    return false;
}

// ---- gangs ----
static int spawn_gang(int gang, float x, float y, bool fighting)
{
    int j = free_actor();
    if (j < 0) return -1;
    actor_t *a = &act[j];
    a->kind = A_GANG; a->gang = gang; a->flags = 0;
    a->state = fighting ? S_CHASE : S_WALK;
    a->hp = 60; a->x = x; a->y = y;
    a->timer = 1; a->cd = 0.5f + frnd();
    a->dir = rnd() % 4; a->ang = a->dir * PIf / 2;
    a->color = (gang == 1) ? 8 : 9;
    return j;
}

static void gang_presence(float dt)
{
    static float t = 0;
    t -= dt;
    if (t > 0) return;
    t = 1.5f;
    int ptx = ((int)plx) >> 4, pty = ((int)ply) >> 4;
    // remove extra gangsters (after shootouts) when they're out of sight
    if (count_kind(A_GANG, true) > 8)
        for (int i = 0; i < NACT; i++)
            if (act[i].kind == A_GANG && act[i].state != S_CHASE && !on_screen(act[i].x, act[i].y, 40)) { act[i].kind = A_NONE; break; }
    bool near = false;                           // player is in the gang's district or nearby (16x16 tile blocks)
    for (int dy = -16; dy <= 16 && !near; dy += 16)
        for (int dx = -16; dx <= 16; dx += 16) if (gang_zone(ptx + dx, pty + dy)) { near = true; break; }
    if (!near) return;
    if (count_kind(A_GANG, true) >= 6) return;
    float x, y;
    offscreen_point(T_SIDEWALK, 150, 320, &x, &y);
    int g = gang_zone(((int)x) >> 4, ((int)y) >> 4);   // west of ghetto A — green, east and ghetto B — purple
    if (!g) return;
    spawn_gang(g, x, y, false);
    if (walkable_xy(x + 7, y + 3)) spawn_gang(g, x + 7, y + 3, false);
}

static void update_gang(int i, float dt)
{
    actor_t *a = &act[i];
    a->cd -= dt;
    float tx = 0, ty = 0, td = 200 * 200;
    bool has = false;
    if (a->state == S_CHASE)
        for (int j = 0; j < NACT; j++) {
            actor_t *b = &act[j];
            if (b->kind != A_GANG || b->state == S_DEAD || b->gang == a->gang) continue;
            float d2 = dist2w(a->x, a->y, b->x, b->y);
            if (d2 < td) { td = d2; tx = b->x; ty = b->y; has = true; }
        }
    if (gang_hate[a->gang - 1] > 0) {
        float d2 = dist2w(a->x, a->y, plx, ply);
        if (d2 < 180 * 180 && d2 < td) { td = d2; tx = plx; ty = ply; has = true; }
    }
    if (has) {
        float dx = wrapd(tx - a->x), dy = wrapd(ty - a->y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        a->ang = atan2f(dy, dx);
        float vx, vy;
        if (d > 90)      { vx = dx / d * 48; vy = dy / d * 48; }
        else if (d < 40) { vx = -dx / d * 40; vy = -dy / d * 40; }
        else { float sgn = sinf(g_time * 1.3f + i) > 0 ? 1 : -1; vx = -dy / d * 25 * sgn; vy = dx / d * 25 * sgn; }   // dashes between cover
        move_slide(&a->x, &a->y, vx, vy, dt);
        if (a->cd <= 0 && d < 170 && line_clear(a->x, a->y, tx, ty)) {
            bullet_src = i;
            fire_bullet(a->x + cosf(a->ang) * 6, a->y + sinf(a->ang) * 6, a->ang + (frnd() - 0.5f) * 0.3f, 300, 6, 2);
            bullet_src = -1;
            a->cd = 0.9f + frnd() * 1.0f;
            scare_civs(a->x, a->y, 110);
            emit(a->x + cosf(a->ang) * 7, a->y + sinf(a->ang) * 7, 0, 0, 0.08f, PT_SPARK);
        }
        return;
    }
    if (a->state == S_CHASE) a->state = S_WALK;
    // hang out: stand in groups, sometimes walk, don't leave the ghetto
    if (a->state != S_IDLE && rnd() % 150 == 0) { a->state = S_IDLE; a->timer = 3 + frnd() * 6; }
    if (a->state == S_IDLE) { a->timer -= dt; if (a->timer <= 0) a->state = S_WALK; }
    else {
        float ox = a->x, oy = a->y;
        update_civ(a, dt);
        if (!gang_zone(((int)a->x) >> 4, ((int)a->y) >> 4)) { a->x = ox; a->y = oy; a->dir = (a->dir + 2) & 3; }
        if (a->state == S_FLEE) a->state = S_WALK;
    }
    if (dist2w(a->x, a->y, plx, ply) > 700.0f * 700.0f) a->kind = A_NONE;
}

// ---- shootout at CHIKKO ----
static void update_shootout(float dt)
{
    static float cd = 35, t = 0;
    float cx = pois[POI_FOOD].dx, cy = pois[POI_FOOD].dy;
    if (t > 0) {
        t -= dt;
        if (t <= 0)
            for (int i = 0; i < NACT; i++) if (act[i].kind == A_GANG && act[i].state == S_CHASE) act[i].state = S_WALK;
        return;
    }
    cd -= dt;
    if (cd > 0) return;
    if (dist2w(cx, cy, plx, ply) > 420.0f * 420.0f) { cd = 5; return; }
    cd = 50 + rnd() % 30;
    t = 25;
    for (int g = 1; g <= 2; g++)
        for (int k = 0; k < 3; k++) {
            float x, y;
            float side = (g == 1) ? -1 : 1;
            if (point_near(cx + side * 60, cy, 0, 30, &x, &y)) spawn_gang(g, x, y, true);
        }
    scare_civs(cx, cy, 200);
    if (dist2w(cx, cy, plx, ply) < 300.0f * 300.0f) game_message("SHOOTOUT AT CHIKKO!", 2.5f);
}

// ---- pedestrian robbery ----
static void rob_end(void)
{
    if (rob_victim >= 0) act[rob_victim].flags &= ~AF_VICTIM;
    if (rob_cop >= 0 && act[rob_cop].kind == A_COP) act[rob_cop].flags &= ~AF_DUTY;
    if (rob_robber >= 0 && act[rob_robber].kind && act[rob_robber].state != S_DEAD) {
        if (act[rob_robber].flags & AF_CUFFED) act[rob_robber].kind = A_NONE;   // taken to the station
        else act[rob_robber].flags &= ~AF_ROBBER;
    }
    rob_state = RB_NONE; rob_robber = rob_victim = rob_cop = -1;
}

static void update_robbery(float dt)
{
    static float cd = 25;
    if (rob_state != RB_NONE && rob_robber >= 0 && (act[rob_robber].kind == A_NONE || act[rob_robber].state == S_DEAD)) { rob_end(); return; }
    switch (rob_state) {
    case RB_NONE: {
        cd -= dt;
        if (cd > 0) return;
        cd = 35 + rnd() % 25;
        int v = -1;
        for (int j = 0; j < NACT; j++) {
            actor_t *a = &act[j];
            if (a->kind != A_CIV || a->state == S_DEAD || a->flags) continue;
            float d2 = dist2w(a->x, a->y, plx, ply);
            if (d2 > 60 * 60 && d2 < 200 * 200) { v = j; break; }
        }
        if (v < 0) return;
        float x, y;
        if (!point_near(act[v].x, act[v].y, 60, 90, &x, &y)) return;
        int r = free_actor();
        if (r < 0 || r == v) return;
        spawn_civ(&act[r], false);
        act[r].x = x; act[r].y = y; act[r].color = 10; act[r].flags = AF_ROBBER; act[r].hp = 50;
        act[v].flags |= AF_VICTIM;
        rob_robber = r; rob_victim = v; rob_cop = -1;
        rob_state = RB_APPROACH; rob_t = 15;
        break;
    }
    case RB_APPROACH: {
        actor_t *r = &act[rob_robber], *v = &act[rob_victim];
        rob_t -= dt;
        if (v->kind != A_CIV || v->state == S_DEAD || rob_t <= 0) { rob_end(); return; }
        float dx = wrapd(v->x - r->x), dy = wrapd(v->y - r->y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        r->ang = atan2f(dy, dx);
        r->state = S_WALK;
        move_slide(&r->x, &r->y, dx / d * 58, dy / d * 58, dt);
        if (d < 9) {
            rob_state = RB_ROB; rob_t = 1.6f;
            v->state = S_IDLE; v->timer = 2; r->state = S_IDLE; r->timer = 2;
            if (on_screen(v->x, v->y, 20)) game_message("ROBBERY!", 2);
        }
        break;
    }
    case RB_ROB:
        rob_t -= dt;
        if (rob_t <= 0) {
            actor_t *r = &act[rob_robber], *v = &act[rob_victim];
            v->state = S_FLEE; v->timer = 5;
            r->state = S_WALK;
            float x, y;
            offscreen_point(T_SIDEWALK, 120, 200, &x, &y);
            rob_cop = spawn_cop(x, y, 0.3f);
            if (rob_cop >= 0) act[rob_cop].flags |= AF_DUTY;
            rob_state = RB_FLEE; rob_t = 25;
        }
        break;
    case RB_FLEE: {
        actor_t *r = &act[rob_robber];
        rob_t -= dt;
        float fx = act[rob_victim].x, fy = act[rob_victim].y;
        if (rob_cop >= 0 && act[rob_cop].kind == A_COP) { fx = act[rob_cop].x; fy = act[rob_cop].y; }
        float dx = wrapd(r->x - fx), dy = wrapd(r->y - fy), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        float wob = sinf(g_time * 3 + rob_robber) * 30;
        move_slide(&r->x, &r->y, dx / d * 66 - dy / d * wob, dy / d * 66 + dx / d * wob, dt);
        r->ang = atan2f(dy, dx);
        if (rob_t <= 0 || dist2w(r->x, r->y, plx, ply) > 600.0f * 600.0f) rob_end();
        break;
    }
    case RB_CUFFED:
        rob_t -= dt;
        if (rob_t <= 0) rob_end();
        break;
    }
}

// ---- fire and fire truck ----
static void update_fire(float dt)
{
    static float cd = 40, smoke_t = 0;
    car_t *tr = &cars[FIRE_IDX];
    if (fire_stage == 0) {
        if (tr->mode == CAR_PARKED && FIRE_IDX != pcar && !on_screen(tr->x, tr->y, 60)) tr->mode = CAR_OFF;
        cd -= dt * (pcar == FIRE_IDX ? 5 : 1);      // fires are more frequent when in the fire truck
        if (cd > 0) return;
        cd = 55 + rnd() % 35;
        int lot = 0;
        for (int k = 0; k < 80 && !lot; k++) {
            int i = 1 + rnd() % g_nlots;
            const lot_t *l = &g_lots[i];
            float cx = (l->x0 + l->x1 + 1) * 8.0f, cy = (l->y0 + l->y1 + 1) * 8.0f;
            float d2 = dist2w(cx, cy, plx, ply);
            bool poi = false;
            for (int q = 0; q < NPOI; q++) if (pois[q].lot == i) poi = true;
            if (!poi && d2 > 200 * 200 && d2 < 460 * 460) { lot = i; fire_x = cx; fire_y = cy; }
        }
        if (!lot) return;
        fire_lot = lot;
        int rx, ry;
        if (!find_near(((int)fire_x) >> 4, ((int)fire_y) >> 4, T_ROAD, &rx, &ry)) return;
        fire_rx = rx * 16 + 8; fire_ry = ry * 16 + 8;
        bfs_from(navf, fire_rx, fire_ry);
        fire_hp = 100; fire_t = 60; fire_stage = 1;
        // fire truck arrives from off-screen
        if (FIRE_IDX != pcar) {
            for (int k = 0; k < 200; k++) {
                float a = frnd() * 2 * PIf, d = 300 + frnd() * 250;
                int tx = (((int)(fire_rx + cosf(a) * d)) & WMASK) >> 4, ty = (((int)(fire_ry + sinf(a) * d)) & WMASK) >> 4;
                if (!lanes[MI(tx, ty)] || navget(navf, tx, ty) == 255) continue;
                if (on_screen(tx * 16 + 8, ty * 16 + 8, 30)) continue;
                car_reset(tr);
                tr->x = tx * 16 + 8; tr->y = ty * 16 + 8; tr->ang = atan2f(wrapd(fire_ry - tr->y), wrapd(fire_rx - tr->x));
                tr->speed = 0; tr->mode = CAR_FIRE; tr->col = 10; tr->police = 0; tr->hp = 200;
                break;
            }
        }
        if (dist2w(fire_x, fire_y, plx, ply) < 400.0f * 400.0f) game_message("FIRE! FIRE TRUCK ON THE WAY", 2.5f);
        return;
    }
    if (fire_stage == 1) {
        fire_t -= dt;
        smoke_t -= dt;
        const lot_t *l = &g_lots[fire_lot];
        if (smoke_t <= 0) {
            smoke_t = 0.12f;
            float sx = l->x0 * 16 + frnd() * (l->x1 - l->x0 + 1) * 16, sy = l->y0 * 16 + frnd() * (l->y1 - l->y0 + 1) * 16;
            emit(sx, sy, frnd() * 10 - 5, -10, 2.2f * fire_hp / 100 + 0.5f, PT_SMOKE);
            scare_civs(fire_x, fire_y, 70);
        }
        if (tr->mode == CAR_FIRE) {
            float dx = wrapd(fire_rx - tr->x), dy = wrapd(fire_ry - tr->y), d = sqrtf(dx * dx + dy * dy);
            if (d > 34) {
                float tx, ty;
                if (!nav_next_f(navf, tr->x, tr->y, 3, true, &tx, &ty)) { tx = fire_rx; ty = fire_ry; }
                float ta = atan2f(wrapd(ty - tr->y), wrapd(tx - tr->x));
                float diff = fabsf(angle_diff(ta, tr->ang));
                if (tr->rev_t > 0) { tr->rev_t -= dt; drive_free(FIRE_IDX, tr->ang + 1.0f, -60, 0.6f, dt); }
                else {
                    drive_free(FIRE_IDX, ta, diff < 0.5f ? 120 : 55, 0.5f, dt);
                    if (tr->stuck_t > 0.6f) { tr->rev_t = 0.7f; tr->stuck_t = 0; }
                }
            } else {
                // on site: brake and extinguish
                tr->speed *= 0.85f;
                fire_hp -= 9 * dt;
                float ax = wrapd(fire_x - tr->x), ay = wrapd(fire_y - tr->y), al = sqrtf(ax * ax + ay * ay) + 0.01f;
                for (int k = 0; k < 2; k++)
                    emit(tr->x, tr->y, ax / al * 110 + frnd() * 20 - 10, ay / al * 110 + frnd() * 20 - 10, 0.5f, PT_WATER);
            }
        }
        if (fire_hp <= 0 || fire_t <= 0) {
            if (dist2w(fire_x, fire_y, plx, ply) < 400.0f * 400.0f) game_message(fire_hp <= 0 ? "FIRE IS OUT" : "THE FIRE BURNED OUT", 2);
            fire_stage = 2; fire_t = 5;
        }
        return;
    }
    // 2 — firefighters leave
    fire_t -= dt;
    if (fire_t <= 0) {
        if (tr->mode == CAR_FIRE) tr->mode = CAR_PARKED;
        fire_stage = 0;
    }
}

static void update_events(float dt)
{
    for (int g = 0; g < 2; g++) if (gang_hate[g] > 0) gang_hate[g] -= dt;
    gang_presence(dt);
    update_shootout(dt);
    update_robbery(dt);
    update_fire(dt);
}

// Pay'n'Spray and "massacre"
static void update_activities(float dt)
{
    update_shops(dt);
    update_events(dt);
    if (spray_cd > 0) spray_cd -= dt;
    if (pcar >= 0 && spray_cd <= 0 && dist2w(spray_x, spray_y, plx, ply) < 20 * 20) {
        spray_cd = 4;
        car_t *c = &cars[pcar];
        if (stars == 0) game_message("PAY'N'SPRAY: COME WHEN WANTED", 2);
        else if (money < 100) game_message("RESPRAY COSTS $100", 2);
        else {
            money -= 100;
            reset_police();
            if (c->col < 8) c->col = (c->col + 3) % 8;
            c->hp = c->police ? 140 : 100;
            emit_burst(c->x, c->y, 10, 30, 1.0f, PT_SMOKE);
            game_message("RESPRAYED! -$100", 2.5f);
            save_req = true;
        }
    }
    if (frenzy_t > 0) {
        frenzy_t -= dt;
        if (frenzy_t <= 0) {
            char s2[32];
            snprintf(s2, sizeof(s2), "FRENZY OVER: %d KILLS", frenzy_kills);
            game_message(s2, 3);
        }
    }
}

// ================= Initialization / respawn =================
static void reset_police(void)
{
    heat = 0; stars = 0; lose_t = 0;
    for (int i = 0; i < NACT; i++) if (act[i].kind == A_COP) act[i].kind = A_NONE;
    for (int i = 0; i < NBUL; i++) bul[i].life = 0;
    for (int i = NCIVCARS; i < POLICE_END; i++) {
        if (cars[i].mode == CAR_COP || cars[i].mode == CAR_PARKED) respawn_offscreen(&cars[i]);
    }
}

// Nearest tile of the given type to (tx, ty) (search over expanding squares)
static bool find_near(int tx, int ty, int kind, int *ox, int *oy)
{
    for (int r = 0; r < 40; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                if (tile_kind(tx + dx, ty + dy) == kind) { *ox = (tx + dx) & (MAP - 1); *oy = (ty + dy) & (MAP - 1); return true; }
            }
    return false;
}

// Sidewalk next to a lane (not an intersection): player on the sidewalk, car on the lane
static void find_curb(int tx, int ty, float *px, float *py, float *cx, float *cy, int *cdir)
{
    static const int8_t dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int r = 0; r < 40; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int sx = tx + dx, sy = ty + dy;
                if (tile_kind(sx, sy) != T_SIDEWALK) continue;
                for (int k = 0; k < 4; k++) {
                    int rx = sx + dx4[k], ry = sy + dy4[k];
                    int d = lane_dir(lanes[MI(rx, ry)]);
                    if (d < 0) continue;
                    *px = (sx & (MAP - 1)) * 16 + 8; *py = (sy & (MAP - 1)) * 16 + 8;
                    *cx = (rx & (MAP - 1)) * 16 + 8; *cy = (ry & (MAP - 1)) * 16 + 8;
                    *cdir = d;
                    return;
                }
            }
}

static void respawn_player(bool hospital)
{
    if (pcar >= 0) { cars[pcar].mode = CAR_PARKED; cars[pcar].speed = 0; pcar = -1; }
    // hospital / station — on sidewalks in different blocks
    int tx, ty;
    if (hospital) find_near(LM_AT[LM_HOSPITAL][0], LM_AT[LM_HOSPITAL][1], T_SIDEWALK, &tx, &ty);      // hospital — residential district
    else          find_near(LM_AT[LM_POLICE][0], LM_AT[LM_POLICE][1], T_SIDEWALK, &tx, &ty);      // station — east downtown
    plx = tx * 16 + 8;
    ply = ty * 16 + 8;
    php = 100;
    armor = 0;
    for (int i = 1; i < W_COUNT; i++) ammo[i] = 0;
    ammo[W_PISTOL] = 24;
    weapon = W_PISTOL;
    invuln_t = 2.0f;
    arrest_t = 0;
    camX = plx; camY = ply;
    reset_police();
    save_req = true;
}

static void world_setup(void);
static bool world_missing = false;
static bool ov_ok = false;          // pause overview map is built
void game_init(uint32_t seed, const game_save_t *save)
{
    rng_state = seed ? seed : 12345;
    init_palette();
    world_setup();

    plus_init();
    fauna_reset();
    memset(navd, 255, NAVW * NAVW);
    // start: downtown, a car stands by the sidewalk
    float scx, scy;
    int sdir;
    find_curb(LM_AT[LM_START][0], LM_AT[LM_START][1], &plx, &ply, &scx, &scy, &sdir);
    camX = plx; camY = ply;
    for (int i = 0; i < NCARS; i++) {
        cars[i].police = (i >= NCIVCARS && i < POLICE_END);
        cars[i].col = cars[i].police ? 8 : i % 8;
        if (i < NCARS / 2) respawn_offscreen(&cars[i]); else spawn_ai(&cars[i]);
    }
    for (int i = FIRE_IDX; i < NCARS; i++) {
        car_reset(&cars[i]);
        cars[i].mode = CAR_OFF; cars[i].x = 40; cars[i].y = 40 + i * 30; cars[i].speed = 0;
        cars[i].col = (i == FIRE_IDX) ? 10 : 11; cars[i].police = 0;
    }
    cars[0].x = scx; cars[0].y = scy;
    cars[0].ang = sdir * PIf / 2;
    cars[0].speed = 0;
    cars[0].mode = CAR_PARKED;
    // police car parked nearby — for a spectacular carjacking
    {
        float px2, py2, cx2, cy2;
        int d2;
        find_curb(LM_AT[LM_START][0] + 9, LM_AT[LM_START][1] + 2, &px2, &py2, &cx2, &cy2, &d2);
        cars[NCIVCARS].mode = CAR_PARKED;
        cars[NCIVCARS].x = cx2; cars[NCIVCARS].y = cy2;
        cars[NCIVCARS].ang = d2 * PIf / 2; cars[NCIVCARS].speed = 0;
    }

    memset(act, 0, sizeof(act));
    for (int i = 0; i < NCIV_TARGET; i++) spawn_civ(&act[i], false);
    memset(bul, 0, sizeof(bul));
    memset(fx, 0, sizeof(fx));
    init_boats();

    for (int i = 0; i < NPICK; i++) {
        pick[i].active = 1;
        pick[i].timer = 0;
        if (i < 8) { random_tile_point(T_SIDEWALK, &pick[i].x, &pick[i].y); pick[i].kind = i % PK_COUNT; }
        else if (i < 10) { random_tile_point(T_PLAZA, &pick[i].x, &pick[i].y); pick[i].kind = PK_FRENZY; }
        else { random_tile_point(rnd() & 1 ? T_PARK : T_PLAZA, &pick[i].x, &pick[i].y); pick[i].kind = PK_PACKAGE; }
    }
    packages = 0; frenzy_t = 0;
    {
        int tx, ty;
        find_near(LM_AT[LM_SPRAY][0], LM_AT[LM_SPRAY][1], T_PLAZA, &tx, &ty);       // Pay'n'Spray — in the industrial zone
        spray_x = tx * 16 + 8; spray_y = ty * 16 + 8;
    }
    // weapon near the start
    {
        int tx, ty, sx = ((int)plx) >> 4, sy = ((int)ply) >> 4;
        if (find_near(sx + 4, sy + 1, T_SIDEWALK, &tx, &ty)) { pick[1].x = tx * 16 + 8; pick[1].y = ty * 16 + 8; }
        pick[1].kind = PK_UZI;
        if (find_near(sx - 4, sy - 1, T_SIDEWALK, &tx, &ty)) { pick[2].x = tx * 16 + 8; pick[2].y = ty * 16 + 8; }
        pick[2].kind = PK_SHOTGUN;
    }

    for (int i = 0; i < W_COUNT; i++) ammo[i] = 0;
    ammo[W_PISTOL] = 48;
    weapon = W_PISTOL;
    php = 100;

    super_won = false;
    if (save) {
        money = save->money; kills = save->kills;
        deliveries = save->deliveries; best_wanted = save->best_wanted;
        super_won = (save->flags & 1) != 0;
        plus_load(save->flags);
    }
    if (super_won) park_car_near(SUPER_IDX, pois[POI_CASINO].dx, pois[POI_CASINO].dy, 11);
    mission_start(M_DELIVER);
    msg_t = 0;
    gstate = G_TITLE;
    state_t = 0;
}

// ================= Main update =================
static void update_world(const game_input_t *in, float dt)
{
    // player
    if (in_heli) plus_heli_control(in, dt);
    else if (pcar >= 0) {
        update_player_car(in, dt);
        plx = cars[pcar].x;
        ply = cars[pcar].y;
        pang = cars[pcar].ang;
    } else {
        float sp = (in->held & BTN_RUN) ? RUN_SPEED : WALK_SPEED;
        float mx = in->mx, my = in->my;
        float m = sqrtf(mx * mx + my * my);
        if (m > 1) { mx /= m; my /= m; m = 1; }
        move_slide(&plx, &ply, mx * sp, my * sp, dt);
        p_moving = m > 0.2f;
        if (m > 0.2f) pang = atan2f(my, mx);
        // hit by an AI car (they're slow, but still)
        for (int i = 0; i < NCARS; i++) {
            car_t *c = &cars[i];
            if (c->mode == CAR_WRECK || fabsf(c->speed) < 40 || c->mode == CAR_COP) continue;
            if (dist2w(c->x, c->y, plx, ply) < 10 * 10) hurt_player(1);
        }
    }
    if (in->aim) pang = atan2f(in->ay, in->ax);

    fire_cd -= dt;
    if (flash_t > 0) flash_t -= dt;
    if (hurt_t > 0) hurt_t -= dt;
    if (invuln_t > 0) invuln_t -= dt;

    bool fire = (in->held & BTN_FIRE) != 0;
    if (in->aim && sqrtf(in->ax * in->ax + in->ay * in->ay) > 0.8f) fire = true;   // twin-stick
    if (fire) {
        if (weapon == W_FIST && !(pressed & BTN_FIRE) && !in->aim) {}
        else player_fire();
    }
    if (poi_near >= 0) {
        if (pressed & BTN_ENTER) shop_buy();
        if (pressed & BTN_WEAPON) { shop_item++; if (shop_item >= 4 || !SHOP_ITEMS[poi_near][shop_item].name) shop_item = 0; }
    } else {
        if (pressed & BTN_ENTER) action();
        if (pressed & BTN_WEAPON) next_weapon(1);
    }
    if (pressed & BTN_WEAPON_PREV) { if (pcar >= 0) plus_radio_next(); else next_weapon(-1); }

    // cars
    for (int i = 0; i < NCARS; i++) {
        if (i == pcar) continue;
        car_t *c = &cars[i];
        if (c->hp <= 0 && c->mode != CAR_WRECK) {
            c->x = wrapf(c->x + cosf(c->ang) * c->speed * dt);
            c->y = wrapf(c->y + sinf(c->ang) * c->speed * dt);
            continue;
        }
        if (c->mode == CAR_AI) update_ai(i, dt);
        else if (c->mode == CAR_COP) update_cop_car(i, dt);
        else if (c->mode == CAR_PARKED && i < POLICE_END && !on_screen(c->x, c->y, 120) && i != 0 && !(mtype == M_STEAL && i == mtarget) &&
                 dist2w(c->x, c->y, plx, ply) > 500 * 500) respawn_offscreen(c);
    }
    update_cars_damage(dt);

    // run-overs: the player's car and police crush pedestrians
    for (int i = 0; i < NCARS; i++) {
        car_t *c = &cars[i];
        if (fabsf(c->speed) < 45 || (i != pcar && c->mode != CAR_COP)) continue;
        for (int j = 0; j < NACT; j++) {
            actor_t *a = &act[j];
            if (!a->kind || a->state == S_DEAD) continue;
            if (a->kind == A_COP && i != pcar) continue;
            if (dist2w(c->x, c->y, a->x, a->y) < 10 * 10) { knock_ang = c->ang; knock_sp = fabsf(c->speed) * 0.8f; kill_actor(j, i == pcar); if (i == pcar) plus_shake(2); }
        }
    }

    // people
    arrest_touch = arrest_car = false;
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (!a->kind) continue;
        if (a->state == S_DEAD) {
            plus_dead_tick(a, dt);
            a->timer -= dt;
            if (a->timer <= 0 && !on_screen(a->x, a->y, 20)) a->kind = A_NONE;
            continue;
        }
        if (a->kind == A_GANG) { update_gang(i, dt); continue; }
        if (a->flags & AF_ROBBER) {                  // the robber is controlled by the event
            if (a->state == S_IDLE && !(a->flags & AF_CUFFED)) { a->timer -= dt; }
            continue;
        }
        if (a->kind == A_CIV) {
            update_civ(a, dt);
            if (dist2w(a->x, a->y, plx, ply) > 600.0f * 600.0f && !(a->flags & 1)) spawn_civ(a, true);   // keep the crowd nearby
        } else update_cop(a, dt);
        if (gstate != G_PLAY) return;
    }
    // arrest meter: rises while a cop holds, decays 1.5x faster once you break free
    if (arrest_touch && invuln_t <= 0) {
        arrest_t += dt;
        if (arrest_t >= ARREST_TIME) {
            if (arrest_car) game_message("PULLED OUT!", 1.5f);
            busted();
            return;
        }
    } else {
        arrest_t -= dt * 1.5f;
        if (arrest_t < 0) arrest_t = 0;
    }

    // maintain the population
    if (count_kind(A_CIV, false) < NCIV_TARGET) {
        int j = free_actor();
        if (j >= 0 && act[j].kind == A_NONE) spawn_civ(&act[j], true);
    }

    update_police(dt);
    update_bullets(dt);
    update_pickups(dt);

    can_act = (pcar >= 0) || (nearest_car() >= 0);
    update_mission();
    update_activities(dt);
}

static void update_camera(float dt)
{
    float tlx = 0, tly = 0;
    plus_cam_lead(&tlx, &tly);
    if (pcar >= 0) {
        tlx = cosf(cars[pcar].ang) * cars[pcar].speed * 0.35f;
        tly = sinf(cars[pcar].ang) * cars[pcar].speed * 0.35f;
    }
    float k = dt * (cinema ? 1.5f : 4);
    if (k > 1) k = 1;
    lead_x += (tlx - lead_x) * k;
    lead_y += (tly - lead_y) * k;
    camX = wrapf(plx + lead_x);
    camY = wrapf(ply + lead_y);
}

void game_update(const game_input_t *in, float dt)
{
    cur_in = *in;
    pressed = in->held & ~prev_held;
    prev_held = in->held;
    g_time += dt;
    state_t += dt;
    if (msg_t > 0) msg_t -= dt;
    if (pressed & BTN_SHOT) shot_req = true;
    if (replay) { replay_tick(dt); return; }

    switch (gstate) {
    case G_TITLE:
        camX = wrapf(camX + 25 * dt);
        camY = wrapf(camY + 12 * dt);
        if (pressed & (BTN_FIRE | BTN_START | BTN_ENTER)) {
            gstate = G_PLAY;
            state_t = 0;
            camX = plx; camY = ply;
            game_message("GET IN THE CAR", 2.5f);
        }
        // the city lives on in the background
        update_world_extras(dt);
        for (int i = 0; i < NCARS; i++) if (cars[i].mode == CAR_AI) update_ai(i, dt);
        for (int i = 0; i < NACT; i++) if (act[i].kind == A_CIV) update_civ(&act[i], dt);
        return;
    case G_PAUSE:
        if (pressed & BTN_START) gstate = G_PLAY;
        else plus_pause_input();
        return;
    case G_WASTED:
    case G_BUSTED: {
        // the world keeps living, the player is frozen
        game_input_t none = {0};
        prev_held = in->held;
        if (pcar >= 0) cars[pcar].speed *= 0.95f;
        update_world_extras(dt);
        for (int i = 0; i < NCARS; i++) if (cars[i].mode == CAR_AI) update_ai(i, dt);
        for (int i = 0; i < NACT; i++) if (act[i].kind == A_CIV) update_civ(&act[i], dt);
        update_bullets(dt);
        (void)none;
        if (state_t > 3.5f) {
            bool wasted = (gstate == G_WASTED);
            if (wasted) money -= money / 10;
            else money -= (money < 100) ? money : 100;
            plus_on_respawn();
            respawn_player(wasted);
            goosed = false;
            gstate = G_PLAY;
            state_t = 0;
            game_message(wasted ? "HOSPITAL" : "POLICE STATION", 2);
        }
        update_camera(dt);
        return;
    }
    case G_PLAY:
        if (pressed & BTN_START) { gstate = G_PAUSE; ov_ok = false; return; }
        update_world_extras(dt);
        update_world(in, dt);
        if (gstate == G_PLAY) plus_update(dt);
        update_camera(dt);
        plus_record(dt);
        return;
    }
}

// ================= Drawing =================
// Current screen strip
static uint16_t *g_buf;
static int g_y0, g_y1;

static inline HOT void put_px(int x, int y, uint16_t c)
{
    if ((unsigned)x < SW && y >= g_y0 && y < g_y1) g_buf[(y - g_y0) * SW + x] = c;
}

static HOT void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int xa = x < 0 ? 0 : x, xb = (x + w > SW) ? SW : x + w;
    int ya = y < g_y0 ? g_y0 : y, yb = (y + h > g_y1) ? g_y1 : y + h;
    for (int yy = ya; yy < yb; yy++) {
        uint16_t *d = g_buf + (yy - g_y0) * SW;
        for (int xx = xa; xx < xb; xx++) d[xx] = c;
    }
}

// Darken a rectangle (semi-transparent backdrop under text)
static HOT void shade_rect(int x, int y, int w, int h)
{
    int xa = x < 0 ? 0 : x, xb = (x + w > SW) ? SW : x + w;
    int ya = y < g_y0 ? g_y0 : y, yb = (y + h > g_y1) ? g_y1 : y + h;
    for (int yy = ya; yy < yb; yy++) {
        uint16_t *d = g_buf + (yy - g_y0) * SW;
        for (int xx = xa; xx < xb; xx++) {
            uint16_t v = d[xx];
            v = (uint16_t)((v >> 8) | (v << 8));
            v = (v >> 1) & 0x7BEF;               // brightness / 2
            d[xx] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

static HOT void fill_circle(float cx, float cy, float r, uint16_t c)
{
    int ya = (int)floorf(cy - r), yb = (int)ceilf(cy + r);
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int xa = (int)floorf(cx - r), xb = (int)ceilf(cx + r);
    if (xa < 0) xa = 0;
    if (xb > SW - 1) xb = SW - 1;
    float r2 = r * r;
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - cy;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - cx;
            if (dx * dx + dy * dy <= r2) d[x] = c;
        }
    }
}

static HOT void ring(float cx, float cy, float r, float th, uint16_t c)
{
    int ya = (int)floorf(cy - r), yb = (int)ceilf(cy + r);
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int xa = (int)floorf(cx - r), xb = (int)ceilf(cx + r);
    if (xa < 0) xa = 0;
    if (xb > SW - 1) xb = SW - 1;
    float ro = r * r, ri = (r - th) * (r - th);
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - cy;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 <= ro && d2 >= ri) g_buf[(y - g_y0) * SW + x] = c;
        }
    }
}

// 3x5 font
static const char FONT_CHARS[] = "0123456789$ABCDEFGHIJKLMNOPQRSTUVWXYZ!:-/+.*x";
static const uint16_t FONT[] = {
    0x7B6F, 0x2C97, 0x73E7, 0x72CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF, 0x3C9E, 0x2BED,
    0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED,
    0x6B6D, 0x2B6A, 0x6BA4, 0x2B73, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92,
    0x72A7, 0x2482, 0x0410, 0x01C0, 0x12A4, 0x05D0, 0x0002, 0x55D5, 0x0AA8,
};

static int text_w(const char *s, int sc) { return (int)strlen(s) * 4 * sc - sc; }

static void draw_text(int x, int y, const char *s, int sc, uint16_t c)
{
    if (y > g_y1 || y + 5 * sc < g_y0) return;
    for (; *s; s++, x += 4 * sc) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'w') ch -= 32;
        const char *p = strchr(FONT_CHARS, ch);
        if (!p || ch == 0) continue;
        uint16_t g = FONT[p - FONT_CHARS];
        for (int r = 0; r < 5; r++)
            for (int col = 0; col < 3; col++)
                if (g & (1 << (14 - (r * 3 + col))))
                    fill_rect(x + col * sc, y + r * sc, sc, sc, c);
    }
}

static void draw_text_sh(int x, int y, const char *s, int sc, uint16_t c)
{
    draw_text(x + (sc > 1 ? sc / 2 + 1 : 1), y + (sc > 1 ? sc / 2 + 1 : 1), s, sc, C_BLACK);
    draw_text(x, y, s, sc, c);
}

static void draw_text_c(int y, const char *s, int sc, uint16_t c)
{
    draw_text_sh(SW / 2 - text_w(s, sc) / 2, y, s, sc, c);
}

static inline float scr_x(float wx) { return SW / 2 + wrapd(wx - (float)cam_ix); }
static inline float scr_y(float wy) { return SH / 2 + wrapd(wy - (float)cam_iy); }

// Background from the atlas: the main "hot" loop
static HOT void render_bg(void)
{
    int wx0 = (cam_ix - SW / 2) & WMASK;
    for (int y = g_y0; y < g_y1; y++) {
        int wy = (cam_iy + y - SH / 2) & WMASK;
        int ty = wy >> 4, ly = wy & 15;
        const uint16_t *row = map_v + (ty << MSH);
        const uint8_t *srow = shore_row[ty] ? bm_shore + (ty << (MSH - 3)) : NULL;   // NULL — no shore in this tile row on screen
        uint16_t *d = g_buf + (y - g_y0) * SW;
        int wx = wx0, n = SW;
        while (n > 0) {
            int tx = wx >> 4, lx = wx & 15;
            int run = 16 - lx;
            if (run > n) run = n;
            uint16_t v = row[tx];
            const uint8_t *s = tex_ptr[MV_TEX(v)] + ly * 16 + lx;
            const uint16_t *p = pal[MV_PAL(v)];
            if (!srow || !((srow[tx >> 3] >> (tx & 7)) & 1)) {
                for (int i = 0; i < run; i++) d[i] = p[s[i]];
            } else {      // shore mask (cache): 4 bits per pixel, 0 — tile pixel
                const uint8_t *m = shore_slot(tx, ty) + ly * 8;
                for (int i = 0; i < run; i++) {
                    int px = lx + i;
                    int c = (px & 1) ? (m[px >> 1] >> 4) : (m[px >> 1] & 15);
                    d[i] = c ? shore_col[c] : p[s[i]];
                }
            }
            d += run;
            n -= run;
            wx = (wx + run) & WMASK;
        }
    }
}

// Darken a pixel by ~40% (shadows)
static inline uint16_t darken(uint16_t v)
{
    v = (uint16_t)((v >> 8) | (v << 8));
    v = ((v >> 1) & 0x7BEF) + ((v >> 3) & 0x18E3);
    return (uint16_t)((v >> 8) | (v << 8));
}

static HOT void shadow_circle(float cx, float cy, float r)
{
    int ya = (int)floorf(cy - r), yb = (int)ceilf(cy + r);
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int xa = (int)floorf(cx - r), xb = (int)ceilf(cx + r);
    if (xa < 0) xa = 0;
    if (xb > SW - 1) xb = SW - 1;
    float r2 = r * r;
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - cy;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - cx;
            if (dx * dx + dy * dy <= r2) d[x] = darken(d[x]);
        }
    }
}

// ================= GTA.PAK graphics pack (SD -> PSRAM) =================
typedef struct { uint16_t w, h, n; const uint8_t *px; } spr_t;
static spr_t spr_cars, spr_people, spr_trees[6], spr_slot, spr_logo;
static bool pak_ok = false;

bool game_set_pak(const uint8_t *data, size_t len)
{
    if (!data || len < 12 || memcmp(data, "GPAK", 4) != 0) return false;
    uint32_t n;
    memcpy(&n, data + 8, 4);
    if (12 + n * 20 > len) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = data + 12 + i * 20;
        char name[9] = {0};
        memcpy(name, e, 8);
        uint16_t w, h, fr;
        uint32_t off;
        memcpy(&w, e + 8, 2); memcpy(&h, e + 10, 2); memcpy(&fr, e + 12, 2); memcpy(&off, e + 16, 4);
        if ((size_t)off + (size_t)w * h * 4 * fr > len) return false;
        spr_t sp = { w, h, fr, data + off };
        if (!strcmp(name, "CARS")) spr_cars = sp;
        else if (!strcmp(name, "PEOPLE")) spr_people = sp;
        else if (!strcmp(name, "SLOT")) spr_slot = sp;
        else if (!strcmp(name, "LOGO")) spr_logo = sp;
        else if (!strncmp(name, "TREE", 4) && name[4] >= '0' && name[4] <= '5') spr_trees[name[4] - '0'] = sp;
    }
    pak_ok = spr_cars.px && spr_people.px && spr_cars.n >= 14 * 32 && spr_people.n >= 12 * 64;
    printf("pak: %s (%u bytes)\n", pak_ok ? "loaded" : "incomplete", (unsigned)len);
    return pak_ok;
}

static inline uint16_t blend50(uint16_t a, uint16_t b)
{
    a = (uint16_t)((a >> 8) | (a << 8)); b = (uint16_t)((b >> 8) | (b << 8));
    uint16_t c = ((a >> 1) & 0x7BEF) + ((b >> 1) & 0x7BEF);
    return (uint16_t)((c >> 8) | (c << 8));
}

// Draw an RGBA sprite frame: 3-level alpha (none / 50% / full); tint — darken at night;
// shadow — darken the ground under the sprite's silhouette instead of the sprite
static HOT void blit(const spr_t *s, int frame, int x0, int y0, bool tint, bool shadow)
{
    if (!s->px || frame < 0 || frame >= s->n) return;
    int ya = y0 < g_y0 ? g_y0 : y0, yb = (y0 + s->h > g_y1) ? g_y1 : y0 + s->h;
    int xa = x0 < 0 ? 0 : x0, xb = (x0 + s->w > SW) ? SW : x0 + s->w;
    if (ya >= yb || xa >= xb) return;
    const uint8_t *base = s->px + (size_t)frame * s->w * s->h * 4;
    for (int y = ya; y < yb; y++) {
        const uint8_t *p = base + ((y - y0) * s->w + (xa - x0)) * 4;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = xa; x < xb; x++, p += 4) {
            uint8_t a = p[3];
            if (a < 48) continue;
            if (shadow) { if (a >= 128) d[x] = darken(d[x]); continue; }
            uint16_t c;
            if (tint) c = lut_r[p[0]] | lut_g[p[1]] | lut_b[p[2]];
            else c = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
            c = (uint16_t)((c >> 8) | (c << 8));
            d[x] = (a >= 200) ? c : blend50(d[x], c);
        }
    }
}

static HOT void draw_car_sprite(const car_t *c)
{
    float sx = scr_x(c->x), sy = scr_y(c->y);
    if (sy + 20 < g_y0 || sy - 20 >= g_y1 || sx < -20 || sx > SW + 20) return;
    int a = ((int)lroundf(c->ang / (2 * PIf) * 32)) & 31;
    int kind = (c->mode == CAR_WRECK) ? 13 : c->col;
    int fr = kind * 32 + a;
    int x0 = (int)sx - 16, y0 = (int)sy - 16;
    blit(&spr_cars, fr, x0 + 2, y0 + 3, false, true);
    blit(&spr_cars, fr, x0, y0, true, false);
    bool flip = ((int)(g_time * 6)) & 1;
    float ca = cosf(c->ang), sa = sinf(c->ang);
    if ((c->police && c->mode == CAR_COP) || (c->col == 10 && c->mode == CAR_FIRE)) {
        float ux = sx - ca * 1, uy = sy - sa * 1;
        fill_rect((int)(ux + sa * 2.5f) - 1, (int)(uy - ca * 2.5f) - 1, 2, 2, flip ? C_RED : C_DARK);
        fill_rect((int)(ux - sa * 2.5f) - 1, (int)(uy + ca * 2.5f) - 1, 2, 2, flip ? C_DARK : C_BLUE);
    } else if (c->police && c->mode != CAR_WRECK) {
        fill_rect((int)(sx - ca + sa * 2.5f) - 1, (int)(sy - sa - ca * 2.5f) - 1, 2, 2, C_RED);
        fill_rect((int)(sx - ca - sa * 2.5f) - 1, (int)(sy - sa + ca * 2.5f) - 1, 2, 2, C_BLUE);
    }
    if (c->mode != CAR_WRECK && c->hp <= 0) {
        fill_circle(sx, sy - 2, 5 + (flip ? 2 : 0), C_ORANGE);
        fill_circle(sx + 2, sy - 3, 3, C_YELLOW);
    }
}

static void draw_taxi_sign(const car_t *c)
{
    if (c->col != 2 || c->police || c->mode == CAR_WRECK) return;
    float sx = scr_x(c->x), sy = scr_y(c->y);
    float ca = cosf(c->ang), sa = sinf(c->ang);
    float ux = sx - ca * 1.5f, uy = sy - sa * 1.5f;
    for (int k = -2; k <= 2; k++) {
        int x = (int)(ux - sa * k), y = (int)(uy + ca * k);
        put_px(x, y, (k & 1) ? C_DARK : C_WHITE);
        put_px(x + (int)roundf(ca), y + (int)roundf(sa), (k & 1) ? C_WHITE : C_DARK);
    }
}

static HOT void draw_car(const car_t *c, int idx)
{
    if (c->mode == CAR_OFF) return;
    if (pak_ok) { draw_car_sprite(c); draw_taxi_sign(c); return; }
    const int R = 15;
    float sx = scr_x(c->x), sy = scr_y(c->y);
    if (sy + R + 3 < g_y0 || sy - R >= g_y1 || sx < -R || sx > SW + R + 3) return;
    int ya = (int)sy - R, yb = (int)sy + R + 3;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int xa = (int)sx - R, xb = (int)sx + R + 3;
    if (xa < 0) xa = 0;
    if (xb > SW - 1) xb = SW - 1;

    bool wreck = c->mode == CAR_WRECK;
    bool burning = !wreck && c->hp <= 0;
    bool siren = c->police && c->mode == CAR_COP;
    bool flip = ((int)(g_time * 6)) & 1;
    uint16_t body, side, hl;
    if (c->col == 8)       { body = C_POL_B; side = C_POL_R; hl = C_POL_H; }
    else if (c->col == 9)  { body = C_GOLD; side = C_GOLD_R; hl = C_GOLD_H; }
    else if (c->col == 10) { body = C_FIRE_B; side = C_FIRE_S; hl = C_POL_H; }
    else if (c->col == 11) { body = C_VX_B; side = C_VX_S; hl = C_VX_H; }
    else if (c->col == 12) { body = C_SP_B; side = C_SP_S; hl = C_SP_H; }
    else                  { body = car_body[c->col]; side = car_roof[c->col]; hl = car_hl[c->col]; }
    if (c->col == 8) side = body;      // police have white sides, only the roof is dark
    if (c->mode == CAR_OFF) return;
    if (wreck) { body = C_WRECK; side = C_WRECK_D; hl = C_WRECK; }
    float ca = cosf(c->ang), sa = sinf(c->ang);

    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - sy;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - sx;
            float u = dx * ca + dy * sa;
            float v = -dx * sa + dy * ca;
            float av = fabsf(v), au = fabsf(u);
            if (au > 12 || av > 7) {
                // car shadow (offset down-right)
                float u2 = (dx - 2) * ca + (dy - 3) * sa, v2 = -(dx - 2) * sa + (dy - 3) * ca;
                if (fabsf(u2) <= 12 && fabsf(v2) <= 6.5f) d[x] = darken(d[x]);
                continue;
            }
            if (av > 6) {
                // wheels stick out at the sides
                if (au >= 5 && au <= 9) d[x] = C_DARK;
                else {
                    float u2 = (dx - 2) * ca + (dy - 3) * sa, v2 = -(dx - 2) * sa + (dy - 3) * ca;
                    if (fabsf(u2) <= 12 && fabsf(v2) <= 6.5f) d[x] = darken(d[x]);
                }
                continue;
            }
            uint16_t col = (av > 4.8f || au > 11) ? side : body;      // sides darker — volume
            if (!wreck) {
                if (u > 9.5f && av > 2.5f && av < 5.5f)        col = C_HEAD;
                else if (u < -10.5f && av > 2.5f && av < 5.5f) col = C_TAIL;
                else if (u > 2 && u < 6 && av < 4.8f)          col = (v < -2) ? hl : C_GLASS;     // windshield + glare
                else if (u > -9 && u < -6 && av < 4.8f)        col = C_GLASS;
                else if (u >= -6 && u <= 2 && av < 4.3f) {
                    col = (v < -2.5f && u > -4) ? hl : body;                                     // roof with glare
                    if (c->police && u >= -3 && u <= -1) {
                        if (v < 0) col = siren ? (flip ? C_RED : C_DARK) : C_RED;
                        else       col = siren ? (flip ? C_DARK : C_BLUE) : C_BLUE;
                    }
                } else if (c->police && u > 6 && u <= 9.5f)    col = C_DARK;
                else if (v < -3.6f && av <= 4.8f)              col = hl;                         // glare along the side
                if (c->col == 11 && av < 1.3f && !(u > 2 && u < 6) && !(u > -9 && u < -6)) col = C_GOLD;     // racing stripe
                if (c->col == 10 && u > -9 && u < 6 && av < 2.5f) col = (((int)(u + 20)) & 2) ? C_POL_H : C_DARK;   // ladder
                if (c->col == 10 && u >= 6 && u <= 8 && c->mode == CAR_FIRE) col = flip ? C_RED : C_BLUE;   // light bar
            } else if (((x * 7 + y * 3) & 7) == 0) col = C_SMOKE;
            if (burning && (((x + y + (int)(g_time * 20)) & 3) == 0)) col = flip ? C_ORANGE : C_YELLOW;
            d[x] = col;
        }
    }
    (void)idx;
    if (burning) fill_circle(sx, sy - 3, 4 + (flip ? 2 : 0), C_ORANGE);
    draw_taxi_sign(c);
}

// Person from above: shadow, walking feet, shoulders, head
static void draw_person(float x, float y, float ang, uint16_t shirt, uint16_t head, bool gun, bool moving, float seed, int ci)
{
    if (y + 8 < g_y0 || y - 7 >= g_y1 || x < -7 || x > SW + 8) return;
    if (pak_ok) {
        shadow_circle(x + 1.5f, y + 2.2f, 4.0f);
        int a = ((int)lroundf(ang / (2 * PIf) * 16)) & 15;
        int f = moving ? ((int)(g_time * 9 + seed)) & 3 : 0;
        blit(&spr_people, ci * 64 + a * 4 + f, (int)x - 8, (int)y - 8, true, false);
        if (gun) fill_rect((int)(x + cosf(ang) * 6) - 1, (int)(y + sinf(ang) * 6) - 1, 2, 2, C_BLACK);
        (void)shirt; (void)head;
        return;
    }
    float ca = cosf(ang), sa = sinf(ang);
    shadow_circle(x + 1.5f, y + 2.2f, 4.2f);
    float st = moving ? sinf(g_time * 13 + seed) : 0;
    float px = -sa, py = ca;
    fill_circle(x + ca * 3.8f * st + px * 1.9f, y + sa * 3.8f * st + py * 1.9f, 1.4f, C_DARK);
    fill_circle(x - ca * 3.8f * st - px * 1.9f, y - sa * 3.8f * st - py * 1.9f, 1.4f, C_DARK);
    fill_circle(x, y, 3.7f, shirt);
    if (gun) fill_rect((int)(x + ca * 5) - 1, (int)(y + sa * 5) - 1, 2, 2, C_BLACK);
    fill_circle(x + ca * 0.8f, y + sa * 0.8f, 2.1f, head);
}

// ================= Volumetric buildings (GTA1-style perspective) =================
// The roof is the same atlas image, lifted above the base: its projection is the base
// stretched from the screen center by a factor of (1 + hk). Between base and roof — walls with windows.
#define MAX_VIS 120
typedef struct {
    float a0, a1, b0, b1, hk, inv;
    int x0, y0, x1, y1;          // on-screen frame
    uint16_t lot;
    float key;
} vis_t;
static vis_t vis[MAX_VIS];
static int nvis = 0;

static void build_visible_lots(void)
{
    nvis = 0;
    const float cx = SW / 2.0f, cy = SH / 2.0f;
    // lots are sorted by y0: look only at rows near the screen (tall buildings "lean" up to ~6 tiles)
    int ra = ((cam_iy - SH / 2 - 100) >> 4) - 16, rb = (cam_iy + SH / 2 + 100) >> 4;
    for (int rr = ra; rr <= rb; rr++)
    for (int i = lot_row[rr & (MAP - 1)], ie = lot_row[(rr & (MAP - 1)) + 1]; i < ie; i++) {
        const lot_t *l = &g_lots[i];
        float bx0 = cx + wrapd(l->x0 * 16.0f - cam_ix), by0 = cy + wrapd(l->y0 * 16.0f - cam_iy);
        float bx1 = bx0 + (l->x1 - l->x0 + 1) * 16, by1 = by0 + (l->y1 - l->y0 + 1) * 16;
        if (bx1 < -90 || bx0 > SW + 90 || by1 < -90 || by0 > SH + 90) continue;
        float hk = l->h10 * 0.0055f;
        vis_t v;
        v.a0 = bx0 - cx; v.a1 = bx1 - cx; v.b0 = by0 - cy; v.b1 = by1 - cy;
        v.hk = hk; v.inv = 1.0f / (1.0f + hk);
        float rx0 = cx + v.a0 * (1 + hk), rx1 = cx + v.a1 * (1 + hk), ry0 = cy + v.b0 * (1 + hk), ry1 = cy + v.b1 * (1 + hk);
        v.x0 = (int)floorf(fminf(bx0, rx0)); v.x1 = (int)ceilf(fmaxf(bx1, rx1));
        v.y0 = (int)floorf(fminf(by0, ry0)); v.y1 = (int)ceilf(fmaxf(by1, ry1));
        if (v.x1 < 0 || v.x0 >= SW || v.y1 < 0 || v.y0 >= SH) continue;
        if (v.x0 < 0) v.x0 = 0;
        if (v.x1 > SW - 1) v.x1 = SW - 1;
        v.lot = i;
        float mx = (v.a0 + v.a1) * 0.5f, my = (v.b0 + v.b1) * 0.5f;
        v.key = mx * mx + my * my;
        // insert in descending key order: farthest from center drawn first
        int j = nvis < MAX_VIS ? nvis++ : MAX_VIS - 1;
        while (j > 0 && vis[j - 1].key < v.key) { vis[j] = vis[j - 1]; j--; }
        vis[j] = v;
    }
}

enum { F_ROOF = 0, F_N, F_S, F_W, F_E };

static HOT void draw_building(const vis_t *v)
{
    int ya = v->y0 < g_y0 ? g_y0 : v->y0, yb = v->y1 >= g_y1 ? g_y1 - 1 : v->y1;
    if (ya > yb) return;
    const lot_t *l = &g_lots[v->lot];
    const uint16_t *pl = pal[l->pal];
    const float hk = v->hk;
    const float ka0 = v->a0 * hk, ka1 = v->a1 * hk, kb0 = v->b0 * hk, kb1 = v->b1 * hk;
    const float ika0 = ka0 != 0 ? 1.0f / ka0 : 0, ika1 = ka1 != 0 ? 1.0f / ka1 : 0;
    const float ikb0 = kb0 != 0 ? 1.0f / kb0 : 0, ikb1 = kb1 != 0 ? 1.0f / kb1 : 0;
    const int lx0 = l->x0 * 16, lx1 = l->x1 * 16 + 15, ly0 = l->y0 * 16, ly1 = l->y1 * 16 + 15;
    const float floors = l->h10 / 7.0f;
    const int style = l->style;

    for (int y = ya; y <= yb; y++) {
        float py = y + 0.5f - SH / 2.0f;
        float tlo = 0, thi = 1;
        int fy = F_ROOF;
        // top edge of base b0: kb0*t <= py - b0
        float r = py - v->b0;
        if (kb0 > 0) { float t = r * ikb0; if (t < thi) { thi = t; fy = F_N; } }
        else if (kb0 < 0) { float t = r * ikb0; if (t > tlo) tlo = t; }
        else if (r < 0) continue;
        // bottom edge b1: kb1*t >= py - b1
        r = py - v->b1;
        if (kb1 > 0) { float t = r * ikb1; if (t > tlo) tlo = t; }
        else if (kb1 < 0) { float t = r * ikb1; if (t < thi) { thi = t; fy = F_S; } }
        else if (r > 0) continue;
        if (tlo > thi) continue;

        uint16_t *d = g_buf + (y - g_y0) * SW;
        int wy_roof = cam_iy + (int)floorf(py * v->inv);
        if (wy_roof < ly0) wy_roof = ly0;
        if (wy_roof > ly1) wy_roof = ly1;
        // occupied row segment: union of cross-section projections for t from tlo to thi
        float l0 = v->a0 * (1 + hk * tlo), l1 = v->a0 * (1 + hk * thi);
        float u0 = v->a1 * (1 + hk * tlo), u1 = v->a1 * (1 + hk * thi);
        int xs = (int)floorf(SW / 2.0f + (l0 < l1 ? l0 : l1)) - 1, xe = (int)ceilf(SW / 2.0f + (u0 > u1 ? u0 : u1)) + 1;
        if (xs < v->x0) xs = v->x0;
        if (xe > v->x1) xe = v->x1;
        // roof segment (if the row passes through the roof)
        int rs = 1, re = 0;
        if (fy == F_ROOF && thi >= 1) {
            rs = (int)ceilf(SW / 2.0f + v->a0 * (1 + hk) - 0.5f) + 1;
            re = (int)floorf(SW / 2.0f + v->a1 * (1 + hk) - 0.5f) - 1;
            if (rs < xs) rs = xs;
            if (re > xe) re = xe;
            if (rs <= re) {
                int mwy = wy_roof & WMASK;
                const int row = (mwy & 15) << 4;
                const uint16_t *mrow = map_v + ((mwy >> 4) << MSH);
                float fx = (rs + 0.5f - SW / 2.0f) * v->inv + cam_ix;
                const float inv = v->inv;
                for (int x = rs; x <= re; x++, fx += inv) {
                    int wx = (int)floorf(fx);
                    if (wx < lx0) wx = lx0;
                    if (wx > lx1) wx = lx1;
                    int mwx = wx & WMASK;
                    uint16_t mv = mrow[mwx >> 4];
                    d[x] = pal[MV_PAL(mv)][tex_ptr[MV_TEX(mv)][row | (mwx & 15)]];
                }
            }
        }
        for (int x = xs; x <= xe; x++) {
            if (x == rs && rs <= re) { x = re; continue; }
            float px = x + 0.5f - SW / 2.0f;
            float lo = tlo, hi = thi;
            int f = fy;
            float rr = px - v->a0;                         // left edge: ka0*t <= px - a0
            if (ka0 > 0) { float t = rr * ika0; if (t < hi) { hi = t; f = F_W; } }
            else if (ka0 < 0) { float t = rr * ika0; if (t > lo) lo = t; }
            else if (rr < 0) continue;
            rr = px - v->a1;                               // right edge: ka1*t >= px - a1
            if (ka1 > 0) { float t = rr * ika1; if (t > lo) lo = t; }
            else if (ka1 < 0) { float t = rr * ika1; if (t < hi) { hi = t; f = F_E; } }
            else if (rr > 0) continue;
            if (lo > hi) continue;

            if (f == F_ROOF) {
                // roof: atlas pixel under the base point
                int wx = cam_ix + (int)floorf(px * v->inv);
                if (wx < lx0) wx = lx0;
                if (wx > lx1) wx = lx1;
                int mwx = wx & WMASK, mwy = wy_roof & WMASK;
                uint16_t mv = map_v[MI(mwx >> 4, mwy >> 4)];
                d[x] = pal[MV_PAL(mv)][tex_ptr[MV_TEX(mv)][((mwy & 15) << 4) | (mwx & 15)]];
                continue;
            }
            // wall: height t = hi (0 — ground, 1 — roof), coordinate along the wall
            float t = hi;
            float sc = 1.0f / (1.0f + hk * t);
            int along = (f == F_W || f == F_E) ? (int)floorf(py * sc) + cam_iy : (int)floorf(px * sc) + cam_ix;
            uint8_t ci;
            if (t < 0.06f) ci = P_WALL_BASE;
            else if (t > 0.94f) ci = P_ROOF_EDGE;
            else {
                ci = (f == F_W) ? P_WALL_1 : (f == F_N) ? P_WALL_2 : P_WALL_3;
                float fl = t * floors, fp = fl - floorf(fl);
                int a = along & 15;
                bool win;
                if (IS_HOUSELIKE(style))    win = fp > 0.3f && fp < 0.72f && a >= 5 && a <= 10;
                else if (style != ST_INDUSTRY) win = fp > 0.25f && fp < 0.7f && (a & 7) >= 2 && (a & 7) <= 5;
                else                        win = fp > 0.55f && fp < 0.78f && a < 13;
                if (win) ci = (hash2(along >> 3, (int)fl + l->x0 * 7) % 7 == 0) ? P_WIN_LIT : P_WIN;
            }
            d[x] = pl[ci];
        }
    }
}

static inline uint16_t actor_col(const actor_t *a)
{
    switch (a->color) {
    case 6: return C_COP;
    case 7: return C_SUIT;
    case 8: return C_GANG1;
    case 9: return C_GANG2;
    case 10: return C_ROBBER;
    }
    return shirt_c[a->color % 6];
}

static void draw_corpse(const actor_t *a)
{
    float x = scr_x(a->x), y = scr_y(a->y);
    if (y + 8 < g_y0 || y - 8 >= g_y1 || x < -8 || x > SW + 8) return;
    float age = 15 - a->timer;                     // blood spreads
    float pr = 2.5f + fminf(age, 4.0f) * 1.3f;
    fill_circle(x + 2, y + 1, pr + 1.5f, C_BLOOD_D);
    fill_circle(x + 1, y, pr, C_BLOOD);
    if (age < 0.35f && a->kind) {                 // still falling
        draw_person(x, y, a->ang + PIf + age * 6, actor_col(a), a->kind == A_COP ? C_COP_HEAD : C_SKIN, false, false, 0, a->kind == A_COP ? 6 : a->color);
        return;
    }
    float ca = cosf(a->ang), sa = sinf(a->ang);
    for (int k = -3; k <= 3; k++) fill_circle(x + ca * k, y + sa * k, 2, actor_col(a));
    fill_circle(x + ca * 5, y + sa * 5, 2, a->kind == A_COP ? C_COP_HEAD : C_SKIN);
}

static void draw_pickup(const pickup_t *p)
{
    float x = scr_x(p->x), y = scr_y(p->y);
    if (y + 9 < g_y0 || y - 9 >= g_y1 || x < -9 || x > SW + 9) return;
    float b = sinf(g_time * 5) * 1.5f;
    int ix = (int)x, iy = (int)(y + b);
    fill_rect(ix - 6, iy - 6, 13, 13, C_BLACK);
    uint16_t c = (p->kind == PK_HEALTH) ? C_WHITE : C_YELLOW;
    fill_rect(ix - 5, iy - 5, 11, 11, c);
    switch (p->kind) {
    case PK_HEALTH:  fill_rect(ix - 1, iy - 4, 3, 9, C_RED); fill_rect(ix - 4, iy - 1, 9, 3, C_RED); break;
    case PK_PISTOL:  fill_rect(ix - 3, iy - 2, 7, 2, C_BLACK); fill_rect(ix - 3, iy, 2, 3, C_BLACK); break;
    case PK_UZI:     fill_rect(ix - 4, iy - 2, 9, 3, C_BLACK); fill_rect(ix - 1, iy + 1, 2, 3, C_BLACK); fill_rect(ix + 2, iy + 1, 2, 3, C_BLACK); break;
    case PK_SHOTGUN: fill_rect(ix - 5, iy - 1, 11, 2, C_BLACK); fill_rect(ix - 5, iy + 1, 3, 2, C_BLACK); break;
    case PK_FRENZY:  fill_rect(ix - 5, iy - 5, 11, 11, C_RED); fill_rect(ix - 3, iy - 3, 7, 5, C_WHITE);
                     fill_rect(ix - 2, iy - 2, 2, 2, C_BLACK); fill_rect(ix + 1, iy - 2, 2, 2, C_BLACK); fill_rect(ix - 2, iy + 2, 5, 2, C_WHITE); break;
    case PK_PACKAGE: fill_rect(ix - 5, iy - 5, 11, 11, C_BLACK); fill_rect(ix - 4, iy - 4, 9, 9, C_ORANGE); fill_rect(ix - 4, iy, 9, 1, C_YELLOW); fill_rect(ix, iy - 4, 1, 9, C_YELLOW); break;
    }
}

// ---------- Map place icons (7x7) ----------
enum { IC_GUN = 0, IC_FOOD, IC_CAR, IC_CASINO, IC_SPRAY, IC_FIRE, IC_SKULL, IC_PHONE, IC_BANK, IC_HELI };
static const char *ICONS[10][7] = {
    { ".......", "#######", "#######", "...##..", "...#...", "..##...", "......." },
    { "..###..", ".#####.", ".#####.", "..###..", "...#...", "..#.#..", "......." },
    { ".......", "..###..", ".#####.", "#######", "#######", ".#...#.", "......." },
    { "...#...", ".#####.", ".#.#...", ".#####.", "...#.#.", ".#####.", "...#..." },
    { "..#....", ".###...", ".###.#.", ".###...", ".###.#.", ".###...", "......." },
    { "...#...", "..##...", "..###..", ".####.#", ".#####.", ".#####.", "..###.." },
    { ".#####.", "#######", "#.#.#.#", "#######", ".#####.", ".#.#.#.", "......." },
    { ".#####.", "##...##", "#.....#", "..###..", ".##.##.", ".#####.", "......." },
    { "...#...", ".#####.", "#######", ".#.#.#.", ".#.#.#.", "#######", "......." },
    { "#######", "...#...", ".###...", "#####.#", "#######", ".###...", "#...#.." },
};
static void draw_icon(int cx, int cy, int ic, uint16_t bg)
{
    fill_rect(cx - 5, cy - 5, 11, 11, C_BLACK);
    fill_rect(cx - 4, cy - 4, 9, 9, bg);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++)
            if (ICONS[ic][y][x] == '#') put_px(cx - 3 + x, cy - 3 + y, C_WHITE);
}

// ---------- Map ----------
static inline uint16_t mini_color(int tx, int ty)
{
    int i = MI(tx, ty);
    uint16_t v = map_v[i];
    int k = tex_kind[MV_TEX(v)];
    const uint16_t *p = pal[MV_PAL(v)];
    switch (k) {
    case T_BUILDING: return p[P_ROOF];
    case T_PARK:     return BM_GET(bm_sand, i) ? rgb565(226, 208, 150) : p[P_GRASS1];    // beach / grass, fields, forest
    case T_SIDEWALK: return p[P_SIDEWALK];
    case T_PLAZA:    return p[P_PLAZA];
    default:         return mini_kc[k];
    }
}

// Overview map of the whole world (once per pause: 146x146 pixels, roads over fields)
#define OV_K 7
#define OV_N (MAP / OV_K)
static uint16_t *ov_buf;
static void build_overview(void)
{
    if (!ov_buf) ov_buf = malloc(OV_N * OV_N * sizeof(uint16_t));
    if (!ov_buf) return;
    for (int y = 0; y < OV_N; y++)
        for (int x = 0; x < OV_N; x++) {
            int tx = x * OV_K + OV_K / 2, ty = y * OV_K + OV_K / 2;
            uint16_t c = mini_color(tx, ty);
            int roads = 0;                                  // a road crosses the block's center row or column
            for (int k = 0; k < OV_K; k++) {
                if (lanes[MI(x * OV_K + k, ty)]) roads++;
                if (lanes[MI(tx, y * OV_K + k)]) roads++;
            }
            if (roads >= 2) c = mini_kc[0];
            ov_buf[y * OV_N + x] = c;
        }
    ov_ok = true;
}
static void draw_overview(int x0, int y0)
{
    if (!ov_ok) build_overview();
    if (!ov_buf) return;
    int ya = y0 < g_y0 ? g_y0 : y0, yb = (y0 + OV_N > g_y1) ? g_y1 : y0 + OV_N;
    for (int y = ya; y < yb; y++) memcpy(g_buf + (y - g_y0) * SW + x0, ov_buf + (y - y0) * OV_N, OV_N * sizeof(uint16_t));
}

// Map section: (x0,y0,w,h) on screen, center (cwx,cwy) in world px, num/den — screen pixels per tile
static void draw_map(int x0, int y0, int w, int h, float cwx, float cwy, int num, int den)
{
    int ya = y0 < g_y0 ? g_y0 : y0, yb = (y0 + h > g_y1) ? g_y1 : y0 + h;
    if (ya >= yb) return;
    int ctx = ((int)cwx & WMASK) >> 4, cty = ((int)cwy & WMASK) >> 4;
    for (int y = ya; y < yb; y++) {
        int ty = cty + ((y - y0) - h / 2) * den / num;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = 0; x < w; x++) {
            int tx = ctx + (x - w / 2) * den / num;
            d[x0 + x] = mini_color(tx, ty);
        }
    }
}

static bool map_pt(int x0, int y0, int w, int h, float cwx, float cwy, int num, int den, float wx, float wy, int *mx, int *my, bool clamp)
{
    float fx = x0 + w / 2 + wrapd(wx - cwx) / 16.0f * num / den;
    float fy = y0 + h / 2 + wrapd(wy - cwy) / 16.0f * num / den;
    bool in = fx >= x0 + 1 && fx < x0 + w - 2 && fy >= y0 + 1 && fy < y0 + h - 2;
    if (!in && !clamp) return false;
    if (fx < x0 + 1) fx = x0 + 1;
    if (fx > x0 + w - 3) fx = x0 + w - 3;
    if (fy < y0 + 1) fy = y0 + 1;
    if (fy > y0 + h - 3) fy = y0 + h - 3;
    *mx = (int)fx; *my = (int)fy;
    return true;
}

static void draw_map_markers(int x0, int y0, int w, int h, float cwx, float cwy, int num, int den, int dot)
{
    int mx, my;
    // police
    for (int i = 0; i < NACT; i++)
        if (act[i].kind == A_COP && act[i].state != S_DEAD &&
            map_pt(x0, y0, w, h, cwx, cwy, num, den, act[i].x, act[i].y, &mx, &my, false))
            fill_rect(mx, my, dot, dot, C_BLUE);
    for (int i = NCIVCARS; i < POLICE_END; i++)
        if (cars[i].mode == CAR_COP &&
            map_pt(x0, y0, w, h, cwx, cwy, num, den, cars[i].x, cars[i].y, &mx, &my, false))
            fill_rect(mx - 1, my - 1, dot + 1, dot + 1, (((int)(g_time * 6)) & 1) ? C_RED : C_BLUE);
    // interactive place markers
    static const uint8_t poi_icon[NPOI] = { IC_GUN, IC_FOOD, IC_CAR, IC_CASINO, IC_BANK };
    for (int k = 0; k < NPOI; k++)
        if (map_pt(x0, y0, w, h, cwx, cwy, num, den, pois[k].dx, pois[k].dy, &mx, &my, false)) draw_icon(mx, my, poi_icon[k], poi_color(k));
    if (map_pt(x0, y0, w, h, cwx, cwy, num, den, spray_x, spray_y, &mx, &my, false)) draw_icon(mx, my, IC_SPRAY, rgb565(255, 90, 200));
    for (int i = 0; i < NPICK; i++)
        if (pick[i].active && pick[i].kind == PK_FRENZY && map_pt(x0, y0, w, h, cwx, cwy, num, den, pick[i].x, pick[i].y, &mx, &my, false))
            draw_icon(mx, my, IC_SKULL, C_RED);
    plus_map_markers(x0, y0, w, h, cwx, cwy, num, den);
    if (fire_stage == 1 && map_pt(x0, y0, w, h, cwx, cwy, num, den, fire_x, fire_y, &mx, &my, false))
        draw_icon(mx, my, IC_FIRE, (((int)(g_time * 5)) & 1) ? C_ORANGE : C_RED);
    // mission target (at the edge if far away)
    if (map_pt(x0, y0, w, h, cwx, cwy, num, den, mis_x, mis_y, &mx, &my, true)) {
        fill_rect(mx - 1, my - 1, dot + 2, dot + 2, C_BLACK);
        fill_rect(mx, my, dot, dot, stage == 0 ? C_PICK : C_DROP);
    }
    // player — blinking white dot with a heading arrow
    if (map_pt(x0, y0, w, h, cwx, cwy, num, den, plx, ply, &mx, &my, true)) {
        uint16_t c = (((int)(g_time * 4)) & 1) ? C_WHITE : C_PLAYER;
        fill_rect(mx - 1, my - 1, dot + 2, dot + 2, C_BLACK);
        fill_rect(mx, my, dot, dot, c);
        put_px(mx + (int)roundf(cosf(pang) * (dot + 1)), my + (int)roundf(sinf(pang) * (dot + 1)), c);
    }
}

#define MINI_W 76
#define MINI_H 56
static void draw_minimap(void)
{
    int x0 = SW - MINI_W - 6, y0 = 24;
    if (y0 + MINI_H + 1 < g_y0 || y0 - 1 >= g_y1) return;
    fill_rect(x0 - 1, y0 - 1, MINI_W + 2, MINI_H + 2, C_BLACK);
    draw_map(x0, y0, MINI_W, MINI_H, plx, ply, 1, 1);
    draw_map_markers(x0, y0, MINI_W, MINI_H, plx, ply, 1, 1, 2);
}

// Mat at the shop entrance
static void draw_poi_doors(void)
{
    for (int k = 0; k < NPOI; k++) {
        float x = scr_x(pois[k].dx), y = scr_y(pois[k].dy);
        if (y + 10 < g_y0 || y - 10 >= g_y1 || x < -10 || x > SW + 10) continue;
        uint16_t c = poi_color(k);
        int ix = (int)x, iy = (int)y;
        fill_rect(ix - 7, iy - 7, 14, 14, C_BLACK);
        fill_rect(ix - 6, iy - 6, 12, 12, c);
        static const char *ic[NPOI] = { "G", "F", "A", "$", "B" };
        draw_text(ix - 1, iy - 2, ic[k], 1, C_WHITE);
        if (((int)(g_time * 3)) & 1) ring(x, y, 10, 1, c);
    }
}

// Rooftop signs (accounting for building height)
static void draw_poi_signs(void)
{
    for (int k = 0; k < NPOI; k++) {
        const lot_t *l = &g_lots[pois[k].lot];
        float mx = wrapd((l->x0 + l->x1 + 1) * 8.0f - cam_ix), my = wrapd((l->y0 + l->y1 + 1) * 8.0f - cam_iy);
        float hk = BUILDINGS_3D ? l->h10 * 0.0055f : 0;
        float sx = SW / 2 + mx * (1 + hk), sy = SH / 2 + my * (1 + hk);
        if (sy + 12 < g_y0 || sy - 12 >= g_y1 || sx < -60 || sx > SW + 60) continue;
        int sc = (k == POI_CASINO) ? 2 : 1;
        int w = text_w(POI_NAME[k], sc);
        fill_rect((int)sx - w / 2 - 3, (int)sy - 3 * sc - 2, w + 6, 5 * sc + 4, C_BLACK);
        uint16_t c = poi_color(k);
        if (k == POI_CASINO) { static const uint8_t neon[3][3] = { {255, 80, 220}, {255, 220, 60}, {80, 220, 255} }; int n = ((int)(g_time * 3)) % 3; c = rgb565(neon[n][0], neon[n][1], neon[n][2]); }
        draw_text((int)sx - w / 2, (int)sy - 2 * sc - 1, POI_NAME[k], sc, c);
    }
}

static void draw_casino(void);
// Shop panel / casino reels
static void draw_shop_panel(void)
{
    if (poi_near == POI_CASINO) { draw_casino(); return; }
    if (poi_near < 0 || (poi_near == POI_BANK && plus_heist_busy())) return;
    const item_t *it = &SHOP_ITEMS[poi_near][shop_item];
    shade_rect(40, SH - 70, SW - 80, 34);
    char t[40];
    if (it->price) snprintf(t, sizeof(t), "%s: %s $%d", POI_NAME[poi_near], it->name, it->price);
    else snprintf(t, sizeof(t), "%s: %s", POI_NAME[poi_near], it->name);
    draw_text_c(SH - 64, t, 1, poi_color(poi_near));
    draw_text_c(SH - 52, poi_near == POI_BANK ? "B/K2 START - STAY 10 SEC" : "B/K2 BUY   X/K3 NEXT", 1, C_WHITE);
    if (poi_near == POI_CASINO && !super_won) draw_text_c(SH - 44, "BIG SPIN JACKPOT: VORTEX GT", 1, C_YELLOW);
}

// ---------- Casino: slot machine and roulette ----------
static uint16_t sym_pixel(char sy, int x, int y)       // reel symbol 16x16
{
    switch (sy) {
    case '7': {                                         // red seven from the font, large
        int gx = (x - 2) / 4, gy = (y - 0) / 3;
        if (gx < 0 || gx > 2 || gy < 0 || gy > 4) return 0;
        return (FONT[7] & (1 << (14 - (gy * 3 + gx)))) ? C_RED : 0;
    }
    case '$':                                           // BAR
        if (y >= 4 && y < 12 && x >= 1 && x < 15) return (y == 4 || y == 11 || x == 1 || x == 14) ? C_YELLOW : C_DARK;
        return 0;
    case '*': {                                         // bell
        int dx = x - 8, dy = y - 8;
        if (dy < 5 && dx * dx * 49 + (dy + 1) * (dy + 1) * 25 < 49 * 25) return (dx < -2) ? C_WHITE : C_YELLOW;
        if (dy >= 5 && dy < 7 && abs(dx) < 6) return C_ORANGE;
        if (dy >= 7 && dy < 9 && abs(dx) < 2) return C_ORANGE;
        return 0;
    }
    default: {                                          // cherries
        int d1 = (x - 4) * (x - 4) + (y - 11) * (y - 11), d2 = (x - 11) * (x - 11) + (y - 11) * (y - 11);
        if (d1 < 12 || d2 < 12) return (d1 < 3 || d2 < 3) ? C_WHITE : C_RED;
        if ((y < 9 && y > 1 && (abs(x - 4 - (9 - y) / 2) < 1 || abs(x - 11 + (9 - y) / 3) < 1))) return C_GREEN;
        return 0;
    }
    }
}

static void draw_slot_machine(int ox, int oy)
{
    if (spr_slot.px) blit(&spr_slot, 0, ox, oy, false, false);
    else {
        fill_rect(ox + 4, oy, 84, 128, rgb565(150, 20, 30));
        fill_rect(ox + 8, oy + 6, 76, 18, rgb565(30, 10, 40));
        draw_text(ox + 20, oy + 12, "LUCKY 7", 1, C_YELLOW);
    }
    // three reels in the window (14..74, 40..76) in sprite coordinates
    static const char strip[4] = { '7', '$', '*', 'x' };
    float el = spin_len - spin_t;
    for (int r = 0; r < 3; r++) {
        int wx = ox + 14 + r * 20, wy = oy + 40;
        bool spinning = spin_t > 0 && spin_kind == 0 && el < 0.9f + 0.35f * r;
        // strip position: when stopped — the needed symbol in the center
        int final_i = 0;
        for (int k = 0; k < 4; k++) if (strip[k] == spin_sym[r]) final_i = k;
        float pos = spinning ? (el * 260.0f + r * 37) : final_i * 18.0f;
        for (int y = wy; y < wy + 36; y++) {
            if (y < g_y0 || y >= g_y1) continue;
            uint16_t *d = g_buf + (y - g_y0) * SW;
            float sp = pos + (y - wy) - 9;              // symbol in the center of the window
            int idx = (int)floorf(sp / 18.0f);
            int ly = (int)(sp - idx * 18.0f);
            char sy = strip[((idx % 4) + 4) % 4];
            for (int x = wx; x < wx + 20; x++) {
                uint16_t c = (x == wx || x == wx + 19) ? C_GREY : C_WHITE;
                int lx = x - wx - 2;
                if (lx >= 0 && lx < 16 && ly >= 1 && ly < 17) { uint16_t q = sym_pixel(sy, lx, ly - 1); if (q) c = q; }
                if (y == wy + 17 || y == wy + 18) c = spinning ? c : blend50(c, C_RED);   // winning line
                d[x] = c;
            }
        }
    }
}

static void draw_roulette(int cx, int cy)
{
    const int R = 58;
    uint16_t rim1 = rgb565(120, 70, 30), rim2 = rgb565(80, 44, 18), gold = rgb565(230, 190, 70);
    uint16_t red = rgb565(200, 20, 24), blk = rgb565(20, 20, 22), grn = rgb565(20, 140, 60);
    uint16_t wood = rgb565(150, 96, 50), wood2 = rgb565(110, 70, 36);
    for (int y = cy - R; y <= cy + R; y++) {
        if (y < g_y0 || y >= g_y1) continue;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        int dy = y - cy;
        for (int x = cx - R; x <= cx + R; x++) {
            int dx = x - cx, d2 = dx * dx + dy * dy;
            if (d2 > R * R) continue;
            float dd = sqrtf((float)d2);
            uint16_t c;
            if (dd > R - 6) c = (dx + dy < 0) ? rim1 : rim2;
            else if (dd > R - 22) {
                float a = atan2f((float)dy, (float)dx) - wheel_ang;
                a = fmodf(a, 2 * PIf); if (a < 0) a += 2 * PIf;
                float f = a / (2 * PIf / ROUL_N);
                int p = (int)f % ROUL_N;
                c = (p == 0) ? grn : (p & 1) ? red : blk;
                if (f - (int)f < 0.08f || dd < R - 20 || dd > R - 8) c = gold;
            } else if (dd > R - 26) c = gold;
            else {
                float a = atan2f((float)dy, (float)dx) - wheel_ang;
                float sp = fabsf(sinf(a * 2));
                c = (sp < 0.12f && dd > 6) ? gold : (dx + dy < 0 ? wood : wood2);
                if (dd < 6) c = gold;
            }
            d[x] = c;
        }
    }
    // ball
    if (spin_kind == 1 && (spin_t > 0 || roul_pocket >= 0)) {
        float bx = cx + cosf(ball_ang) * ball_r, by = cy + sinf(ball_ang) * ball_r;
        fill_circle(bx, by, 3.2f, C_GREY);
        fill_circle(bx - 0.5f, by - 0.5f, 2.4f, C_WHITE);
    }
}

static void draw_casino(void)
{
    shade_rect(0, 0, SW, SH);
    shade_rect(0, 0, SW, SH);
    draw_text_c(8, "CASINO", 3, rgb565(255, 220, 60));
    draw_slot_machine(18, 34);
    draw_roulette(232, 104);
    const item_t *it = &SHOP_ITEMS[POI_CASINO][shop_item];
    char t[40];
    snprintf(t, sizeof(t), "%s  $%d", it->name, it->price);
    draw_text_c(SH - 44, t, 2, shop_item < 2 ? C_YELLOW : (shop_item == 2 ? C_RED : C_WHITE));
    draw_text_c(SH - 26, "B/K2 PLAY   X/K3 NEXT GAME", 1, C_WHITE);
    if (!super_won) draw_text_c(SH - 16, "BIG SLOTS JACKPOT 777: VORTEX GT", 1, C_GREEN);
    if (msg_t > 0 && msg[0]) draw_text_c(170, msg, 1, C_YELLOW);
}

static void draw_star(int cx, int cy, uint16_t c)
{
    fill_rect(cx - 1, cy - 5, 3, 11, c);
    fill_rect(cx - 5, cy - 1, 11, 3, c);
    fill_rect(cx - 3, cy - 3, 7, 7, c);
}

static void draw_hud(float msx, float msy, uint16_t mc)
{
    char s[32];
    if (world_missing) {
        draw_text_c(92, "NO WORLD.BIN", 3, C_RED);
        draw_text_c(124, "COPY FILES FROM SD_CARD FOLDER", 1, C_WHITE);
        draw_text_c(136, "TO THE ROOT OF THE SD CARD", 1, C_WHITE);
    }
    // money
    snprintf(s, sizeof(s), "$%ld", (long)money);
    draw_text_sh(6, 6, s, 3, C_GREEN);

    // health
    fill_rect(6, 25, 62, 6, C_BLACK);
    fill_rect(7, 26, 60, 4, C_HP_BG);
    fill_rect(7, 26, php * 60 / 100, 4, (hurt_t > 0) ? C_WHITE : C_HP);
    if (armor > 0) { fill_rect(6, 31, 62, 3, C_BLACK); fill_rect(7, 31, armor * 60 / 100, 2, rgb565(80, 160, 255)); }

    // arrest meter
    if (arrest_t > 0) {
        int w = (int)(arrest_t / ARREST_TIME * 60);
        fill_rect(6, 34, 62, 6, C_BLACK);
        fill_rect(7, 35, w, 4, C_BLUE);
        draw_text_sh(72, 33, "ARREST!", 1, ((int)(g_time * 8)) & 1 ? C_WHITE : C_BLUE);
    }

    // wanted level — 6 stars
    bool blink = stars > 0 && lose_t > 5 && (((int)(g_time * 4)) & 1);
    for (int i = 0; i < 6; i++) {
        int cx = SW - 12 - i * 14;
        draw_star(cx + 1, 13, C_BLACK);
        draw_star(cx, 12, (i < stars && !blink) ? C_STAR : C_STAR_OFF);
    }

    // weapon and ammo
    if (in_heli) snprintf(s, sizeof(s), "MINIGUN");
    else if (weapon == W_FIST) snprintf(s, sizeof(s), "FIST");
    else snprintf(s, sizeof(s), "%s %d", WEAP[weapon].name, ammo[weapon]);
    draw_text_sh(6, SH - 16, s, 2, C_WHITE);

    draw_minimap();

    // gamepad / SD status
    draw_text_sh(SW - 40, SH - 12, "PAD", 2, st_pad ? C_GREEN : C_GREY);
    if (st_sd) draw_text_sh(SW - 68, SH - 12, "SD", 2, C_GREEN);

    // target pointer
    if (msx < 0 || msx >= SW || msy < 0 || msy >= SH) {
        float dx = msx - SW / 2, dy = msy - SH / 2;
        float k = fminf((SW / 2 - 14) / (fabsf(dx) + 0.001f), (SH / 2 - 14) / (fabsf(dy) + 0.001f));
        fill_circle(SW / 2 + dx * k, SH / 2 + dy * k, 7, C_BLACK);
        fill_circle(SW / 2 + dx * k, SH / 2 + dy * k, 5, mc);
    }

    // clock
    {
        float h = game_hour();
        char ts[16];
        snprintf(ts, sizeof(ts), "%02d:%02d", ((int)h) % 24, ((int)((h - (int)h) * 60) / 10 * 10) % 60);
        draw_text_sh(6, 34 + (arrest_t > 0 ? 9 : 0), ts, 1, raining ? C_GREY : C_WHITE);
    }
    // mission
    if (gstate == G_PLAY && poi_near < 0) draw_text_c(SH - 30, mission_text(), 1, C_YELLOW);
    // massacre
    if (frenzy_t > 0) {
        char fs[32];
        snprintf(fs, sizeof(fs), "FRENZY %d  KILLS %d", (int)frenzy_t + 1, frenzy_kills);
        draw_text_c(28, fs, 2, ((int)(g_time * 4)) & 1 ? C_RED : C_WHITE);
    }

    // "enter" hint
    if (gstate == G_PLAY && ON_FOOT && can_act && poi_near < 0) draw_text_c(SH - 52, "CAR: B OR K2", 2, C_WHITE);

    if (msg_t > 0 && msg[0] && poi_near != POI_CASINO) draw_text_c(44, msg, 2, C_YELLOW);
}

static void render_overlay(void)
{
    switch (gstate) {
    case G_TITLE: {
        shade_rect(0, 60, SW, 120);
        if (spr_logo.px) blit(&spr_logo, 0, SW / 2 - spr_logo.w / 2, 52, false, false);
        else draw_text_c(72, "GTA", 9, C_ORANGE);
        draw_text_c(124, "ESP32-S3 EDITION", 2, C_WHITE);
        if (((int)(g_time * 2)) & 1) draw_text_c(150, "PRESS FIRE", 2, C_YELLOW);
        draw_text_c(168, st_pad ? "GAMEPAD OK" : "PAD: SEARCHING / JOYSTICK", 1, st_pad ? C_GREEN : C_GREY);
        break;
    }
    case G_PAUSE: {
        // map: whole world 1024x1024 tiles -> 146x146 px (7 tiles per pixel) + player surroundings 1:2
        shade_rect(0, 0, SW, SH);
        int mx0 = 4, my0 = 18, mw = OV_N, mh = OV_N;
        fill_rect(mx0 - 2, my0 - 2, mw + 4, mh + 4, C_BLACK);
        draw_overview(mx0, my0);
        draw_map_markers(mx0, my0, mw, mh, 512 * 16, 512 * 16, 1, OV_K, 2);
        draw_text_sh(mx0 + 14, my0 + 38, "OLD TOWN", 1, C_WHITE);
        draw_text_sh(mx0 + 98, my0 + 48, "PORT CITY", 1, C_WHITE);
        draw_text_sh(mx0 + 90, my0 + 18, "AIRPORT", 1, C_WHITE);
        {
            int zx0 = mx0 + mw + 6, zw = 256 - mw - 6;
            fill_rect(zx0 - 2, my0 - 2, zw + 4, mh + 4, C_BLACK);
            draw_map(zx0, my0, zw, mh, plx, ply, 1, 2);
            draw_map_markers(zx0, my0, zw, mh, plx, ply, 1, 2, 2);
        }
        draw_text_sh(mx0, 4, "MAP / PAUSE", 2, C_WHITE);
        int rx = 4, ry = 174;
        static const char *lines[] = {
            "A/K1   FIRE", "B/K2   CINEMA", "X/K3   REPLAY", "Y/K4   RUN", "START  RESUME", "SELECT SHOT", "",
        };
        for (int i = 0; i < 6; i++) draw_text_sh(rx, ry + i * 10, lines[i], 1, C_WHITE);
        char s2[24];
        int sx2 = 266;
        snprintf(s2, sizeof(s2), "KILLS %ld", (long)kills);  draw_text_sh(sx2, 22, s2, 1, C_YELLOW);
        snprintf(s2, sizeof(s2), "JOBS %ld", (long)deliveries); draw_text_sh(sx2, 32, s2, 1, C_YELLOW);
        snprintf(s2, sizeof(s2), "BEST *%ld", (long)best_wanted); draw_text_sh(sx2, 42, s2, 1, C_YELLOW);
        snprintf(s2, sizeof(s2), "PKG %d/%d", packages, NPACKAGES); draw_text_sh(sx2, 52, s2, 1, C_YELLOW);
        int lx = 70;
        fill_rect(lx, ry + 1, 4, 4, C_WHITE);  draw_text_sh(lx + 8, ry, "YOU", 1, C_WHITE);
        fill_rect(lx, ry + 11, 4, 4, C_DROP);   draw_text_sh(lx + 8, ry + 10, "JOB", 1, C_WHITE);
        fill_rect(lx, ry + 21, 4, 4, C_BLUE);   draw_text_sh(lx + 8, ry + 20, "POLICE", 1, C_WHITE);
        fill_rect(lx, ry + 31, 4, 4, rgb565(255, 90, 200)); draw_text_sh(lx + 8, ry + 30, "SPRAY", 1, C_WHITE);
        for (int k = 0; k < NPOI; k++) { fill_rect(lx + 52, ry + 1 + k * 10, 4, 4, poi_color(k)); draw_text_sh(lx + 60, ry + k * 10, POI_NAME[k], 1, C_WHITE); }
        plus_draw_pause(222, ry);
        if (st_pad && st_pad_name[0]) draw_text_sh(sx2, 64, st_pad_name, 1, C_GREEN);
        break;
    }
    case G_WASTED:
        if (state_t > 0.3f) { shade_rect(0, 90, SW, 50); if (goosed) draw_text_c(98, "GOOSED", 7, C_WHITE); else draw_text_c(98, "WASTED", 7, C_RED); }
        break;
    case G_BUSTED:
        if (state_t > 0.3f) { shade_rect(0, 90, SW, 50); draw_text_c(98, "BUSTED", 7, C_BLUE); }
        break;
    }
}

// ================= Living city: time of day, weather, light, trees, lamps, traffic lights, boats, marks, particles =================

// ---- time of day and weather ----

static void update_daylight(float dt)
{
    day_t += dt;
    if (day_t >= DAY_LEN) day_t -= DAY_LEN;
    weather_t -= dt;
    if (weather_t <= 0) {
        raining = !raining && (rnd() % 3 == 0);
        weather_t = raining ? 40 + rnd() % 40 : 60 + rnd() % 60;
    }
    tint_timer -= dt;
    if (tint_timer > 0) return;
    tint_timer = 0.2f;
    float h = game_hour();
    float d = sinf((h - 6) / 12 * PIf) * 1.5f + 0.45f;
    if (d < 0) d = 0;
    if (d > 1) d = 1;
    float dusk = 4 * d * (1 - d);                          // sunset/sunrise — warm tint
    float r = (0.30f + 0.70f * d) * (1 + 0.14f * dusk);
    float g = (0.36f + 0.64f * d) * (1 - 0.03f * dusk);
    float b = (0.56f + 0.44f * d) * (1 - 0.20f * dusk);
    if (raining) { r *= 0.78f; g *= 0.8f; b *= 0.86f; }
    night = 1 - d;
    apply_tint((int)(r * 256), (int)(g * 256), (int)(b * 256), night > 0.45f);
}

// ---- props: trees, lamps, intersections ----
typedef struct { int16_t x, y; uint8_t r; } prop_t;
#define MAX_TREES 60000
#define MAX_LAMPS 20000
#define MAX_XING  4000
static uint16_t vtree[256], vlamp[256];
static int nvt = 0, nvl = 0;
static prop_t *trees, *lamps;
static int ntrees = 0, nlamps = 0;
static uint16_t (*xing)[2];
static int nxing = 0;

static int prop_cmp(const void *a, const void *b) { const prop_t *p = a, *q = b; return p->y != q->y ? p->y - q->y : p->x - q->x; }
static void build_prop_index(void)
{
    int a = 0, b = 0, c = 0;
    for (int r = 0; r <= MAP; r++) {
        while (a < ntrees && (trees[a].y >> 4) < r) a++;
        while (b < nlamps && (lamps[b].y >> 4) < r) b++;
        while (c < nxing && xing[c][1] < r) c++;
        tree_row[r] = a; lamp_row[r] = b; xing_row[r] = c;
    }
}

static void add_tree(int x, int y, int r) { if (ntrees < MAX_TREES) { trees[ntrees].x = (int16_t)x; trees[ntrees].y = (int16_t)y; trees[ntrees].r = (uint8_t)r; ntrees++; } }

static void gen_props(void)
{
    if (!trees) {
        trees = malloc(MAX_TREES * sizeof(prop_t));
        lamps = malloc(MAX_LAMPS * sizeof(prop_t));
        xing = malloc(MAX_XING * sizeof(xing[0]));
    }
    ntrees = nlamps = nxing = 0;
    static const int8_t dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int ty = 0; ty < MAP; ty++)
        for (int tx = 0; tx < MAP; tx++) {
            int k = gk(tx, ty);
            uint32_t h = hash2(tx, ty);
            if (k == T_PARK) {
                int u = g_use[MI(tx, ty)] & 0x3F, dd = g_dist[MI(tx, ty)];
                bool wide = gk(tx - 1, ty) == T_PARK && gk(tx + 1, ty) == T_PARK && gk(tx, ty - 1) == T_PARK && gk(tx, ty + 1) == T_PARK;
                bool beach = false;      // no trees by the water (sand there)
                for (int dy = -2; dy <= 2 && !beach; dy++)
                    for (int dx = -2; dx <= 2; dx++) if (gk(tx + dx, ty + dy) == T_WATER && (g_sea[MI(tx + dx, ty + dy)] || (abs(dx) <= 1 && abs(dy) <= 1))) { beach = true; break; }
                if (beach || u == U_AIRGRASS) continue;
                int jx = (int)((h >> 8) % 7) - 3, jy = (int)((h >> 12) % 7) - 3;
                if ((dd >> 5) || (dd & 31) == D_VILLAGE) {              // city and villages: parks and yards
                    if (wide && (h >> 4) % ((dd & 31) == D_PARK ? 2 : 3) == 0) add_tree(tx * 16 + 8 + jx, ty * 16 + 8 + jy, 9 + (h >> 16) % 4);
                } else if (u == U_FOREST) {                              // forest edge (inside the forest — a solid canopy via texture)
                    int nf = 0;
                    for (int d = 0; d < 4; d++) if ((g_use[MI(tx + dx4[d], ty + dy4[d])] & 0x3F) == U_FOREST) nf++;
                    if (nf < 4 && (h >> 4) % 3 != 0) add_tree(tx * 16 + 8 + jx, ty * 16 + 8 + jy, 10 + (h >> 16) % 4);
                } else if (IS_FIELD(u)) {                                // shelterbelts along field borders
                    int ul = g_use[MI(tx - 1, ty)] & 0x3F, uu = g_use[MI(tx, ty - 1)] & 0x3F;
                    bool edge = (IS_FIELD(ul) && ul != u) || (IS_FIELD(uu) && uu != u);
                    if (edge && (h >> 4) % 2 == 0) add_tree(tx * 16 + 8 + jx / 2, ty * 16 + 8 + jy / 2, 8 + (h >> 16) % 3);
                    else if (u == U_MEADOW && wide && (h >> 4) % 23 == 0) add_tree(tx * 16 + 8 + jx, ty * 16 + 8 + jy, 10 + (h >> 16) % 4);
                } else if (u == U_VERGE && (h >> 4) % 9 == 0) add_tree(tx * 16 + 8, ty * 16 + 8, 7 + (h >> 16) % 3);
            } else if (k == T_SIDEWALK) {
                // street lamps along the roadway (at the curb)
                int road = -1;
                for (int d = 0; d < 4; d++) if (lanes[MI(tx + dx4[d], ty + dy4[d])]) { road = d; break; }
                if (road >= 0 && ((tx * 3 + ty * 5) % 7) == 0 && nlamps < MAX_LAMPS) {
                    lamps[nlamps].x = tx * 16 + 8 + dx4[road] * 5;
                    lamps[nlamps].y = ty * 16 + 8 + dy4[road] * 5;
                    lamps[nlamps].r = road;
                    nlamps++;
                } else if (road >= 0 && ((tx * 3 + ty * 5) % 7) == 3) {
                    // street trees, if there's a grassy yard behind the sidewalk
                    bool yard = false;
                    for (int d = 0; d < 4; d++) if (gk(tx + dx4[d], ty + dy4[d]) == T_PARK) yard = true;
                    if (yard) add_tree(tx * 16 + 8, ty * 16 + 8, 5 + (h >> 16) % 2);
                }
            } else if (k == T_PLAZA) {
                // floodlights of the apron, parking lots and gas stations
                int u = g_use[MI(tx, ty)] & 0x3F;
                if ((u == U_APRON || u == U_LOT || u == U_GAS) && (tx % 9) == 4 && (ty % 9) == 4 && nlamps < MAX_LAMPS) {
                    lamps[nlamps].x = tx * 16 + 8; lamps[nlamps].y = ty * 16 + 8; lamps[nlamps].r = 0; nlamps++;
                }
            } else if (k == T_ROAD) {
                int f = lanes[MI(tx, ty)];
                int rt = g_rt[MI(tx, ty)];
                if ((f & LF_W) && (f & LF_S) && nxing < MAX_XING && (rt == RT_CITY || rt == RT_PORT)) { xing[nxing][0] = (uint16_t)tx; xing[nxing][1] = (uint16_t)ty; nxing++; }
            }
        }
    qsort(trees, ntrees, sizeof(prop_t), prop_cmp);
    qsort(lamps, nlamps, sizeof(prop_t), prop_cmp);
    build_prop_index();
    printf("props: %d trees, %d lamps, %d crossings\n", ntrees, nlamps, nxing);
}

// ================= WORLD.BIN: prebuilt world (generated on PC, loaded on ESP32) =================
#define WORLD_MAGIC 0x444C5747u      // "GWLD"
#define WORLD_VER   1
static game_io_fn world_rd = NULL;
static void *world_ctx = NULL;
static bool world_ok = false;

void game_set_world_source(game_io_fn rd, void *ctx) { world_rd = rd; world_ctx = ctx; }
bool game_world_loaded(void) { return world_ok; }

typedef struct { game_io_fn fn; void *ctx; uint8_t buf[4096]; size_t pos, len; bool err; } wbuf_t;
static void wb_flush(wbuf_t *b) { if (b->pos && b->fn(b->ctx, b->buf, b->pos) != b->pos) b->err = true; b->pos = 0; }
static void wb_put(wbuf_t *b, const void *p, size_t n)
{
    const uint8_t *s = p;
    while (n) { size_t k = sizeof(b->buf) - b->pos; if (k > n) k = n; memcpy(b->buf + b->pos, s, k); b->pos += k; s += k; n -= k; if (b->pos == sizeof(b->buf)) wb_flush(b); }
}
static void wb_get(wbuf_t *b, void *p, size_t n)
{
    uint8_t *d = p;
    while (n) {
        if (b->pos == b->len) { b->len = b->fn(b->ctx, b->buf, sizeof(b->buf)); b->pos = 0; if (!b->len) { b->err = true; memset(d, 0, n); return; } }
        size_t k = b->len - b->pos; if (k > n) k = n;
        memcpy(d, b->buf + b->pos, k); b->pos += k; d += k; n -= k;
    }
}
static void wb_u32(wbuf_t *b, uint32_t v) { wb_put(b, &v, 4); }
static uint32_t wb_g32(wbuf_t *b) { uint32_t v = 0; wb_get(b, &v, 4); return v; }
// RLE: (u16 length, value)
static void wb_rle16(wbuf_t *b, const uint16_t *a, size_t n)
{
    for (size_t i = 0; i < n;) { size_t j = i; while (j < n && j - i < 65535 && a[j] == a[i]) j++; uint16_t L = (uint16_t)(j - i); wb_put(b, &L, 2); wb_put(b, &a[i], 2); i = j; }
}
static void wb_rle8(wbuf_t *b, const uint8_t *a, size_t n)
{
    for (size_t i = 0; i < n;) { size_t j = i; while (j < n && j - i < 65535 && a[j] == a[i]) j++; uint16_t L = (uint16_t)(j - i); wb_put(b, &L, 2); wb_put(b, &a[i], 1); i = j; }
}
static void wb_unrle16(wbuf_t *b, uint16_t *a, size_t n)
{
    for (size_t i = 0; i < n && !b->err;) { uint16_t L, v; wb_get(b, &L, 2); wb_get(b, &v, 2); if (!L || i + L > n) { b->err = true; return; } for (int k = 0; k < L; k++) a[i++] = v; }
}
static void wb_unrle8(wbuf_t *b, uint8_t *a, size_t n)
{
    for (size_t i = 0; i < n && !b->err;) { uint16_t L; uint8_t v; wb_get(b, &L, 2); wb_get(b, &v, 1); if (!L || i + L > n) { b->err = true; return; } memset(a + i, v, L); i += L; }
}

bool game_world_save(game_io_fn wr, void *ctx)
{
    wbuf_t *b = calloc(1, sizeof(wbuf_t));
    b->fn = wr; b->ctx = ctx;
    const size_t N = (size_t)MAP * MAP;
    wb_u32(b, WORLD_MAGIC); wb_u32(b, WORLD_VER); wb_u32(b, MAP); wb_u32(b, sizeof(lot_t));
    wb_u32(b, ntex);
    wb_put(b, tex_kind, ntex);
    for (int i = 0; i < ntex; i++) wb_put(b, tex_ptr[i], TILE * TILE);
    wb_rle16(b, map_v, N);
    wb_rle8(b, lanes, N);
    wb_rle8(b, bm_sea, BM_BYTES); wb_rle8(b, bm_rock, BM_BYTES); wb_rle8(b, bm_shore, BM_BYTES); wb_rle8(b, bm_sand, BM_BYTES);
    wb_u32(b, g_nlots); wb_put(b, g_lots, sizeof(lot_t) * (g_nlots + 1));
    for (int k = 0; k < NPOI; k++) { wb_put(b, &pois[k].dx, 4); wb_put(b, &pois[k].dy, 4); wb_u32(b, (uint32_t)pois[k].lot); }
    wb_u32(b, nfount); for (int k = 0; k < MAX_FOUNT; k++) { wb_u32(b, fount_x[k]); wb_u32(b, fount_y[k]); }
    wb_u32(b, ntrees); wb_put(b, trees, sizeof(prop_t) * ntrees);
    wb_u32(b, nlamps); wb_put(b, lamps, sizeof(prop_t) * nlamps);
    wb_u32(b, nxing); wb_put(b, xing, sizeof(xing[0]) * nxing);
    wb_put(b, zgang, ZG_BYTES);
    wb_u32(b, WORLD_MAGIC ^ 0xFFFFFFFFu);
    wb_flush(b);
    bool ok = !b->err;
    free(b);
    return ok;
}

static bool world_load(void)
{
    wbuf_t *b = calloc(1, sizeof(wbuf_t));
    if (!b) return false;
    b->fn = world_rd; b->ctx = world_ctx;
    const size_t N = (size_t)MAP * MAP;
    bool ok = false;
    if (wb_g32(b) != WORLD_MAGIC || wb_g32(b) != WORLD_VER || wb_g32(b) != MAP || wb_g32(b) != sizeof(lot_t)) { printf("world: bad header\n"); goto out; }
    world_alloc();
    ntex = (int)wb_g32(b);
    if (ntex <= 0 || ntex > MAX_TEX) goto out;
    wb_get(b, tex_kind, ntex);
    for (int i = 0; i < ntex; i++) wb_get(b, tex_w(i), TILE * TILE);
    wb_unrle16(b, map_v, N);
    wb_unrle8(b, lanes, N);
    wb_unrle8(b, bm_sea, BM_BYTES); wb_unrle8(b, bm_rock, BM_BYTES); wb_unrle8(b, bm_shore, BM_BYTES); wb_unrle8(b, bm_sand, BM_BYTES);
    g_nlots = (int)wb_g32(b);
    if (g_nlots < 0 || g_nlots >= MAX_LOTS) goto out;
    wb_get(b, g_lots, sizeof(lot_t) * (g_nlots + 1));
    for (int k = 0; k < NPOI; k++) { wb_get(b, &pois[k].dx, 4); wb_get(b, &pois[k].dy, 4); pois[k].lot = (int)wb_g32(b); }
    nfount = (int)wb_g32(b); for (int k = 0; k < MAX_FOUNT; k++) { fount_x[k] = (int)wb_g32(b); fount_y[k] = (int)wb_g32(b); }
    ntrees = (int)wb_g32(b); if (ntrees < 0 || ntrees > MAX_TREES) goto out;
    trees = malloc(sizeof(prop_t) * (ntrees + 1)); wb_get(b, trees, sizeof(prop_t) * ntrees);
    nlamps = (int)wb_g32(b); if (nlamps < 0 || nlamps > MAX_LAMPS) goto out;
    lamps = malloc(sizeof(prop_t) * (nlamps + 1)); wb_get(b, lamps, sizeof(prop_t) * nlamps);
    nxing = (int)wb_g32(b); if (nxing < 0 || nxing > MAX_XING) goto out;
    xing = malloc(sizeof(xing[0]) * (nxing + 1)); wb_get(b, xing, sizeof(xing[0]) * nxing);
    wb_get(b, zgang, ZG_BYTES);
    if (wb_g32(b) != (WORLD_MAGIC ^ 0xFFFFFFFFu) || b->err) { printf("world: truncated/corrupt\n"); goto out; }
    for (int k = 0; k < NSTASH; k++) { stash_x[k] = STASH_AT[k][0] * 16 + 8; stash_y[k] = STASH_AT[k][1] * 16 + 8; }
    light_x = LIGHT_TX * 16 + 8; light_y = LIGHT_TY * 16 + 8;
    build_lot_index();
    build_prop_index();
    printf("world: loaded %d textures, %d lots, %d trees, %d lamps\n", ntex, g_nlots, ntrees, nlamps);
    ok = true;
out:
    free(b);
    return ok;
}

// No WORLD.BIN on ESP32: empty sea and an on-screen message (1024x1024 generation doesn't fit in memory on the board)
static __attribute__((unused)) void world_placeholder(void)
{
    world_alloc();
    uint8_t t[TILE * TILE];
    for (int i = 0; i < TILE * TILE; i++) t[i] = P_WATER;
    ntex = 0; memcpy(tex_w(0), t, sizeof(t)); tex_kind[0] = T_WATER; ntex = 1;
    for (int i = 1; i < MAX_TEX; i++) tex_kind[i] = T_WATER;
    for (size_t i = 0; i < (size_t)MAP * MAP; i++) map_v[i] = 0;
    memset(lanes, 0, (size_t)MAP * MAP);
    g_nlots = 0; nfount = 0;
    trees = malloc(sizeof(prop_t)); lamps = malloc(sizeof(prop_t)); xing = malloc(sizeof(xing[0]));
    ntrees = nlamps = nxing = 0;
    build_lot_index(); build_prop_index();
    world_missing = true;
}

static void world_setup(void)
{
    world_ok = false;
    if (world_rd && world_load()) { world_ok = true; return; }
#if defined(ESP_PLATFORM) || defined(FORCE_PLACEHOLDER)
    printf("world: WORLD.BIN missing or broken\n");
    world_placeholder();
#else
    generate_city();
    world_ok = true;
#endif
}

// ---- traffic lights: shared 16 s cycle ----
static float tl_t = 0;
static inline int light_state(bool horiz)     // 0 green, 1 yellow, 2 red
{
    float p = fmodf(tl_t, 16.0f);
    if (horiz) return p < 6.5f ? 0 : p < 8 ? 1 : 2;
    return (p >= 8 && p < 14.5f) ? 0 : (p >= 14.5f) ? 1 : 2;
}

static bool must_stop_at_light(const car_t *c)
{
    static const int8_t dx4[4] = {1, 0, -1, 0}, dy4[4] = {0, 1, 0, -1};
    int tx = ((int)c->x) >> 4, ty = ((int)c->y) >> 4;
    int f = lanes[MI(tx, ty)];
    if ((f & (LF_E | LF_W)) && (f & (LF_S | LF_N))) return false;          // already at the intersection
    int nf = lanes[MI(tx + dx4[c->dir], ty + dy4[c->dir])];
    if (!((nf & (LF_E | LF_W)) && (nf & (LF_S | LF_N)))) return false;
    if (light_state(c->dir == 0 || c->dir == 2) == 0) return false;
    float d = c->dir == 0 ? (tx + 1) * 16 - c->x : c->dir == 2 ? c->x - tx * 16 :
              c->dir == 1 ? (ty + 1) * 16 - c->y : c->y - ty * 16;
    return d < 9;
}

// ---- boats ----
#define NBOATS 4
typedef struct { float x, y, a, b; int8_t dir; uint8_t horiz, col; } boat_t;
static boat_t boats[NBOATS];

static void init_boats(void)
{
    for (int i = 0; i < NBOATS; i++) {
        const boat_route_t *r = &BOAT_AT[i];
        boats[i] = (boat_t){ r->x, r->y, r->a, r->b, r->dir, r->horiz, r->col };
    }
}

static void update_boats(float dt)
{
    for (int i = 0; i < NBOATS; i++) {
        boat_t *b = &boats[i];
        float sp = 26 * dt * b->dir;
        if (b->horiz) { b->x += sp; if (b->x > b->b || b->x < b->a) b->dir = -b->dir; }
        else          { b->y += sp; if (b->y > b->b || b->y < b->a) b->dir = -b->dir; }
    }
}

// ---- tire marks and stains (ring buffer) ----
#define NDEC 360
typedef struct { int16_t x, y; uint8_t type; } decal_t;   // 0 tire mark, 1 scorch mark
static decal_t decals[NDEC];
static int dec_head = 0, dec_n = 0;
static void add_decal(float x, float y, int type)
{
    decals[dec_head].x = (int16_t)x; decals[dec_head].y = (int16_t)y; decals[dec_head].type = type;
    dec_head = (dec_head + 1) % NDEC;
    if (dec_n < NDEC) dec_n++;
}

// ---- particles ----
#define NPART 160
typedef struct { float x, y, vx, vy, life, max; uint8_t type; } part_t;
static part_t parts[NPART];
static int part_next = 0;
static void emit(float x, float y, float vx, float vy, float life, int type)
{
    part_t *p = &parts[part_next];
    part_next = (part_next + 1) % NPART;
    p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = p->max = life; p->type = type;
}
static void emit_burst(float x, float y, int n, float speed, float life, int type)
{
    for (int i = 0; i < n; i++) {
        float a = frnd() * 2 * PIf, s = speed * (0.4f + frnd() * 0.6f);
        emit(x, y, cosf(a) * s, sinf(a) * s, life * (0.6f + frnd() * 0.4f), type);
    }
}
static void update_particles(float dt)
{
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        float drag = (p->type == PT_SMOKE || p->type == PT_EXHAUST) ? 0.96f : (p->type == PT_CASING) ? 0.82f : 0.9f;
        p->vx *= drag; p->vy *= drag;
        if (p->type == PT_SMOKE) p->vy -= 6 * dt;
    }
}

// ---- rain (in screen coordinates) ----
#define NRAIN 90
static float rain_x[NRAIN], rain_y[NRAIN];
static void update_rain(float dt)
{
    for (int i = 0; i < NRAIN; i++) {
        if (rain_y[i] == 0 && rain_x[i] == 0) { rain_x[i] = rnd() % SW; rain_y[i] = rnd() % SH; }
        rain_x[i] -= 70 * dt;
        rain_y[i] += 300 * dt;
        if (rain_y[i] > SH) { rain_y[i] -= SH + 10; rain_x[i] = rnd() % (SW + 40); }
        if (rain_x[i] < 0) rain_x[i] += SW;
    }
}

static void update_world_extras(float dt)
{
    update_daylight(dt);
    tl_t += dt;
    update_boats(dt);
    fauna_update(dt);
    update_particles(dt);
    if (raining) update_rain(dt);
    // smoke from burning and damaged cars, exhaust of the player's car
    static float smoke_t = 0;
    smoke_t -= dt;
    if (smoke_t <= 0) {
        smoke_t = 0.12f;
        for (int i = 0; i < NCARS; i++) {
            car_t *c = &cars[i];
            if (c->mode == CAR_WRECK) { if (c->timer < 8 && on_screen(c->x, c->y, 20)) emit(c->x, c->y, frnd() * 8 - 4, -6, 1.6f, PT_SMOKE); continue; }
            if (c->hp < 40 && on_screen(c->x, c->y, 20)) emit(c->x + cosf(c->ang) * 8, c->y + sinf(c->ang) * 8, frnd() * 6 - 3, -8, c->hp <= 0 ? 1.4f : 0.9f, PT_SMOKE);
        }
        if (pcar >= 0 && fabsf(cars[pcar].speed) > 20) {
            car_t *c = &cars[pcar];
            emit(c->x - cosf(c->ang) * 13, c->y - sinf(c->ang) * 13, -cosf(c->ang) * 10, -sinf(c->ang) * 10, 0.5f, PT_EXHAUST);
        }
        // boat wakes
        for (int i = 0; i < NBOATS; i++) {
            boat_t *b = &boats[i];
            if (!on_screen(b->x, b->y, 30)) continue;
            float bx = b->horiz ? -b->dir * 14 : 0, by = b->horiz ? 0 : -b->dir * 14;
            emit(b->x + bx + frnd() * 4 - 2, b->y + by + frnd() * 4 - 2, frnd() * 10 - 5, frnd() * 10 - 5, 0.8f, PT_SPLASH);
        }
    }
}

// ---- rendering ----
static inline uint16_t add_light(uint16_t v, int k, int lr, int lg, int lb)
{
    v = (uint16_t)((v >> 8) | (v << 8));
    int r = ((v >> 11) & 31) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    r += lr * k >> 8; g += lg * k >> 8; b += lb * k >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

// Light spot: radius r, strength k (0..256). Per row only the segment inside the circle is computed,
// falloff (1 - d²/r²)² — from a 33-step table, no per-pixel division.
static uint8_t falloff_lut[33];
static HOT void light_pool(float cx, float cy, float r, int k, int lr, int lg, int lb)
{
    if (!falloff_lut[0]) for (int i = 0; i <= 32; i++) { float f = 1 - i / 32.0f; falloff_lut[i] = (uint8_t)(f * f * 255); }
    int ya = (int)(cy - r), yb = (int)(cy + r);
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    float r2 = r * r, s32 = 32.0f / r2;
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - cy, rem = r2 - dy * dy;
        if (rem <= 0) continue;
        float hw = sqrtf(rem);
        int xa = (int)(cx - hw), xb = (int)(cx + hw);
        if (xa < 0) xa = 0;
        if (xb > SW - 1) xb = SW - 1;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        float dy2 = dy * dy;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - cx;
            int i = (int)((dx * dx + dy2) * s32);
            if (i >= 32) continue;
            int kk = (k * falloff_lut[i]) >> 8;
            if (kk > 4) d[x] = add_light(d[x], kk, lr, lg, lb);
        }
    }
}

// Headlight cone of length L
static HOT void headlights(float sx, float sy, float ca, float sa, int k)
{
    const float L = 46;
    float fx = sx + ca * (12 + L / 2), fy = sy + sa * (12 + L / 2);
    int R = (int)(L / 2 + 14);
    int ya = (int)fy - R, yb = (int)fy + R;
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int xa = (int)fx - R, xb = (int)fx + R;
    if (xa < 0) xa = 0;
    if (xb > SW - 1) xb = SW - 1;
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - sy;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - sx;
            float u = dx * ca + dy * sa - 11;
            if (u < 0 || u > L) continue;
            float v = fabsf(-dx * sa + dy * ca);
            float w = 5 + u * 0.42f;
            if (v > w) continue;
            float f = (1 - u / L) * (1 - v / w * 0.6f);
            d[x] = add_light(d[x], (int)(k * f), 255, 240, 200);
        }
    }
}

static void draw_decals(void)
{
    for (int i = 0; i < dec_n; i++) {
        const decal_t *dc = &decals[i];
        float sx = scr_x(dc->x), sy = scr_y(dc->y);
        if (sy < g_y0 - 16 || sy >= g_y1 + 16 || sx < -16 || sx > SW + 16) continue;
        if (dc->type == 0) {
            int x = (int)sx, y = (int)sy;
            for (int yy = y; yy < y + 2; yy++)
                if (yy >= g_y0 && yy < g_y1)
                    for (int xx = x; xx < x + 2; xx++)
                        if ((unsigned)xx < SW) g_buf[(yy - g_y0) * SW + xx] = darken(g_buf[(yy - g_y0) * SW + xx]);
        } else if (dc->type == 2) {                 // blood
            int x = (int)sx, y = (int)sy;
            fill_rect(x, y, 2, 2, C_BLOOD_D);
            if (i & 1) put_px(x + 2, y + 1, C_BLOOD_D); else put_px(x - 1, y, C_BLOOD_D);
        } else {
            shadow_circle(sx, sy, 13);
            shadow_circle(sx + 1, sy, 8);
        }
    }
}

static void draw_lights(void)
{
    if (night < 0.35f) return;
    int k = (int)((night - 0.35f) / 0.65f * 200);
    for (int vi = 0; vi < nvl; vi++) { int i = vlamp[vi];
        float sx = scr_x(lamps[i].x), sy = scr_y(lamps[i].y);
        if (sy < g_y0 - 30 || sy >= g_y1 + 30 || sx < -30 || sx > SW + 30) continue;
        light_pool(sx, sy, 26, k, 255, 200, 120);
    }
    for (int i = 0; i < NCARS; i++) {
        car_t *c = &cars[i];
        if (c->mode == CAR_WRECK || c->mode == CAR_PARKED || c->mode == CAR_OFF) continue;
        float sx = scr_x(c->x), sy = scr_y(c->y);
        if (sy < g_y0 - 70 || sy >= g_y1 + 70 || sx < -70 || sx > SW + 70) continue;
        headlights(sx, sy, cosf(c->ang), sinf(c->ang), k);
        if (c->mode == CAR_COP || c->mode == CAR_FIRE) {
            bool flip = ((int)(g_time * 6)) & 1;
            light_pool(sx, sy, 22, k / 2, flip ? 255 : 40, 40, flip ? 40 : 255);
        }
        if (c->hp <= 0) light_pool(sx, sy, 30, k, 255, 140, 40);
    }
    if (flash_t > 0) light_pool(scr_x(plx), scr_y(ply), 30, k, 255, 220, 120);
    if (fire_stage == 1) light_pool(scr_x(fire_x), scr_y(fire_y), 60, k * fire_hp / 100 + 20, 255, 130, 40);
    plus_draw_lights(k);
}

static void draw_boats(void)
{
    for (int i = 0; i < NBOATS; i++) {
        const boat_t *b = &boats[i];
        float sx = scr_x(b->x), sy = scr_y(b->y);
        if (sy < g_y0 - 20 || sy >= g_y1 + 20 || sx < -20 || sx > SW + 20) continue;
        float ca = b->horiz ? (float)b->dir : 0, sa = b->horiz ? 0 : (float)b->dir;
        int ya = (int)sy - 16, yb = (int)sy + 16;
        if (ya < g_y0) ya = g_y0;
        if (yb > g_y1 - 1) yb = g_y1 - 1;
        static const uint8_t hulls[3][3] = { {236, 236, 232}, {176, 60, 50}, {60, 80, 120} };
        uint16_t hull = tint565(PK(hulls[b->col][0], hulls[b->col][1], hulls[b->col][2]));
        uint16_t deck = tint565(PK(160, 116, 70)), cabin = tint565(PK(210, 210, 214)), glass = C_GLASS;
        for (int y = ya; y <= yb; y++) {
            float dy = y + 0.5f - sy;
            uint16_t *d = g_buf + (y - g_y0) * SW;
            int wy = (cam_iy + y - SH / 2) & WMASK;
            for (int x = (int)sx - 16; x <= (int)sx + 16; x++) {
                if ((unsigned)x >= SW) continue;
                int wx = (cam_ix + x - SW / 2) & WMASK;
                if (tile_kind(wx >> 4, wy >> 4) != T_WATER) continue;   // not visible under a bridge
                float dx = x + 0.5f - sx;
                float u = dx * ca + dy * sa, v = fabsf(-dx * sa + dy * ca);
                float w = u > 7 ? 5 * (1 - (u - 7) / 7) : 5;
                if (u < -13 || u > 14 || v > w) {
                    if (u < -13 && u > -26 && v < 2 + (-13 - u) * 0.5f && v > (-13 - u) * 0.5f - 1) d[x] = add_light(d[x], 90, 200, 220, 240);  // wake
                    continue;
                }
                uint16_t col = hull;
                if (v < w - 1.5f) col = deck;
                if (u > -6 && u < 3 && v < 3) col = (u > 0) ? glass : cabin;
                d[x] = col;
            }
        }
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < NPART; i++) {
        const part_t *p = &parts[i];
        if (p->life <= 0) continue;
        float sx = scr_x(p->x), sy = scr_y(p->y);
        if (sy < g_y0 - 10 || sy >= g_y1 + 10 || sx < -10 || sx > SW + 10) continue;
        float t = 1 - p->life / p->max;
        switch (p->type) {
        case PT_SMOKE: {
            float r = 2.5f + t * 6;
            // semi-transparent smoke: checkerboard dithering, sparser toward the end
            int ya = (int)(sy - r), yb = (int)(sy + r), xa = (int)(sx - r), xb = (int)(sx + r);
            for (int y = ya; y <= yb; y++) {
                if (y < g_y0 || y >= g_y1) continue;
                for (int x = xa; x <= xb; x++) {
                    if ((unsigned)x >= SW) continue;
                    float dx = x - sx, dy = y - sy;
                    if (dx * dx + dy * dy > r * r) continue;
                    if (((x + y) & 1) && t > 0.4f) continue;
                    if (((x ^ y) & 3) == 0 && t > 0.7f) continue;
                    g_buf[(y - g_y0) * SW + x] = (t < 0.3f) ? C_DARK : C_SMOKE;
                }
            }
            break;
        }
        case PT_EXHAUST: if (((int)(sx + sy)) & 1) put_px((int)sx, (int)sy, C_SMOKE); break;
        case PT_SPARK:   put_px((int)sx, (int)sy, t < 0.5f ? C_WHITE : C_YELLOW); break;
        case PT_DEBRIS:  fill_rect((int)sx, (int)sy, 2, 2, C_DARK); break;
        case PT_BLOOD:   fill_rect((int)sx, (int)sy, 2, 2, C_BLOOD); break;
        case PT_SPLASH:  put_px((int)sx, (int)sy, C_WHITE); break;
        case PT_WATER:   fill_rect((int)sx, (int)sy, 2, 2, t < 0.5f ? C_WHITE : rgb565(150, 200, 255)); break;
        case PT_CASING:  put_px((int)sx, (int)sy, rgb565(230, 190, 60)); put_px((int)sx + ((i & 1) ? 1 : 0), (int)sy + ((i & 1) ? 0 : 1), rgb565(160, 120, 30)); break;
        }
    }
}

static void draw_tree(const prop_t *t)
{
    float bx = scr_x(t->x), by = scr_y(t->y);
    float r = t->r;
    if (by + r + 8 < g_y0 || by - r - 8 >= g_y1 || bx < -r - 8 || bx > SW + r + 8) return;
    shadow_circle(bx + 4, by + 5, r * 0.9f);
    // crown slightly "raised" — parallax like buildings
    float cx = bx + (bx - SW / 2) * 0.05f, cy = by + (by - SH / 2) * 0.05f;
    if (pak_ok) {
        int sz = r <= 6 ? 0 : r <= 10 ? 2 : 4;
        const spr_t *sp = &spr_trees[sz + (hash2(t->x, t->y) & 1)];
        if (sp->px) { blit(sp, 0, (int)cx - sp->w / 2, (int)cy - sp->h / 2, true, false); return; }
    }
    int ya = (int)(cy - r), yb = (int)(cy + r);
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    int icx = (int)cx, icy = (int)cy;
    uint16_t c1 = pal[0][P_TREE3], c2 = pal[0][P_TREE1], c3 = pal[0][P_TREE2];
    float r2 = r * r;
    for (int y = ya; y <= yb; y++) {
        float dy = y + 0.5f - cy;
        uint16_t *d = g_buf + (y - g_y0) * SW;
        for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
            if ((unsigned)x >= SW) continue;
            float dx = x + 0.5f - cx;
            float q = dx * dx + dy * dy;
            // ragged crown edge
            uint32_t h = hash2(x - icx + t->x, y - icy + t->y);
            if (q > r2 - (h & 7) * 1.5f) continue;
            float lgt = (-dx * 0.6f - dy * 0.75f) / r + (int)(h >> 8 & 3) * 0.18f - 0.25f;
            d[x] = lgt > 0.35f ? c1 : lgt > -0.2f ? c2 : c3;
        }
    }
}

// Airplanes on the airport apron (decoration): fuselage, swept wings, tail, engines, shadow
static int plane_shape(float u, float v)          // 0 — none, 1 — fuselage, 2 — wing/tail, 3 — engine, 4 — cockpit, 5 — stripe
{
    float av = fabsf(v);
    if (u > -29 && u < 25 && av < 3.6f) { if (u > 20) return 4; if (av < 0.8f && u < 16) return 5; return 1; }
    if (u >= 25 && u < 31 && av < 3.6f - (u - 25) * 0.55f) return 4;
    if (av < 12.5f && av > 8.5f && u > -1 && u < 7) return 3;
    if (av < 27 && u < 7 - av * 0.42f && u > -4 - av * 0.42f) return 2;
    if (av < 10 && u < -21 - av * 0.3f && u > -28 - av * 0.3f) return 2;
    return 0;
}
static void draw_planes(void)
{
    uint16_t cb = tint565(PK(236, 238, 242)), cw = tint565(PK(196, 202, 210)), ce = tint565(PK(110, 116, 126)), cc = tint565(PK(40, 54, 78)), cs = tint565(PK(40, 90, 190));
    for (int k = 0; k < NPLANES; k++) {
        float bx = scr_x(PLANE_AT[k][0]), by = scr_y(PLANE_AT[k][1]);
        if (by + 70 < g_y0 || by - 70 >= g_y1 || bx < -70 || bx > SW + 70) continue;
        static const int8_t hx[4] = { 1, 0, -1, 0 }, hy[4] = { 0, 1, 0, -1 };
        int h = PLANE_AT[k][2] & 3;
        for (int pass = 0; pass < 2; pass++) {
            float ox = pass ? 0 : 10, oy = pass ? 0 : 12;
            int ya = (int)(by + oy - 52), yb = (int)(by + oy + 52);
            if (ya < g_y0) ya = g_y0;
            if (yb > g_y1 - 1) yb = g_y1 - 1;
            for (int y = ya; y <= yb; y++) {
                uint16_t *d = g_buf + (y - g_y0) * SW;
                for (int x = (int)(bx + ox - 52); x <= (int)(bx + ox + 52); x++) {
                    if ((unsigned)x >= SW) continue;
                    float dx = x + 0.5f - bx - ox, dy = y + 0.5f - by - oy;
                    float u = (dx * hx[h] + dy * hy[h]) * 0.62f, v = (-dx * hy[h] + dy * hx[h]) * 0.62f;   // scale 1.6
                    int sh = plane_shape(u, v);
                    if (!sh) continue;
                    if (!pass) { d[x] = darken(d[x]); continue; }
                    d[x] = sh == 1 ? cb : sh == 2 ? cw : sh == 3 ? ce : sh == 4 ? cc : cs;
                }
            }
        }
    }
}

static void draw_lamp(const prop_t *l)
{
    float bx = scr_x(l->x), by = scr_y(l->y);
    if (by + 10 < g_y0 || by - 10 >= g_y1 || bx < -10 || bx > SW + 10) return;
    float hx = bx + (bx - SW / 2) * 0.07f, hy = by + (by - SH / 2) * 0.07f;   // lamp shade at height
    shadow_circle(bx + 3, by + 4, 1.5f);
    fill_rect((int)hx - 1, (int)hy - 1, 3, 3, C_DARK);
    if (night > 0.35f) { put_px((int)hx, (int)hy, C_WHITE); put_px((int)hx + 1, (int)hy, C_YELLOW); put_px((int)hx, (int)hy + 1, C_YELLOW); }
    else put_px((int)hx, (int)hy, C_GREY);
}

static void draw_traffic_lights(void)
{
    int sh = light_state(true), sv = light_state(false);
    const uint16_t cols[3] = { C_GREEN, C_YELLOW, C_RED };
    int ra = (cam_iy - SH / 2 - 40) >> 4, rb = (cam_iy + SH / 2 + 60) >> 4;
    for (int rr = ra; rr <= rb; rr++)
    for (int i = xing_row[rr & (MAP - 1)]; i < xing_row[(rr & (MAP - 1)) + 1]; i++) {
        float x0 = xing[i][0] * 16.0f, y0 = xing[i][1] * 16.0f;
        float sx = scr_x(x0), sy = scr_y(y0);
        if (sy < g_y0 - 50 || sy >= g_y1 + 20 || sx < -50 || sx > SW + 50) continue;
        // four intersection corners: NW and SE — for horizontal, NE and SW — for vertical
        const float cx[4] = { sx - 4, sx + 35, sx + 35, sx - 4 }, cy[4] = { sy - 4, sy + 35, sy - 4, sy + 35 };
        for (int k = 0; k < 4; k++) {
            int st = (k < 2) ? sh : sv;
            float hx = cx[k] + (cx[k] - SW / 2) * 0.07f, hy = cy[k] + (cy[k] - SH / 2) * 0.07f;
            fill_rect((int)hx - 2, (int)hy - 2, 4, 4, C_DARK);
            fill_rect((int)hx - 1, (int)hy - 1, 2, 2, cols[st]);
        }
    }
}

static void draw_fire(void)
{
    if (fire_stage != 1) return;
    const lot_t *l = &g_lots[fire_lot];
    float hk = BUILDINGS_3D ? l->h10 * 0.0055f : 0;
    float power = fire_hp / 100.0f;
    for (int ty = l->y0; ty <= l->y1; ty++)
        for (int tx = l->x0; tx <= l->x1; tx++) {
            if (hash2(tx, ty) % 3 == 0) continue;
            float bx = SW / 2 + wrapd(tx * 16 + 8.0f - cam_ix), by = SH / 2 + wrapd(ty * 16 + 8.0f - cam_iy);
            float sx = SW / 2 + (bx - SW / 2) * (1 + hk), sy = SH / 2 + (by - SH / 2) * (1 + hk);
            if (sy < g_y0 - 16 || sy >= g_y1 + 16 || sx < -16 || sx > SW + 16) continue;
            float f = sinf(g_time * 11 + tx * 3 + ty * 7);
            float r = (5 + f * 1.5f) * power + 1;
            fill_circle(sx, sy, r + 2, C_RED);
            fill_circle(sx + f, sy - 1, r, C_ORANGE);
            fill_circle(sx + f * 0.5f, sy - 2, r * 0.5f, C_YELLOW);
        }
}

static void draw_rain(void)
{
    if (!raining) return;
    uint16_t c = rgb565(196, 212, 230);
    for (int i = 0; i < NRAIN; i++) {
        int x = (int)rain_x[i], y = (int)rain_y[i];
        for (int k = 0; k < 7; k++) put_px(x + k / 4, y - k, c);
    }
    // splashes on the ground
    for (int i = 0; i < 8; i++) {
        int x = rnd() % SW, y = rnd() % SH;
        if (y >= g_y0 && y < g_y1) { put_px(x, y, c); put_px(x + 1, y, c); }
    }
}

static void plus_frame(void);
static void plus_apply_shake(void);
void game_begin_frame(void)
{
    plus_frame();
    cam_ix = (int)floorf(camX);
    cam_iy = (int)floorf(camY);
    plus_apply_shake();
    // visible trees and lamps — once per frame (~2500 on the big map)
    nvt = nvl = 0;
    {   // trees and lamps are sorted by tile rows — look only at rows near the screen
        int ra = (cam_iy - SH / 2 - 90) >> 4, rb = (cam_iy + SH / 2 + 90) >> 4;
        for (int rr = ra; rr <= rb; rr++) {
            int r = rr & (MAP - 1);
            for (int i = tree_row[r]; i < tree_row[r + 1] && nvt < 255; i++) if (on_screen(trees[i].x, trees[i].y, 60)) vtree[nvt++] = (uint16_t)i;
            for (int i = lamp_row[r]; i < lamp_row[r + 1] && nvl < 255; i++) if (on_screen(lamps[i].x, lamps[i].y, 90)) vlamp[nvl++] = (uint16_t)i;
        }
    }
    build_visible_lots();
    // shore masks of visible tiles (cache) and row flags — land far from the shore doesn't pay for the check
    shore_prepare_view((cam_ix - SW / 2) >> 4, (cam_iy - SH / 2) >> 4, SW / 16 + 2, SH / 16 + 2);
    // water and fountain animation by palette swapping (free for the renderer)
    static const uint8_t wl[4][3] = { {120, 182, 228}, {104, 168, 220}, {92, 156, 212}, {104, 168, 220} };
    static const uint8_t fo[2][3] = { {176, 222, 246}, {226, 244, 252} };
    int ph = ((int)(g_time * 4)) & 3, pf = ((int)(g_time * 8)) & 1;
    uint16_t cw = tint565(PK(wl[ph][0], wl[ph][1], wl[ph][2])), cf = tint565(PK(fo[pf][0], fo[pf][1], fo[pf][2]));
    for (int p = 0; p < NPAL; p++) if (p < 16 || p > 18) { pal[p][P_WATER_L] = cw; pal[p][P_FOUNT] = cf; }
    for (int p = 16; p < 19; p++) {
        int k = WATER_DEPTH_K[p - 16];
        pal[p][P_WATER_L] = tint565(PK(wl[ph][0] * k / 100, wl[ph][1] * k / 100, wl[ph][2] * k / 100));
        pal[p][P_FOUNT] = cf;
    }
    shore_update_colors();
}

static void plus_draw_ground(void);
static void plus_draw_air(void);
static void plus_draw_hud(void);
static void plus_draw_overlay(void);
HOT void game_render_chunk(uint16_t *buf, int y0, int lines)
{
    g_buf = buf; g_y0 = y0; g_y1 = y0 + lines;
    render_bg();
    draw_decals();
    plus_draw_ground();
    draw_boats();
    draw_lights();

    // mission marker
    float msx = scr_x(mis_x), msy = scr_y(mis_y);
    uint16_t mc = (stage == 0) ? C_PICK : C_DROP;
    if (gstate != G_TITLE) {
        ring(msx, msy, 10 + 2 * sinf(g_time * 6), 2.5f, mc);
        fill_circle(msx, msy, 3, mc);
    }
    {   // Pay'n'Spray pad
        float zx = scr_x(spray_x), zy = scr_y(spray_y);
        if (zy + 20 >= g_y0 && zy - 20 < g_y1 && zx > -20 && zx < SW + 20) {
            uint16_t pk = rgb565(255, 90, 200);
            for (int k = 0; k < 32; k += 4) {
                fill_rect((int)zx - 16 + k, (int)zy - 16, 2, 3, pk); fill_rect((int)zx - 16 + k, (int)zy + 13, 2, 3, pk);
                fill_rect((int)zx - 16, (int)zy - 16 + k, 3, 2, pk); fill_rect((int)zx + 13, (int)zy - 16 + k, 3, 2, pk);
            }
            draw_text_sh((int)zx - 9, (int)zy - 3, "SPRAY", 1, pk);
        }
    }
    for (int i = 0; i < NPICK; i++) if (pick[i].active) draw_pickup(&pick[i]);
    draw_poi_doors();

    // corpses under everything else
    for (int i = 0; i < NACT; i++) if (act[i].kind && act[i].state == S_DEAD) draw_corpse(&act[i]);
    fauna_draw();
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (!a->kind || a->state == S_DEAD) continue;
        bool mv = !(a->kind == A_COP && a->timer > 0) && a->state != S_IDLE;
        if (a->kind == A_COP && (a->flags & AF_SWAT)) draw_person(scr_x(a->x), scr_y(a->y), a->ang, C_DARK, C_DARK, true, mv, i * 1.7f, 7);
        else if (a->kind == A_COP) draw_person(scr_x(a->x), scr_y(a->y), a->ang, C_COP, C_COP_HEAD, stars >= 3, mv, i * 1.7f, 6);
        else draw_person(scr_x(a->x), scr_y(a->y), a->ang, actor_col(a), C_SKIN, a->kind == A_GANG, mv, i * 1.7f, a->color);
        if ((a->flags & AF_VICTIM) && rob_state == RB_ROB) draw_text_sh((int)scr_x(a->x) - 1, (int)scr_y(a->y) - 14, "!", 2, C_YELLOW);
        if ((a->flags & AF_ROBBER) && rob_state == RB_FLEE) draw_text_sh((int)scr_x(a->x) - 1, (int)scr_y(a->y) - 13, "$", 1, C_RED);
        if (a->flags & AF_CUFFED) fill_rect((int)scr_x(a->x) - 2, (int)scr_y(a->y) + 3, 5, 2, C_GREY);
    }
    // player on foot
    if (ON_FOOT && gstate != G_TITLE) {
        float x = scr_x(plx), y = scr_y(ply);
        bool vis = !(invuln_t > 0 && (((int)(g_time * 10)) & 1));
        if (vis) {
            if (gstate == G_WASTED) {
                fill_circle(x + 1, y + 1, 6, C_BLOOD);
            }
            ring(x, y, 7, 1.5f, C_WHITE);
            draw_person(x, y, pang, hurt_t > 0 ? C_RED : C_PLAYER, C_SKIN, weapon != W_FIST, p_moving, 0, 11);
        }
        if (flash_t > 0) fill_circle(x + cosf(pang) * 8, y + sinf(pang) * 8, 3, C_YELLOW);
    }
    for (int i = 0; i < NCARS; i++) draw_car(&cars[i], i);
    if (pcar >= 0 && flash_t > 0) {
        car_t *c = &cars[pcar];
        fill_circle(scr_x(c->x) + cosf(c->ang) * 15, scr_y(c->y) + sinf(c->ang) * 15, 3, C_YELLOW);
    }

    // bullets
    for (int i = 0; i < NBUL; i++) {
        if (bul[i].life <= 0) continue;
        int x = (int)scr_x(bul[i].x), y = (int)scr_y(bul[i].y);
        uint16_t c = bul[i].owner ? C_RED : C_YELLOW;
        put_px(x, y, c); put_px(x + 1, y, c); put_px(x, y + 1, c); put_px(x + 1, y + 1, c);
    }
    // effects
    for (int i = 0; i < NFX; i++) {
        if (fx[i].t <= 0) continue;
        float x = scr_x(fx[i].x), y = scr_y(fx[i].y);
        if (fx[i].kind == 0) {
            float t = fx[i].t / 0.9f;
            float r = 8 + t * 30;
            if (t < 0.6f) fill_circle(x, y, r, C_ORANGE);
            fill_circle(x, y, r * (0.7f - t * 0.5f), C_YELLOW);
            if (t > 0.4f) ring(x, y, r, 3, C_SMOKE);
        } else {
            fill_circle(x, y, 2, C_WHITE);
        }
    }

    // trees, lamps and traffic lights — above people, below roofs
    draw_planes();
    for (int vi = 0; vi < nvt; vi++) draw_tree(&trees[vtree[vi]]);
    for (int vi = 0; vi < nvl; vi++) draw_lamp(&lamps[vlamp[vi]]);
    draw_traffic_lights();
    // volumetric buildings over people and cars — as in GTA1, walls hide what's behind them
    if (BUILDINGS_3D) for (int i = 0; i < nvis; i++) draw_building(&vis[i]);
    draw_fire();
    draw_particles();          // smoke rises above the roofs
    plus_draw_air();
    fauna_draw_air();
    draw_poi_signs();
    draw_rain();

    if (gstate != G_TITLE && !cinema && !replay) { draw_hud(msx, msy, mc); plus_draw_hud(); if (gstate == G_PLAY) draw_shop_panel(); }
    if (!replay) render_overlay();
    plus_draw_overlay();
}

// ================= Platform glue =================
void game_set_status(bool pad_connected, bool sd_ok, const char *pad_name)
{
    if (pad_connected && !st_pad) game_message("GAMEPAD CONNECTED", 2);
    if (!pad_connected && st_pad) game_message("GAMEPAD LOST", 2);
    st_pad = pad_connected;
    st_sd = sd_ok;
    if (pad_name) {
        int j = 0;
        for (int i = 0; pad_name[i] && j < (int)sizeof(st_pad_name) - 1; i++) {
            char ch = pad_name[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            st_pad_name[j++] = ch;
        }
        st_pad_name[j] = 0;
    }
}

void game_get_save(game_save_t *s)
{
    s->money = money; s->kills = kills; s->deliveries = deliveries; s->best_wanted = best_wanted;
    s->flags = (super_won ? 1 : 0) | plus_save_flags();
}

bool game_take_save_request(void) { bool r = save_req; save_req = false; return r; }
bool game_take_screenshot_request(void) { bool r = shot_req; shot_req = false; return r; }

// Debug/screenshots: 1 — advance time by 3 hours, 2 — toggle rain
void game_debug(int what)
{
    if (what == 3 && gstate == G_PLAY) { cinema = !cinema; return; }
    if (what == 4) { if (replay) { pressed = BTN_START; replay_tick(0); } else if (gstate == G_PLAY || gstate == G_PAUSE) replay_start(); return; }
    if (what == 1) { day_t += DAY_LEN / 8; if (day_t >= DAY_LEN) day_t -= DAY_LEN; tint_timer = 0; }
    if (what == 2) { raining = !raining; weather_t = 120; tint_timer = 0; }
}

#include "game_plus.inc"
#include "game_fauna.inc"

// Loading/diagnostics screen (called from main.c before the game starts). Lines separated by \n:
// '#' at the start — red, '+' — green, '*' — large heading, '!' — yellow.
void game_draw_boot(uint16_t *buf, int y0, int lines, const char *text)
{
    g_buf = buf; g_y0 = y0; g_y1 = y0 + lines;
    uint16_t bg = rgb565(8, 10, 16);
    for (int i = 0; i < SW * lines; i++) buf[i] = bg;
    int y = 8;
    const char *p = text;
    char line[80];
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < 79) n++;
        memcpy(line, p, n); line[n] = 0;
        p += n; if (*p == '\n') p++;
        const char *t = line;
        uint16_t c = rgb565(225, 228, 235);
        int sc = 1;
        if (line[0] == '#') { c = rgb565(255, 80, 70); t++; }
        else if (line[0] == '+') { c = rgb565(90, 230, 110); t++; }
        else if (line[0] == '!') { c = rgb565(255, 210, 60); t++; }
        else if (line[0] == '*') { c = rgb565(255, 150, 30); sc = 3; t++; }
        draw_text(8, y, t, sc, c);
        y += sc * 5 + 6;
    }
}
