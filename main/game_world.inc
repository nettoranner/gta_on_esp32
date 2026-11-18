// game_world.inc — 1024x1024 tile world (16384x16384 px).
// Included from game.c (single translation unit).
//
// Macro layout (tiles):
//   * Old city (A)  x 70..399,  y 330..659 — downtown with skyscrapers, business belt, residential blocks,
//                        villas on the west bank and northwest hills, apartment blocks in the northeast,
//                        ghetto across the river and industrial zone in the southeast, warehouses at the river mouth, park and square.
//   * Port city (B) x 640..959, y 400..739 — business center (glass), "warm" blocks, ghetto in the east,
//                        warehouses and container port with piers in the southern bay.
//   * Airport           x 560..890, y 140..288 — two runways, taxiways, apron, terminal, hangars, tower.
//   * Countryside: fields (wheat, seedlings, plowland, meadows), forests, lakes, river, villages, farms, gas stations.
//   * Highways: HW1 (west–east, y 496) between the cities, HW2 (north–south, x 500); country roads in a ring.
// Generation runs on a PC (tools/worldgen -> WORLD.BIN on SD); the ESP32 only loads the finished world.

#define WORLD_SEED 0x20261114u

// ---- cities and zones ----
#define CA_X0 70
#define CA_X1 399
#define CA_Y0 330
#define CA_Y1 659
#define CA_CX 240
#define CA_CY 480
#define RIVER_X 213                       // river through city A: tiles 213..218
#define CB_X0 640
#define CB_X1 959
#define CB_Y0 400
#define CB_Y1 739
#define CB_CX 800
#define CB_CY 565
#define AP_X0 560
#define AP_X1 890
#define AP_Y0 140
#define AP_Y1 288

// road types (temporary layer g_rt)
enum { RT_NONE = 0, RT_CITY, RT_HWY, RT_COUNTRY, RT_PORT };
// land use (temporary layer g_use)
enum { U_NONE = 0, U_COUNTRY, U_VERGE, U_FIELD_H, U_FIELD_V, U_PLOUGH_H, U_PLOUGH_V, U_MEADOW, U_FOREST,
       U_RUNWAY, U_TAXI, U_APRON, U_LOT, U_GAS, U_YARD, U_PIER, U_AIRGRASS };
#define IS_FIELD(u) ((u) >= U_FIELD_H && (u) <= U_MEADOW)

// ground tones (palette rows R_TONE0 + tone; TN_NORMAL — row 0)
enum { TN_NORMAL = 0, TN_CLEAN, TN_RICH, TN_WORN, TN_DIRTY, TN_GRIME, TN_RURAL, TN_WHEAT, TN_CROP, TN_PLOUGH,
       TN_MEADOW, TN_FOREST, TN_AIRPORT, TN_HWY, TN_WARM, TN_WARM_CLEAN, TN_WARM_DIRTY, TN_N };
static inline int tone_row(int t) { return t == TN_NORMAL ? 0 : R_TONE0 + t; }

typedef struct { uint16_t pos, a, b; uint8_t type; } road_t;   // 2-tile road: pos — first tile, a..b — extent
#define MAX_ROADS 1600
static road_t *g_hr, *g_vr;
static int g_nhr, g_nvr;
static uint8_t *g_use, *g_rt, *g_dist;                        // temporary generation layers [MAP*MAP]
static uint32_t *g_q;                                         // flood-fill queue (tiles)


static void add_hr(int pos, int a, int b, int type) { if (g_nhr < MAX_ROADS && b > a) g_hr[g_nhr++] = (road_t){ (uint16_t)pos, (uint16_t)a, (uint16_t)b, (uint8_t)type }; }
static void add_vr(int pos, int a, int b, int type) { if (g_nvr < MAX_ROADS && b > a) g_vr[g_nvr++] = (road_t){ (uint16_t)pos, (uint16_t)a, (uint16_t)b, (uint8_t)type }; }

// ---- points of interest (tiles); nearest suitable tile is searched during the game ----
enum { LM_START, LM_HOSPITAL, LM_POLICE, LM_SPRAY, LM_PHONE0, LM_PHONE1, LM_PHONE2, LM_GANG1, LM_GANG2, LM_HELI,
       LM_POI_GUNS, LM_POI_FOOD, LM_POI_CARS, LM_POI_CASINO, LM_POI_BANK, LM_N };
static const int16_t LM_AT[LM_N][2] = {
    { 229, 462 },                 // start: center of city A
    { 150, 560 }, { 292, 470 },   // hospital, police station
    { 352, 612 },                 // Pay'n'Spray — industrial zone A
    { 257, 506 }, { 238, 612 }, { 296, 616 },   // phones: square, green ghetto, purple ghetto
    { 238, 612 }, { 296, 616 },   // gang "homes"
    { 257, 512 },                 // helipad — near the square
    { 150, 446 }, { 262, 598 }, { 362, 470 }, { 806, 560 }, { 226, 472 },   // weapons, CHIKKO, car dealer, casino (city B), bank
};

// ---- islands, stashes, lighthouse ----
static const int16_t STASH_AT[NSTASH][2] = { { 805, 925 }, { 160, 223 }, { 10, 470 }, { 430, 788 }, { 1004, 66 } };
#define LIGHT_TX 1007
#define LIGHT_TY 66

// ---- boats: routes over water (px) ----
typedef struct { float x, y, a, b; int8_t dir; uint8_t horiz, col; } boat_route_t;
static const boat_route_t BOAT_AT[] = {
    { 20 * 16 + 8, 3000, 1600, 14400, 1, 0, 0 },        // western strait (across the torus seam)
    { 9000, 880 * 16 + 8, 690 * 16, 920 * 16, -1, 1, 1 },  // port bay
    { 4000, 16 * 16 + 8, 1600, 14400, 1, 1, 2 },        // northern sea
    { 6600, 776 * 16 + 8, 400 * 16, 460 * 16, 1, 1, 0 },   // lake in the south
};

// ---- aircraft parked on the apron (px, heading) ----
static const int16_t PLANE_AT[][3] = { { 668 * 16, 240 * 16, 1 }, { 706 * 16, 240 * 16, 1 }, { 752 * 16, 240 * 16, 1 }, { 790 * 16, 240 * 16, 1 }, { 842 * 16, 214 * 16 - 100, 0 } };
#define NPLANES ((int)(sizeof(PLANE_AT) / sizeof(PLANE_AT[0])))

// ---- villages ----
typedef struct { int16_t x0, y0, x1, y1; } rect_t;
static const rect_t VILLAGES[] = { { 60, 110, 150, 196 }, { 322, 872, 432, 940 } };
#define NVILL ((int)(sizeof(VILLAGES) / sizeof(VILLAGES[0])))
static inline bool in_rect(const rect_t *r, int x, int y) { return x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1; }

// ---- districts ----
static const rect_t PARK_A = { 268, 404, 300, 436 }, PLAZA_A = { 246, 500, 266, 520 };
static const rect_t PARK_B = { 742, 452, 776, 484 }, PLAZA_B = { 814, 586, 836, 606 };

static int city_at(int x, int y)
{
    if (x >= CA_X0 - 36 && x <= CA_X1 + 36 && y >= CA_Y0 - 30 && y <= CA_Y1 + 30) return 1;
    if (x >= CB_X0 - 36 && x <= CB_X1 + 36 && y >= CB_Y0 - 30 && y <= 860) return 2;
    return 0;
}
static inline bool in_city_rect(int x, int y)
{
    return (x >= CA_X0 && x <= CA_X1 + 1 && y >= CA_Y0 && y <= CA_Y1 + 1) || (x >= CB_X0 && x <= CB_X1 + 1 && y >= CB_Y0 && y <= 812);
}

static int district_A(int x, int y)
{
    float n = (fbm2(x * 0.03f, y * 0.03f, 21) - 0.5f) * 24.0f;
    float dx = x - CA_CX, dy = y - CA_CY, r = sqrtf(dx * dx + dy * dy) + n;
    if (in_rect(&PARK_A, x, y)) return D_PARK;
    if (in_rect(&PLAZA_A, x, y)) return D_PLAZA;
    if (x < 104) return D_RICH;                                  // west bank — villas
    if (r < 34) return D_TOWERS;
    if (r < 66) return D_DOWNTOWN;
    if (y > 584 + n * 0.5f && x > 314) return D_INDUSTRY;        // southeast industrial zone
    if (y > 604 && x < RIVER_X) return D_INDUSTRY;               // warehouses at the river mouth
    if (y > 560 + n * 0.5f && x >= RIVER_X) return D_GHETTO;     // ghetto across the river
    if (y < 404 && x < 200) return D_RICH;                       // northwest hills
    if ((y < 430 && x > 296) || (x > 344 && y < 584)) return D_SLEEP;   // apartment blocks
    return D_RESID;
}
static int district_B(int x, int y)
{
    float n = (fbm2(x * 0.03f + 40, y * 0.03f, 23) - 0.5f) * 24.0f;
    float dx = x - CB_CX, dy = y - CB_CY, r = sqrtf(dx * dx + dy * dy) + n;
    if (y >= CB_Y1 + 1) return D_PORT;
    if (in_rect(&PARK_B, x, y)) return D_PARK;
    if (in_rect(&PLAZA_B, x, y)) return D_PLAZA;
    if (r < 40) return D_TOWERS;
    if (r < 70) return D_DOWNTOWN;
    if (y > 676 + n * 0.5f) return D_INDUSTRY;
    if (x > 890 + n * 0.5f && y > 540) return D_GHETTO;
    if (y < 470) return D_SLEEP;
    if (x < 704) return D_RICH;
    return D_RESID;
}
static int district(int cx, int cy)
{
    if (cx >= AP_X0 && cx <= AP_X1 && cy >= AP_Y0 && cy <= AP_Y1) return D_AIRPORT;
    for (int i = 0; i < NVILL; i++) if (in_rect(&VILLAGES[i], cx, cy)) return D_VILLAGE;
    int c = city_at(cx, cy);
    if (c == 1) return district_A(cx, cy);
    if (c == 2) return district_B(cx, cy);
    return D_COUNTRY;
}
static int tone_of(int d, int city)
{
    bool b = city == 2;
    switch (d) {
    case D_TOWERS: case D_DOWNTOWN: case D_PLAZA: return b ? TN_WARM_CLEAN : TN_CLEAN;
    case D_RICH: return TN_RICH;
    case D_PARK: return b ? TN_WARM_CLEAN : TN_RICH;
    case D_SLEEP: return TN_WORN;
    case D_GHETTO: return b ? TN_WARM_DIRTY : TN_DIRTY;
    case D_INDUSTRY: case D_PORT: return TN_GRIME;
    case D_COUNTRY: case D_VILLAGE: return TN_RURAL;
    case D_AIRPORT: return TN_AIRPORT;
    default: return b ? TN_WARM : TN_NORMAL;
    }
}
// district gang: 0 — none, 1 — green (west of ghetto A), 2 — purple (east of ghetto A and ghetto B)
#define ZG_BYTES ((MAP / 16) * (MAP / 16))
static uint8_t *zgang;                 // [ZG_BYTES], on the heap
static inline int gang_zone(int tx, int ty) { return zgang[(((ty) & (MAP - 1)) >> 4) * (MAP / 16) + (((tx) & (MAP - 1)) >> 4)]; }

// ---------------------------------------------------------------- city street grid
static int grid_lines(int a, int b, int c, const int *forced, int nf, int lo_ex, int hi_ex, int *out)
{
    int n = 0, p = a;
    out[n++] = a;
    while (1) {
        int d = abs(p - c);
        int s = d < 40 ? 11 + rnd() % 3 : d < 80 ? 13 + rnd() % 3 : d < 140 ? 15 + rnd() % 4 : 17 + rnd() % 5;
        int np = p + s;
        for (int i = 0; i < nf; i++) if (forced[i] > p + 6 && forced[i] <= np + 3) { np = forced[i]; break; }
        if (np > lo_ex && np < hi_ex) {                   // skip the strip along the river
            bool f = false;
            for (int i = 0; i < nf; i++) if (forced[i] == np) f = true;
            if (!f) { p = np; continue; }
        }
        if (np >= b - 7) break;
        out[n++] = np;
        p = np;
    }
    out[n++] = b;
    return n;
}

typedef struct { int nv, nh; int vx[40], hy[40]; uint8_t vseg[40][40], hseg[40][40]; } grid_t;   // vseg[i][j]: line i between hy[j] and hy[j+1]

static int grid_arms(const grid_t *g, int i, int j)
{
    int n = 0;
    if (j > 0 && g->vseg[i][j - 1]) n++;
    if (j < g->nh - 1 && g->vseg[i][j]) n++;
    if (i > 0 && g->hseg[j][i - 1]) n++;
    if (i < g->nv - 1 && g->hseg[j][i]) n++;
    return n;
}

static void build_city(int city)
{
    grid_t *g = calloc(1, sizeof(grid_t));
    int x0, x1, y0, y1, cx, cy;
    if (city == 1) { x0 = CA_X0; x1 = CA_X1 - 1; y0 = CA_Y0; y1 = CA_Y1 - 1; cx = CA_CX; cy = CA_CY; }
    else           { x0 = CB_X0; x1 = CB_X1 - 1; y0 = CB_Y0; y1 = CB_Y1 - 1; cx = CB_CX; cy = CB_CY; }
    if (city == 1) {
        static const int fv[] = { 160, RIVER_X - 3, RIVER_X + 7, 240, 290, 330 }, fh[] = { 404, 437, 496, 560, 604 };
        g->nv = grid_lines(x0, x1, cx, fv, 6, RIVER_X - 9, RIVER_X + 13, g->vx);
        g->nh = grid_lines(y0, y1, cy, fh, 5, -1, -1, g->hy);
    } else {
        static const int fv[] = { 700, 742, 776, 800, 836, 900 }, fh[] = { 452, 485, 496, 586, 607, 676 };
        g->nv = grid_lines(x0, x1, cx, fv, 6, -1, -1, g->vx);
        g->nh = grid_lines(y0, y1, cy, fh, 6, -1, -1, g->hy);
    }
    for (int i = 0; i < g->nv; i++) for (int j = 0; j < g->nh - 1; j++) g->vseg[i][j] = 1;
    for (int j = 0; j < g->nh; j++) for (int i = 0; i < g->nv - 1; i++) g->hseg[j][i] = 1;
    // irregular city outline: corner and edge blocks (superellipse + noise) fall out to the countryside.
    // A street stays if a city block lies on one of its sides (boundary is a closed ring, no dead ends).
    {
        static uint8_t in[40][40];
        float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f, mx0 = (x0 + x1) * 0.5f, my0 = (y0 + y1) * 0.5f;
        for (int i = 0; i < g->nv - 1; i++)
            for (int j = 0; j < g->nh - 1; j++) {
                float ccx = (g->vx[i] + g->vx[i + 1]) * 0.5f, ccy = (g->hy[j] + g->hy[j + 1]) * 0.5f;
                float dx = fabsf(ccx - mx0) / hw, dy = fabsf(ccy - my0) / hh;
                float sup = sqrtf(sqrtf(dx * dx * dx * dx + dy * dy * dy * dy)) + (fbm2(ccx * 0.02f, ccy * 0.02f, 51 + city) - 0.5f) * 0.42f;
                in[i][j] = sup < 0.94f;
            }
        // blocks adjacent to country roads and highways are always urban
        static const int16_t ATT_A[][2] = { { 70, 330 }, { 240, 330 }, { 399, 496 }, { 70, 659 }, { 330, 659 }, { 240, 496 } };
        static const int16_t ATT_B[][2] = { { 640, 496 }, { 959, 400 }, { 640, 739 }, { 700, 739 }, { 800, 739 }, { 900, 739 }, { 800, 565 } };
        const int16_t (*att)[2] = city == 1 ? ATT_A : ATT_B;
        int natt = city == 1 ? 6 : 7;
        static uint8_t in0[40][40];
        memcpy(in0, in, sizeof(in0));
        int ci = 0, cj = 0;
        for (int i = 0; i < g->nv - 1; i++) if (g->vx[i] <= cx) ci = i;
        for (int j = 0; j < g->nh - 1; j++) if (g->hy[j] <= cy) cj = j;
        for (int a = 0; a < natt; a++)
            for (int i = 0; i < g->nv - 1; i++)
                for (int j = 0; j < g->nh - 1; j++)
                    if (att[a][0] >= g->vx[i] - 1 && att[a][0] <= g->vx[i + 1] + 2 && att[a][1] >= g->hy[j] - 1 && att[a][1] <= g->hy[j + 1] + 2) {
                        // "strip" of blocks along the road from the city junction (up to the first urban block)
                        int ii = i, jj = j;
                        while (!in0[ii][jj]) {
                            in[ii][jj] = 1;
                            if (abs(ii - ci) >= abs(jj - cj)) ii += ii < ci ? 1 : -1; else jj += jj < cj ? 1 : -1;
                            if (ii == ci && jj == cj) break;
                        }
                        in[i][j] = 1;
                    }
        if (city == 2) for (int i = 0; i < g->nv - 1; i++) { int px = (g->vx[i] + g->vx[i + 1]) / 2; if (px >= 690 && px <= 910) in[i][g->nh - 2] = 1; }   // at the port
        // isolated "holes" inside the city are not needed
        for (int i = 1; i < g->nv - 2; i++)
            for (int j = 1; j < g->nh - 2; j++)
                if (!in[i][j] && in[i - 1][j] && in[i + 1][j] && in[i][j - 1] && in[i][j + 1]) in[i][j] = 1;
#define CIN(i, j) ((i) >= 0 && (j) >= 0 && (i) < g->nv - 1 && (j) < g->nh - 1 && in[i][j])
        for (int i = 0; i < g->nv; i++) for (int j = 0; j < g->nh - 1; j++) if (!CIN(i - 1, j) && !CIN(i, j)) g->vseg[i][j] = 0;
        for (int j = 0; j < g->nh; j++) for (int i = 0; i < g->nv - 1; i++) if (!CIN(i, j - 1) && !CIN(i, j)) g->hseg[j][i] = 0;
#undef CIN
    }
    // river in city A: bridges only on every third street and on "through" arterials
    if (city == 1) {
        int ri = -1;
        for (int i = 0; i < g->nv - 1; i++) if (g->vx[i] == RIVER_X - 3) ri = i;
        if (ri >= 0)
            for (int j = 1; j < g->nh - 1; j++) {
                int y = g->hy[j];
                if (y == 496 || y == 404 || y == 560 || (j % 3) == 0) continue;
                g->hseg[j][ri] = 0;
            }
    }
    // merging blocks into "superblocks" (industrial, villas, park); no dead ends left
    const rect_t *pk = city == 1 ? &PARK_A : &PARK_B;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 1; i < g->nv - 1; i++)
            for (int j = 0; j < g->nh - 1; j++) {
                bool v = pass == 0;
                int mx = v ? g->vx[i] : (g->vx[i] + g->vx[i + 1]) / 2;
                int my = v ? (g->hy[j] + g->hy[j + 1]) / 2 : g->hy[j + 1];
                int jj = v ? j : j + 1;
                if (!v && (jj >= g->nh - 1 || i >= g->nv - 1)) continue;
                if (v ? !g->vseg[i][j] : !g->hseg[jj][i]) continue;
                int d = city == 1 ? district_A(mx, my) : district_B(mx, my);
                int pr = d == D_INDUSTRY ? 30 : d == D_RICH ? 16 : d == D_PORT ? 30 : d == D_TOWERS ? 0 : 6;
                bool park = mx > pk->x0 + 2 && mx < pk->x1 - 2 && my > pk->y0 + 2 && my < pk->y1 - 2;
                if (park) pr = 100;
                int pv = v ? g->vx[i] : g->hy[jj];
                if (pv == RIVER_X - 3 || pv == RIVER_X + 7 || pv == 496 || pv == 240 || pv == 330 || pv == 800 || pv == 700 || pv == 900) pr = park ? 100 : 0;
                if ((int)(rnd() % 100) >= pr) continue;
                // segment ends after removal must keep >= 3 "arms" (otherwise a T-junction would turn into a corner/dead end)
                int ai, aj, bi, bj;
                if (v) { ai = i; aj = j; bi = i; bj = j + 1; } else { ai = i; aj = jj; bi = i + 1; bj = jj; }
                if (grid_arms(g, ai, aj) < 4 && !park) continue;
                if (grid_arms(g, bi, bj) < 4 && !park) continue;
                if (grid_arms(g, ai, aj) < 3 || grid_arms(g, bi, bj) < 3) continue;
                if (v) g->vseg[i][j] = 0; else g->hseg[jj][i] = 0;
            }
    // rasterize into the road list: consecutive segments of a line are merged
    int rt = RT_CITY;
    for (int i = 0; i < g->nv; i++)
        for (int j = 0; j < g->nh - 1;) {
            if (!g->vseg[i][j]) { j++; continue; }
            int k = j;
            while (k < g->nh - 1 && g->vseg[i][k]) k++;
            add_vr(g->vx[i], g->hy[j], g->hy[k] + 1, rt);
            j = k;
        }
    for (int j = 0; j < g->nh; j++)
        for (int i = 0; i < g->nv - 1;) {
            if (!g->hseg[j][i]) { i++; continue; }
            int k = i;
            while (k < g->nv - 1 && g->hseg[j][k]) k++;
            add_hr(g->hy[j], g->vx[i], g->vx[k] + 1, rt);
            i = k;
        }
    if (city == 2) {
        // port: three streets down to the docks and a waterfront (y = 800)
        add_vr(700, CB_Y1 - 1, 801, RT_PORT); add_vr(800, CB_Y1 - 1, 801, RT_PORT); add_vr(900, CB_Y1 - 1, 801, RT_PORT);
        add_hr(800, 700, 901, RT_PORT);
    }
    free(g);
}

static void build_roads(void)
{
    g_nhr = g_nvr = 0;
    build_city(1);
    build_city(2);
    // highways
    add_hr(496, CA_X1 - 1, CB_X0 + 1, RT_HWY);           // HW1 west–east
    add_vr(500, 118, 931, RT_HWY);                       // HW2 north–south
    // country roads
    add_hr(118, 70, 959, RT_COUNTRY);                    // northern
    add_hr(292, 500, 959, RT_COUNTRY);                   // to the airport terminal
    add_vr(958, 118, CB_Y0 + 1, RT_COUNTRY);             // eastern
    add_vr(70, 118, CA_Y0 + 1, RT_COUNTRY);              // northwest
    add_vr(240, 118, CA_Y0 + 1, RT_COUNTRY);             // northern entrance to city A
    add_vr(70, CA_Y1 - 1, 931, RT_COUNTRY);              // southwest
    add_vr(330, CA_Y1 - 1, 931, RT_COUNTRY);             // southern entrance to city A
    add_hr(930, 70, 601, RT_COUNTRY);                    // southern
    add_vr(600, CB_Y1 - 1, 931, RT_COUNTRY);             // southeast
    add_hr(CB_Y1 - 1, 600, CB_X0 + 1, RT_COUNTRY);       // to the southern ring of city B
    // villages
    add_hr(150, 70, 111, RT_COUNTRY); add_vr(110, 118, 151, RT_COUNTRY);
    add_hr(890, 330, 421, RT_COUNTRY); add_vr(420, 890, 931, RT_COUNTRY);
}

// ---------------------------------------------------------------- water
static float river_x(int y)
{
    if (y < 247) return -100;
    if (y < 330) return 160.0f + (RIVER_X - 160.0f) * ssm((y - 247) / 83.0f);
    if (y <= 660) return RIVER_X;
    return RIVER_X + 16.0f * sinf((y - 660) / 38.0f);
}
static inline bool in_ell(float x, float y, float cx, float cy, float rx, float ry)
{
    float a = (x - cx) / rx, b = (y - cy) / ry;
    return a * a + b * b < 1.0f;
}
static bool island_at(int x, int y)
{
    float n = (vnoise(x * 0.4f, y * 0.4f, 41) - 0.5f) * 0.5f;
    if (in_ell(x, y, 805, 925, 10 + n * 4, 6 + n * 3)) return true;       // island in the bay
    if (in_ell(x, y, 160, 223, 5.5f, 4)) return true;                     // island in the northern lake
    if (in_ell(x, y, 10, 470, 7 + n * 4, 5 + n * 3)) return true;         // western islet
    if (in_ell(x, y, 430, 788, 5.5f, 4)) return true;                     // island in the southern lake
    if (in_ell(x, y, 1005, 66, 5 + n * 3, 4 + n * 2)) return true;        // rock with the lighthouse
    return false;
}

static void make_water(void)
{
    // buffer around future roads: the sea does not enter there
    uint8_t *keep = g_use;      // temporary (U_* not set yet)
    memset(keep, 0, MAP * MAP);
    for (int i = 0; i < g_nhr; i++)
        for (int x = g_hr[i].a - 3; x <= g_hr[i].b + 3; x++)
            for (int y = g_hr[i].pos - 3; y <= g_hr[i].pos + 4; y++) keep[MI(x, y)] = 1;
    for (int i = 0; i < g_nvr; i++)
        for (int y = g_vr[i].a - 3; y <= g_vr[i].b + 3; y++)
            for (int x = g_vr[i].pos - 3; x <= g_vr[i].pos + 4; x++) keep[MI(x, y)] = 1;
    for (int y = 0; y < MAP; y++)
        for (int x = 0; x < MAP; x++) {
            int i = MI(x, y);
            // mainland: rounded rectangle 34..990 (corner radius 110) with a jagged coast:
            // large bays and capes (up to 30 tiles inland / 12 outward) + small ripples
            float qx = fabsf(x - 512.0f) - (478.0f - 110.0f), qy = fabsf(y - 512.0f) - (478.0f - 110.0f);
            float ox_ = qx > 0 ? qx : 0, oy_ = qy > 0 ? qy : 0, mq = qx > qy ? qx : qy;
            float de = -(sqrtf(ox_ * ox_ + oy_ * oy_) + (mq < 0 ? mq : 0) - 110.0f);     // tiles from the mainland edge inward
            float sb = -12.0f + 42.0f * sramp(fbm2(x * 0.0085f, y * 0.0085f, 3), 0.28f, 0.82f) + 7.0f * (fbm2(x * 0.035f, y * 0.035f, 4) - 0.5f) + (vnoise(x * 0.18f, y * 0.18f, 5) - 0.5f) * 2.5f;
            bool sea = de < sb && !keep[i];
            if (de < -14) sea = true;
            // port bay, piers and straight waterfront
            float bn = (fbm2(x * 0.05f, y * 0.05f, 6) - 0.5f) * 0.22f;
            if (in_ell(x, y, 805, 905, 165 * (1 + bn), 92 * (1 + bn)) && !keep[i]) sea = true;
            if (y >= 813 && y <= 850 && x >= 692 && x <= 900) {
                sea = true;
                for (int p = 0; p < 5; p++) { int px = 700 + p * 40; if (x >= px && x <= px + 7) sea = false; }
            }
            if (y >= 803 && y < 813 && x >= 690 && x <= 910) sea = false;
            if (!sea && y >= 803 && y <= 850 && x >= 690 && x <= 910) { g_kind[i] = T_PLAZA; continue; }   // concrete docks and piers
            if (island_at(x, y)) sea = false;
            if (sea) { g_kind[i] = T_WATER; g_sea[i] = 1; continue; }
            // lakes and river
            bool lake = in_ell(x, y, 160, 225, 34 + (vnoise(x * 0.2f, y * 0.2f, 7) - 0.5f) * 6, 22) ||
                        in_ell(x, y, 430, 788, 40 + (vnoise(x * 0.2f, y * 0.2f, 8) - 0.5f) * 6, 26);
            float rx = river_x(y);
            bool river = rx > 0 && x >= (int)floorf(rx + 0.5f) && x <= (int)floorf(rx + 0.5f) + 5;
            if ((lake || river) && !island_at(x, y)) g_kind[i] = T_WATER;
            // village ponds
            if (in_ell(x, y, 130, 175, 6, 4) || in_ell(x, y, 372, 910, 5, 4)) g_kind[i] = T_WATER;
        }
    // rocks: northern and northeastern coast, islets
    for (int y = 0; y < MAP; y++)
        for (int x = 0; x < MAP; x++) {
            int i = MI(x, y);
            if (g_kind[i] == T_WATER) continue;
            bool near = false;
            for (int dy = -2; dy <= 2 && !near; dy++)
                for (int dx = -2; dx <= 2; dx++) if (g_sea[MI(x + dx, y + dy)]) { near = true; break; }
            if (!near) continue;
            float r = fbm2(x * 0.03f + 7, y * 0.03f, 13);
            if ((y < 120 && r > 0.52f) || (x > 960 && y < 360 && r > 0.45f) || in_ell(x, y, 1005, 66, 9, 8) || in_ell(x, y, 10, 470, 10, 8)) g_rock[i] = 1;
        }
}

// ---------------------------------------------------------------- airport
static void make_airport(void)
{
    for (int y = AP_Y0; y <= AP_Y1; y++)
        for (int x = AP_X0; x <= AP_X1; x++) { int i = MI(x, y); g_kind[i] = T_PARK; g_use[i] = U_AIRGRASS; }
    // runways (4 tiles) and taxiways (2 tiles)
    for (int y = 160; y <= 163; y++) for (int x = 584; x <= 866; x++) { g_kind[MI(x, y)] = T_ROAD; g_use[MI(x, y)] = U_RUNWAY; }
    for (int y = 214; y <= 217; y++) for (int x = 600; x <= 850; x++) { g_kind[MI(x, y)] = T_ROAD; g_use[MI(x, y)] = U_RUNWAY; }
    for (int y = 188; y <= 189; y++) for (int x = 592; x <= 858; x++) { g_kind[MI(x, y)] = T_ROAD; g_use[MI(x, y)] = U_TAXI; }
    static const int TXC[3] = { 600, 728, 856 };
    for (int k = 0; k < 3; k++)
        for (int y = 164; y <= (k == 1 ? 230 : 213); y++)
            for (int x = TXC[k]; x <= TXC[k] + 1; x++) {
                int i = MI(x, y);
                if (g_use[i] == U_RUNWAY) continue;
                g_kind[i] = T_ROAD; g_use[i] = U_TAXI;
            }
    // apron, terminal parking
    for (int y = 231; y <= 262; y++) for (int x = 640; x <= 862; x++) { g_kind[MI(x, y)] = T_PLAZA; g_use[MI(x, y)] = U_APRON; }
    for (int y = 263; y <= 289; y++) for (int x = 640; x <= 820; x++) { g_kind[MI(x, y)] = T_PLAZA; g_use[MI(x, y)] = (y >= 274 && y <= 286 && x >= 650 && x <= 810) ? U_LOT : U_APRON; }
    g_city_ctx = 0; g_dist_ctx = D_AIRPORT;
    add_lot(660, 263, 800, 270, ST_FLAT);            // terminal
    g_lots[g_nlots].pal = R_TERMINAL; g_lots[g_nlots].h10 = 24;
    add_lot(626, 236, 628, 238, ST_TOWER);           // control tower
    g_lots[g_nlots].pal = R_GLASS3; g_lots[g_nlots].h10 = 72;
    add_lot(832, 226, 852, 239, ST_HANGAR);
    add_lot(832, 244, 852, 257, ST_HANGAR);
}

// ---------------------------------------------------------------- city park
static void make_park(int x0, int y0, int x1, int y1)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f + 2, rx = (x1 - x0) * 0.26f, ry = (y1 - y0) * 0.20f;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (gk(x, y) != T_PARK) continue;
            float ex = (x - cx) / rx, ey = (y - cy) / ry, e = ex * ex + ey * ey;
            if (e < 1.0f) g_kind[MI(x, y)] = T_WATER;
            else {
                float ex2 = (x - cx) / (rx + 2.2f), ey2 = (y - cy) / (ry + 2.2f), e2 = ex2 * ex2 + ey2 * ey2;
                if (e2 > 0.80f && e2 < 1.20f) g_kind[MI(x, y)] = T_PLAZA;
            }
        }
    int mx = (int)cx, my = (int)cy;
    for (int x = x0; x <= x1; x++) if (gk(x, my) == T_PARK && fabsf(x - cx) > rx + 2) g_kind[MI(x, my)] = T_PLAZA;
    for (int y = y0; y <= y1; y++) if (gk(mx, y) == T_PARK && fabsf(y - cy) > ry + 2) g_kind[MI(mx, y)] = T_PLAZA;
    int fy = (int)(cy - ry - 6);
    for (int y = fy - 2; y <= fy + 2; y++) for (int x = mx - 2; x <= mx + 2; x++) if (gk(x, y) == T_PARK) g_kind[MI(x, y)] = T_PLAZA;
    if (nfount < MAX_FOUNT) { fount_x[nfount] = mx * 16 + 8; fount_y[nfount] = fy * 16 + 8; nfount++; }
}

// ---------------------------------------------------------------- fields and forests
static void field_bsp(int x0, int y0, int x1, int y1, int depth)
{
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    int lim = 22 + (int)(rnd() % 30);
    if ((w > lim || h > lim) && depth < 14) {
        if (w >= h) { int s = x0 + w / 3 + (int)(rnd() % (w / 3 + 1)); field_bsp(x0, y0, s - 1, y1, depth + 1); field_bsp(s, y0, x1, y1, depth + 1); }
        else        { int s = y0 + h / 3 + (int)(rnd() % (h / 3 + 1)); field_bsp(x0, y0, x1, s - 1, depth + 1); field_bsp(x0, s, x1, y1, depth + 1); }
        return;
    }
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    float fo = fbm2(cx * 0.011f, cy * 0.011f, 31);
    bool forest_zone = (cx < 220 && cy < 320 && cy > 200) || (cx < 320 && cy > 700) || (cx > 900 && cy < 380 && cy > 140) || (cx > 250 && cx < 480 && cy < 300 && cy > 140);
    int u;
    if (fo > (forest_zone ? 0.40f : 0.64f)) u = U_FOREST;
    else {
        int r = (int)(rnd() % 100);
        bool hz = rnd() & 1;
        if (r < 30) u = hz ? U_FIELD_H : U_FIELD_V;
        else if (r < 52) u = hz ? U_FIELD_H : U_FIELD_V;       // seedlings: same texture, different tone
        else if (r < 72) u = hz ? U_PLOUGH_H : U_PLOUGH_V;
        else u = U_MEADOW;
        // tone is encoded in the "second" value: 30..51 — seedlings (see tone pass)
        if (r >= 30 && r < 52) u |= 0x40;
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int i = MI(x, y);
            if (g_use[i] == U_COUNTRY) g_use[i] = (uint8_t)u;
        }
}

static int g_nfarms = 0;
// Farm near the nearest country road
static void place_farm(int px, int py)
{
    for (int r = 0; r < 40; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int x = px + dx, y = py + dy;
                if (gk(x, y) != T_ROAD || g_rt[MI(x, y)] != RT_COUNTRY) continue;
                // side of the road: try 4 directions
                static const int8_t ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
                for (int d = 0; d < 4; d++) {
                    int bx = x + ox[d] * 3, by = y + oy[d] * 3;          // offset past the shoulder
                    int ax0 = ox[d] < 0 ? bx - 11 : ox[d] > 0 ? bx : bx - 5, ay0 = oy[d] < 0 ? by - 9 : oy[d] > 0 ? by : by - 4;
                    int ax1 = ax0 + 11, ay1 = ay0 + 9;
                    bool ok = true;
                    for (int yy = ay0 - 1; yy <= ay1 + 1 && ok; yy++)
                        for (int xx = ax0 - 1; xx <= ax1 + 1; xx++) {
                            int i = MI(xx, yy);
                            if (gk(xx, yy) != T_PARK || !(IS_FIELD(g_use[i] & 0x3F) || g_use[i] == U_COUNTRY || g_use[i] == U_FOREST || g_use[i] == U_VERGE)) { ok = false; break; }
                        }
                    if (!ok) continue;
                    for (int yy = ay0; yy <= ay1; yy++) for (int xx = ax0; xx <= ax1; xx++) { g_kind[MI(xx, yy)] = T_PLAZA; g_use[MI(xx, yy)] = U_YARD; }
                    g_city_ctx = 0; g_dist_ctx = D_COUNTRY;
                    add_lot(ax0 + 1, ay0 + 1, ax0 + 3, ay0 + 2, ST_HOUSE); g_lots[g_nlots].pal = R_FARM;
                    add_lot(ax0 + 6, ay0 + 4, ax0 + 9, ay0 + 7, ST_BARN);
                    add_lot(ax0 + 1, ay0 + 6, ax0 + 2, ay0 + 7, ST_INDUSTRY); g_lots[g_nlots].pal = R_HANGAR; g_lots[g_nlots].h10 = 26;   // silo
                    g_nfarms++;
                    return;
                }
            }
}

// Gas station: lot by the road + shop
static void place_gas(int x0, int y0, int x1, int y1, int sx0, int sy0)
{
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { g_kind[MI(x, y)] = T_PLAZA; g_use[MI(x, y)] = U_GAS; }
    g_city_ctx = 0; g_dist_ctx = D_COUNTRY;
    add_lot(sx0, sy0, sx0 + 3, sy0 + 2, ST_FLAT); g_lots[g_nlots].pal = R_GAS; g_lots[g_nlots].h10 = 10;
}

// ---------------------------------------------------------------- world layout
static void make_layout(void)
{
    const int N = MAP * MAP;
    memset(g_kind, 0xFF, N);
    memset(g_rt, 0, N);
    memset(g_dist, 0, N);
    g_nlots = 0;
    nfount = 0;
    build_roads();
    make_water();                 // uses g_use as a temporary buffer
    memset(g_use, 0, N);
    for (int y = 803; y <= 850; y++) for (int x = 690; x <= 910; x++) if (gk(x, y) == T_PLAZA) { g_use[MI(x, y)] = U_PIER; g_dist[MI(x, y)] = (uint8_t)(D_PORT | (2 << 5)); }
    g_city_ctx = 2; g_dist_ctx = D_PORT;
    for (int p = 0; p < 5; p++)                         // containers on the piers
        for (int y = 816; y <= 844; y += 5) { int px = 700 + p * 40; if (rnd() % 4) add_lot(px + 1, y, px + 2, y + 3, ST_CONTAINER); if (rnd() % 3) add_lot(px + 5, y, px + 6, y + 3, ST_CONTAINER); }
    for (int x = 694; x <= 904; x += 7) if (rnd() % 3) add_lot(x, 806, x + 3, 807, ST_CONTAINER);
    // lighthouse on the rock
    g_city_ctx = 0; g_dist_ctx = D_COUNTRY;
    add_lot(LIGHT_TX - 1, LIGHT_TY - 1, LIGHT_TX, LIGHT_TY, ST_FLAT); g_lots[g_nlots].pal = 14; g_lots[g_nlots].h10 = 64;
    make_airport();
    // roads (bridges over water)
    for (int i = 0; i < g_nhr; i++)
        for (int x = g_hr[i].a; x <= g_hr[i].b; x++)
            for (int k = 0; k < 2; k++) {
                int j = MI(x, g_hr[i].pos + k);
                uint8_t *t = &g_kind[j];
                *t = (*t == T_WATER || *t == (T_ROAD | 0x80)) ? (T_ROAD | 0x80) : T_ROAD;
                if (!g_rt[j] || g_hr[i].type == RT_HWY) g_rt[j] = g_hr[i].type;
            }
    for (int i = 0; i < g_nvr; i++)
        for (int y = g_vr[i].a; y <= g_vr[i].b; y++)
            for (int k = 0; k < 2; k++) {
                int j = MI(g_vr[i].pos + k, y);
                uint8_t *t = &g_kind[j];
                *t = (*t == T_WATER || *t == (T_ROAD | 0x80)) ? (T_ROAD | 0x80) : T_ROAD;
                if (!g_rt[j] || g_vr[i].type == RT_HWY) g_rt[j] = g_vr[i].type;
            }
    // gas stations, motel (before sidewalks so the lots don't get a curb)
    place_gas(503, 482, 514, 494, 508, 482);
    place_gas(944, 296, 955, 307, 948, 304);
    place_gas(243, 196, 252, 207, 248, 196);
    for (int y = 500; y <= 512; y++) for (int x = 480; x <= 498; x++) { g_kind[MI(x, y)] = T_PLAZA; g_use[MI(x, y)] = U_LOT; }
    add_lot(482, 506, 495, 509, ST_FLAT); g_lots[g_nlots].pal = R_WARM3; g_lots[g_nlots].h10 = 12;   // motel
    // sidewalks along city roads and waterfronts in the cities; shoulders on country roads
    for (int y = 0; y < MAP; y++)
        for (int x = 0; x < MAP; x++) {
            int i = MI(x, y);
            if (g_kind[i] != 0xFF) continue;
            int near = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int j = MI(x + dx, y + dy), k = g_kind[j];
                    if ((k & 0x7F) == T_ROAD && k != 0xFF && k != 0xFE && k != 0xFC) {
                        int rt = g_rt[j];
                        if (rt == RT_CITY || rt == RT_PORT) near |= 1; else if (rt) near |= 2;
                    } else if (k == T_WATER && in_city_rect(x, y)) near |= 1;
                }
            if (near & 1) g_kind[i] = 0xFE;
            else if (near & 2) g_kind[i] = 0xFC;
        }
    for (int i = 0; i < N; i++) {
        if (g_kind[i] == 0xFE) g_kind[i] = T_SIDEWALK;
        else if (g_kind[i] == 0xFC) { g_kind[i] = T_PARK; g_use[i] = U_VERGE; }
    }

    // blocks: connected "unset" areas -> districts (by tile voting within the area)
    for (int sy = 0; sy < MAP; sy++)
        for (int sx = 0; sx < MAP; sx++) {
            if (g_kind[MI(sx, sy)] != 0xFF) continue;
            int x0 = sx, x1 = sx, y0 = sy, y1 = sy, head = 0, tail = 0;
            g_q[tail++] = MI(sx, sy);
            g_kind[MI(sx, sy)] = 0xFD;
            while (head < tail) {
                int i = g_q[head++], x = i & (MAP - 1), y = i >> MSH;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                static const int8_t ox[4] = {1, -1, 0, 0}, oy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; k++) {
                    int j = MI(x + ox[k], y + oy[k]);
                    if (g_kind[j] == 0xFF) { g_kind[j] = 0xFD; g_q[tail++] = j; }
                }
            }
            int votes[D_NUM] = {0};
            int step = tail / 61 + 1;
            for (int k = 0; k < tail; k += step) { int i = g_q[k]; votes[district(i & (MAP - 1), i >> MSH)]++; }
            int d = D_COUNTRY, best = -1;
            for (int k = 0; k < D_NUM; k++) if (votes[k] > best) { best = votes[k]; d = k; }
            if (d != D_COUNTRY && d != D_AIRPORT) { int dc = district((x0 + x1) / 2, (y0 + y1) / 2); if (dc != D_COUNTRY && dc != D_AIRPORT && votes[dc] * 3 >= best) d = dc; }
            if (d == D_AIRPORT) d = D_COUNTRY;
            int city = city_at((x0 + x1) / 2, (y0 + y1) / 2);
            if (d == D_COUNTRY || d == D_VILLAGE) city = 0;
            for (int k = 0; k < tail; k++) g_dist[g_q[k]] = (uint8_t)(d | (city << 5));
            if (d == D_COUNTRY) {
                for (int k = 0; k < tail; k++) { g_kind[g_q[k]] = T_PARK; g_use[g_q[k]] = U_COUNTRY; }
                continue;
            }
            int yard = (d == D_DOWNTOWN || d == D_INDUSTRY || d == D_PLAZA || d == D_TOWERS || d == D_PORT) ? T_PLAZA : T_PARK;
            for (int k = 0; k < tail; k++) g_kind[g_q[k]] = (uint8_t)yard;
            g_city_ctx = city; g_dist_ctx = d;
            if (d == D_PARK) make_park(x0, y0, x1, y1);
            else if (d == D_PLAZA) {
                int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                add_lot(x0 + 1, y0 + 1, x1 - 1, y0 + 3, ST_FLAT);
                add_lot(x0 + 1, y1 - 3, x0 + 3, y1 - 1, ST_FLAT);
                add_lot(x1 - 3, y1 - 3, x1 - 1, y1 - 1, ST_FLAT);
                if (nfount < MAX_FOUNT) { fount_x[nfount] = cx * 16 + 8; fount_y[nfount] = cy * 16 + 8; nfount++; }
            } else split_area(x0, y0, x1, y1, d, yard);
        }
    for (int i = 0; i < N; i++) if (g_kind[i] == 0xFF || g_kind[i] == 0xFD) g_kind[i] = T_PARK;
    // fields and forests on countryside tiles
    field_bsp(0, 0, MAP - 1, MAP - 1, 0);
    for (int i = 0; i < N; i++) if (g_use[i] == U_COUNTRY) g_use[i] = U_MEADOW;
    for (int y = 0; y < MAP; y++) for (int x = 0; x < MAP; x++) if (island_at(x, y) && gk(x, y) == T_PARK) g_use[MI(x, y)] = U_MEADOW;   // islands — meadow
    // farms along country roads (every 50..80 tiles, away from cities and villages)
    g_nfarms = 0;
    for (int pass = 0; pass < 2; pass++) {
        const road_t *R = pass ? g_vr : g_hr;
        int nr = pass ? g_nvr : g_nhr;
        for (int i = 0; i < nr; i++) {
            if (R[i].type != RT_COUNTRY) continue;
            for (int t = R[i].a + 25; t < R[i].b - 25; t += 50 + (int)(rnd() % 30)) {
                int x = pass ? R[i].pos : t, y = pass ? t : R[i].pos;
                if (city_at(x, y) || district(x, y) == D_VILLAGE || district(x, y) == D_AIRPORT) continue;
                place_farm(x + (pass ? ((rnd() & 1) ? 7 : -6) : 0), y + (pass ? 0 : ((rnd() & 1) ? 7 : -6)));
            }
        }
    }
    printf("world: %d farms, %d+%d road segments\n", g_nfarms, g_nhr, g_nvr);

    // lanes for AI: from the road list (only on road tiles)
    memset(lanes, 0, N);
    for (int i = 0; i < g_nhr; i++)
        for (int x = g_hr[i].a; x <= g_hr[i].b; x++) {
            int j0 = MI(x, g_hr[i].pos), j1 = MI(x, g_hr[i].pos + 1);
            if ((g_kind[j0] & 0x7F) == T_ROAD) lanes[j0] |= LF_W;
            if ((g_kind[j1] & 0x7F) == T_ROAD) lanes[j1] |= LF_E;
        }
    for (int i = 0; i < g_nvr; i++)
        for (int y = g_vr[i].a; y <= g_vr[i].b; y++) {
            int j0 = MI(g_vr[i].pos, y), j1 = MI(g_vr[i].pos + 1, y);
            if ((g_kind[j0] & 0x7F) == T_ROAD) lanes[j0] |= LF_S;
            if ((g_kind[j1] & 0x7F) == T_ROAD) lanes[j1] |= LF_N;
        }
    // gang by 16x16 blocks
    memset(zgang, 0, ZG_BYTES);
    for (int by = 0; by < MAP / 16; by++)
        for (int bx = 0; bx < MAP / 16; bx++) {
            int g1 = 0, g2 = 0;
            for (int y = by * 16; y < by * 16 + 16; y += 2)
                for (int x = bx * 16; x < bx * 16 + 16; x += 2) {
                    int dd = g_dist[MI(x, y)];
                    if ((dd & 31) != D_GHETTO) continue;
                    if ((dd >> 5) == 1 && x < 267) g1++; else g2++;
                }
            zgang[by * (MAP / 16) + bx] = g1 > 12 ? 1 : g2 > 12 ? 2 : 0;
        }
    for (int k = 0; k < NSTASH; k++) { stash_x[k] = STASH_AT[k][0] * 16 + 8; stash_y[k] = STASH_AT[k][1] * 16 + 8; }
    light_x = LIGHT_TX * 16 + 8; light_y = LIGHT_TY * 16 + 8;
}

// Ground (palette row) / road tone of a tile
static int tile_tone(int x, int y)
{
    int i = MI(x, y), k = g_kind[i] & 0x7F, u = g_use[i] & 0x3F;
    if (u == U_RUNWAY || u == U_TAXI || u == U_APRON || u == U_LOT || u == U_AIRGRASS) return TN_AIRPORT;
    if (k == T_BUILDING) return TN_NORMAL;
    if (k == T_ROAD) {
        int rt = g_rt[i];
        if (rt == RT_HWY) return TN_HWY;
        if (rt == RT_COUNTRY) return TN_RURAL;
        int c = in_city_rect(x, y) ? city_at(x, y) : 0;
        if (!c) return TN_RURAL;
        return tone_of(c == 1 ? district_A(x, y) : district_B(x, y), c);
    }
    if (u == U_FOREST) return TN_FOREST;
    if (u == U_FIELD_H || u == U_FIELD_V) return (g_use[i] & 0x40) ? TN_CROP : TN_WHEAT;
    if (u == U_PLOUGH_H || u == U_PLOUGH_V) return TN_PLOUGH;
    if (u == U_MEADOW) return TN_MEADOW;
    if (u == U_GAS) return TN_AIRPORT;
    if (u == U_PIER) return TN_GRIME;
    if (u == U_VERGE || u == U_YARD || u == U_COUNTRY) return TN_RURAL;
    int dd = g_dist[i];
    if (dd) return tone_of(dd & 31, dd >> 5);
    if (k == T_SIDEWALK || k == T_PLAZA) {
        int c = in_city_rect(x, y) ? city_at(x, y) : 0;
        if (c) return tone_of(c == 1 ? district_A(x, y) : district_B(x, y), c);
    }
    return TN_RURAL;
}
