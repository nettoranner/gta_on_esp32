// game_shore.inc - per-pixel shore masks (water / sand / grass).
// Included from game.c (single translation unit, shared static context).
//
// The 1024x1024 map: masks for all shore tiles cannot be stored (~3 MB), so:
//  * at generation (PC) only bitmaps are computed: sea, rocks, "has mask", "sand" (128 KB each);
//  * during play the mask is built on the fly for on-screen tiles and kept in a 32x32-tile cache
//    (direct-mapped: tile (x, y) -> cell (x & 31, y & 31); the 21x16-tile screen fits without collisions).
// Game logic stays per-tile; masks are only drawn over the tile.

static uint8_t *g_sea;       // 1 = sea; only during generation
static uint8_t *g_rock;      // 1 = rocks; only during generation
static uint8_t *g_wrow;      // [MAP*MAP] water palette row (0 or 16..18); only during generation

// bitmaps (saved in WORLD.BIN)
#define BM_BYTES (MAP * MAP / 8)
static uint8_t *bm_sea, *bm_rock, *bm_shore, *bm_sand;
#define BM_GET(b, i) (((b)[(i) >> 3] >> ((i) & 7)) & 1)
#define BM_SET(b, i) ((b)[(i) >> 3] |= (uint8_t)(1 << ((i) & 7)))

static inline int tile_kind(int tx, int ty);
static inline int shk(int tx, int ty) { return g_kind ? (g_kind[MI(tx, ty)] & 0x7F) : tile_kind(tx, ty); }
static inline int sh_sea(int i) { return g_sea ? g_sea[i] : BM_GET(bm_sea, i); }
static inline int sh_rock(int i) { return g_rock ? g_rock[i] : BM_GET(bm_rock, i); }

// ---------------------------------------------------------------- noise
static inline float vhash(int x, int y, int s) { return (hash2(x * 7 + s * 1013, y * 13 + s * 577) & 0xFFFF) / 65535.0f; }
static inline float ssm(float t) { return t * t * (3.0f - 2.0f * t); }
static float vnoise(float x, float y, int s)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = ssm(x - xi), fy = ssm(y - yi);
    float a = vhash(xi, yi, s), b = vhash(xi + 1, yi, s), c = vhash(xi, yi + 1, s), d = vhash(xi + 1, yi + 1, s);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
static float fbm2(float x, float y, int s) { return vnoise(x, y, s) * 0.62f + vnoise(x * 2.3f + 5.1f, y * 2.3f + 1.7f, s + 1) * 0.38f; }
static float sramp(float v, float a, float b)
{
    float t = (v - a) / (b - a);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return ssm(t);
}

// ---------------------------------------------------------------- stashes and lighthouse (positions set by the world generator))
#define NSTASH 5
static float stash_x[NSTASH], stash_y[NSTASH];
static const int16_t STASH_PAY[NSTASH] = { 2500, 3000, 2000, 1500, 1000 };
static const char *const STASH_NAME[NSTASH] = { "BAY STASH", "LAKE STASH", "ISLET STASH", "POND STASH", "LIGHTHOUSE STASH" };
static float light_x = -1, light_y = -1;      // lighthouse (center, px); -1 = none

// ---------------------------------------------------------------- shore masks
#define SHORE_BYTES 128
#define SHC_W 32                         // mask cache 32x32 tiles
static uint8_t *shc_d;                   // SHC_W*SHC_W * 128 bytes
static int32_t *shc_tag;                 // [SHC_W*SHC_W] tile number in the cell, or -1
static uint8_t shore_row[MAP];           // 1 = this tile row on screen has shore masks (once per frame)
static uint16_t shore_col[16];
static int shore_gen_frame = 0;          // how many masks were built in the last frame (for debugging)
// mask pixel code -> palette color: 0 = keep the tile
static const uint8_t SH_PAL[16] = {
    0, P_WATER, P_WATER_L, P_SHALLOW, P_FOAM, P_WSAND, P_SAND, P_SAND_L,
    P_GRASS_D, P_GRASS1, P_GRASS2, P_TREE1, P_TREE2, P_QUAY, P_ROCK_D, P_SIDEWALK,
};
enum { SC_BASE = 0, SC_WATER, SC_WATER_L, SC_SHALLOW, SC_FOAM, SC_WSAND, SC_SAND, SC_SAND_L,
       SC_GRASS_D, SC_GRASS1, SC_GRASS2, SC_BUSH, SC_BUSH_D, SC_ROCK, SC_ROCK_D, SC_PAVE };

static void shore_update_colors(void)
{
    for (int i = 1; i < 16; i++) shore_col[i] = pal[0][SH_PAL[i]];
}

static float gw[5][16], gws_inv[256];
static float sw8[8], sw16[16];
static bool shore_tab_ok = false;

static inline bool is_solid_kind(int k) { return k == T_ROAD || k == T_BUILDING; }

static void shore_tables(void)
{
    if (shore_tab_ok) return;
    const float sg = 11.0f;            // Gaussian blur of the water mask, σ = 11 px
    float gws[16];
    for (int o = 0; o < 5; o++)
        for (int p = 0; p < 16; p++) {
            float px = p + 0.5f;
            float a = (16.0f * (o - 2) - px) / sg, b = (16.0f * (o - 2) + 16.0f - px) / sg;
            gw[o][p] = 0.5f * (erff(b * 0.70710678f) - erff(a * 0.70710678f));
        }
    for (int p = 0; p < 16; p++) { gws[p] = 0; for (int o = 0; o < 5; o++) gws[p] += gw[o][p]; }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) gws_inv[y * 16 + x] = 1.0f / (gws[x] * gws[y]);
    for (int i = 0; i < 8; i++) sw8[i] = ssm(i / 8.0f);
    for (int i = 0; i < 16; i++) sw16[i] = ssm(i / 16.0f);
    shore_tab_ok = true;
}

// Candidate tile: not road/building, and the 5x5 window has both water and land
static bool shore_candidate(int tx, int ty)
{
    if (is_solid_kind(shk(tx, ty))) return false;
    bool hw = false, hn = false;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) { if (shk(tx + dx, ty + dy) == T_WATER) hw = true; else hn = true; }
    return hw && hn;
}

// Tile mask: 16x16 SC_* codes -> out (128 bytes, 4 bits per pixel). Returns the number of non-empty pixels.
static int shore_mask(int tx, int ty, uint8_t *out, int *sand_out)
{
    uint8_t code[256], prot[256];
    float fn[256], H[5][16];
    int kt = shk(tx, ty);
    // blurred water mask
    for (int j = 0; j < 5; j++) {
        bool wt[5];
        for (int i = 0; i < 5; i++) wt[i] = shk(tx + i - 2, ty + j - 2) == T_WATER;
        for (int lx = 0; lx < 16; lx++) {
            float s = 0;
            for (int i = 0; i < 5; i++) if (wt[i]) s += gw[i][lx];
            H[j][lx] = s;
        }
    }
    // noise: two grids (8 and 16 px), tied to the world so they join across tiles
    float nA[3][3], nB[2][2];
    for (int b = 0; b < 3; b++) for (int a = 0; a < 3; a++) nA[b][a] = vhash(tx * 2 + a, ty * 2 + b, 11) * 2 - 1;
    for (int b = 0; b < 2; b++) for (int a = 0; a < 2; a++) nB[b][a] = vhash(tx + a, ty + b, 12) * 2 - 1;

    int nk[3][3];
    bool nsol[3][3];
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) { nk[dy + 1][dx + 1] = shk(tx + dx, ty + dy); nsol[dy + 1][dx + 1] = is_solid_kind(nk[dy + 1][dx + 1]); }
    // "pure" water tile: all 8 neighbors are water, tile pixels already match the mask away from the shore
    bool plain = kt == T_WATER;
    for (int dy = 0; dy < 3 && plain; dy++) for (int dx = 0; dx < 3; dx++) if (nk[dy][dx] != T_WATER) { plain = false; break; }
    bool open = false;
    for (int dy = -2; dy <= 2 && !open; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int i = MI(tx + dx, ty + dy);
            if (sh_sea(i) && shk(tx + dx, ty + dy) == T_WATER) { open = true; break; }
        }

    memset(code, 0, sizeof(code));
    memset(prot, 0, sizeof(prot));
    for (int ly = 0; ly < 16; ly++)
        for (int lx = 0; lx < 16; lx++) {
            int pi = ly * 16 + lx;
            int wx = tx * 16 + lx, wy = ty * 16 + ly;
            float F = 0;
            for (int j = 0; j < 5; j++) F += H[j][lx] * gw[j][ly];
            F *= gws_inv[pi];
            float na = nA[ly >> 3][lx >> 3] + (nA[ly >> 3][(lx >> 3) + 1] - nA[ly >> 3][lx >> 3]) * sw8[lx & 7];
            float nb2 = nA[(ly >> 3) + 1][lx >> 3] + (nA[(ly >> 3) + 1][(lx >> 3) + 1] - nA[(ly >> 3) + 1][lx >> 3]) * sw8[lx & 7];
            float n8 = na + (nb2 - na) * sw8[ly & 7];
            float ca = nB[0][0] + (nB[0][1] - nB[0][0]) * sw16[lx], cb = nB[1][0] + (nB[1][1] - nB[1][0]) * sw16[lx];
            float n16 = ca + (cb - ca) * sw16[ly];
            float f = F + 0.15f * (0.6f * n8 + 0.4f * n16);
            fn[pi] = f;
            // 4 px strip next to roads and buildings is left untouched
            bool pr = (lx < 4 && nsol[1][0]) || (lx > 11 && nsol[1][2]) || (ly < 4 && nsol[0][1]) || (ly > 11 && nsol[2][1]) ||
                      (lx < 4 && ly < 4 && nsol[0][0]) || (lx > 11 && ly < 4 && nsol[0][2]) ||
                      (lx < 4 && ly > 11 && nsol[2][0]) || (lx > 11 && ly > 11 && nsol[2][2]);
            if (pr) { prot[pi] = 1; continue; }
            uint32_t hp = hash2(wx, wy);
            int c;
            if (f >= 0.5f) {
                float d = f - 0.5f;
                if (d > 0.012f && d < 0.05f && (hp & 7) < 5) c = SC_FOAM;
                else if (d < (open ? 0.17f : 0.09f)) c = SC_SHALLOW;
                else if (plain) c = SC_BASE;
                else c = (((lx + (ly >> 2) * 5 + (tx & 1) * 7) & 15) < 3 && (ly & 3) == 1) ? SC_WATER_L : SC_WATER;
            } else {
                // nearest suitable land tile (for a pixel in a "water" tile)
                int Lk = kt, lt_dx = 0, lt_dy = 0;
                if (kt == T_WATER) {
                    int bestd = 1000;
                    Lk = -1;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            if ((dx | dy) == 0 || nk[dy + 1][dx + 1] == T_WATER || nsol[dy + 1][dx + 1]) continue;
                            int px_ = dx < 0 ? lx : dx > 0 ? 15 - lx : 0, py_ = dy < 0 ? ly : dy > 0 ? 15 - ly : 0;
                            int dist = px_ * px_ + py_ * py_;
                            if (dist < bestd) { bestd = dist; Lk = nk[dy + 1][dx + 1]; lt_dx = dx; lt_dy = dy; }
                        }
                    if (Lk < 0) { c = SC_WATER; goto put; }
                }
                {
                    bool rock = sh_rock(MI(tx + lt_dx, ty + lt_dy)) != 0;
                    float e = 0.5f - f;
                    if (rock) {
                        c = (e < 0.07f || (hp & 7) == 0) ? SC_ROCK_D : SC_ROCK;
                    } else if (Lk == T_PARK) {
                        bool grass_base = kt != T_WATER;
                        int gr = grass_base ? SC_BASE : (hash2(wx >> 2, wy >> 2) & 1) ? SC_GRASS1 : SC_GRASS2;
                        if (open) {
                            if (e < 0.09f) c = SC_WSAND;
                            else if (e < 0.30f) c = (hp & 15) == 0 ? SC_SAND_L : SC_SAND;
                            else if (e < 0.44f) c = ((hp & 15) == 3) ? SC_GRASS1 : SC_SAND_L;
                            else c = gr;
                        } else {
                            if (e < 0.05f) c = SC_WSAND;
                            else if (e < 0.15f) c = SC_GRASS_D;
                            else c = gr;
                        }
                    } else {
                        // sidewalk / plaza: stone edge
                        if (e < 0.05f) c = SC_ROCK;
                        else c = (kt == T_WATER) ? SC_PAVE : SC_BASE;
                    }
                }
            }
        put:
            code[pi] = (uint8_t)c;
        }
    // bushes and rocks along the edge
    if (kt == T_PARK) {
        for (int k = 0; k < 4; k++) {
            uint32_t h = hash2(tx * 4 + k + 991, ty * 3 + 7);
            int cx = 2 + (int)(h % 12), cy = 2 + (int)((h >> 4) % 12), roll = (int)((h >> 8) & 255);
            float f = fn[cy * 16 + cx];
            if (prot[cy * 16 + cx] || f >= 0.46f) continue;
            bool bush, rock;
            if (open) { rock = roll < 34 && f > 0.14f; bush = !rock && roll > 235 && f < 0.20f && f > 0.05f; }
            else      { bush = roll < 75 && f > 0.12f && f < 0.40f; rock = !bush && roll > 215 && f > 0.12f && f < 0.42f; }
            if (!bush && !rock) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    if (rock && (abs(dx) > 1 || (dx == -1 && dy == -1))) continue;
                    if (bush && abs(dx) == 2 && dy != 0) continue;
                    int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15 || prot[y * 16 + x] || fn[y * 16 + x] >= 0.48f) continue;
                    bool shade = dy == 1 || dx == 2;
                    code[y * 16 + x] = rock ? (shade ? SC_ROCK_D : SC_ROCK) : (shade ? SC_BUSH_D : SC_BUSH);
                }
        }
    }
    int nz = 0, sand = 0;
    for (int i = 0; i < 256; i++) { if (code[i]) nz++; if (code[i] >= SC_WSAND && code[i] <= SC_SAND_L) sand++; }
    if (out) for (int i = 0; i < 128; i++) out[i] = (uint8_t)(code[i * 2] | (code[i * 2 + 1] << 4));
    if (sand_out) *sand_out = sand;
    return nz;
}

static void shore_alloc(void)
{
    if (!bm_sea) {
        bm_sea = calloc(BM_BYTES, 1); bm_rock = calloc(BM_BYTES, 1);
        bm_shore = calloc(BM_BYTES, 1); bm_sand = calloc(BM_BYTES, 1);
    }
    if (!shc_d) shc_d = malloc(SHC_W * SHC_W * SHORE_BYTES);
    if (!shc_tag) shc_tag = malloc(SHC_W * SHC_W * sizeof(int32_t) + 16);
    if (!bm_sea || !bm_rock || !bm_shore || !bm_sand || !shc_d || !shc_tag) { printf("shore: out of memory\n"); abort(); }
    for (int i = 0; i < SHC_W * SHC_W; i++) shc_tag[i] = -1;
    shore_tables();
}

// Generation (PC): sea depth -> palette rows 16..18, bitmaps
static void build_shore(void)
{
    const int N = MAP * MAP;
    shore_alloc();
    memset(bm_sea, 0, BM_BYTES); memset(bm_rock, 0, BM_BYTES); memset(bm_shore, 0, BM_BYTES); memset(bm_sand, 0, BM_BYTES);
    // --- water depth in tiles (3/4 chamfer, torus) ---
    uint8_t *dd = malloc(N);
    for (int i = 0; i < N; i++) dd[i] = ((g_kind[i] & 0x7F) == T_WATER) ? 250 : 0;
    for (int it = 0; it < 2; it++) {
        for (int y = 0; y < MAP; y++)
            for (int x = 0; x < MAP; x++) {
                int i = MI(x, y);
                if (!dd[i]) continue;
                int m = dd[i], v;
                v = dd[MI(x - 1, y)] + 3; if (v < m) m = v;
                v = dd[MI(x, y - 1)] + 3; if (v < m) m = v;
                v = dd[MI(x - 1, y - 1)] + 4; if (v < m) m = v;
                v = dd[MI(x + 1, y - 1)] + 4; if (v < m) m = v;
                dd[i] = (uint8_t)(m > 250 ? 250 : m);
            }
        for (int y = MAP - 1; y >= 0; y--)
            for (int x = MAP - 1; x >= 0; x--) {
                int i = MI(x, y);
                if (!dd[i]) continue;
                int m = dd[i], v;
                v = dd[MI(x + 1, y)] + 3; if (v < m) m = v;
                v = dd[MI(x, y + 1)] + 3; if (v < m) m = v;
                v = dd[MI(x + 1, y + 1)] + 4; if (v < m) m = v;
                v = dd[MI(x - 1, y + 1)] + 4; if (v < m) m = v;
                dd[i] = (uint8_t)(m > 250 ? 250 : m);
            }
    }
    g_wrow = calloc(N, 1);
    for (int y = 0; y < MAP; y++)
        for (int x = 0; x < MAP; x++) {
            int i = MI(x, y);
            if ((g_kind[i] & 0x7F) != T_WATER || !g_sea[i]) continue;      // draw depth only near the sea
            float dt = dd[i] / 3.0f;
            if (dt <= 3.5f) continue;
            dt += (vnoise(x * 0.3f, y * 0.3f, 9) - 0.5f) * 2.4f;
            g_wrow[i] = dt < 4.5f ? 0 : dt < 7.5f ? 16 : dt < 11.5f ? 17 : 18;
        }
    free(dd);
    // --- bitmaps ---
    int cand = 0, masked = 0;
    for (int i = 0; i < N; i++) { if (g_sea[i]) BM_SET(bm_sea, i); if (g_rock[i]) BM_SET(bm_rock, i); }
    for (int ty = 0; ty < MAP; ty++)
        for (int tx = 0; tx < MAP; tx++) {
            if (!shore_candidate(tx, ty)) continue;
            cand++;
            int sand = 0;
            if (!shore_mask(tx, ty, NULL, &sand)) continue;
            int i = MI(tx, ty);
            BM_SET(bm_shore, i);
            if (sand >= 48) BM_SET(bm_sand, i);
            masked++;
        }
    printf("shore: %d masked tiles of %d candidates (would be %d KB stored, cache %d KB)\n",
           masked, cand, masked * SHORE_BYTES / 1024, SHC_W * SHC_W * SHORE_BYTES / 1024);
}

// Frame: build masks for visible tiles (direct-mapped cache), row flags
static void shore_prepare_view(int tx0, int ty0, int tw, int th)
{
    shore_gen_frame = 0;
    for (int j = 0; j < th; j++) {
        int ty = (ty0 + j) & (MAP - 1);
        int any = 0;
        for (int k = 0; k < tw; k++) {
            int tx = (tx0 + k) & (MAP - 1), i = MI(tx, ty);
            if (!BM_GET(bm_shore, i)) continue;
            any = 1;
            int slot = ((ty & (SHC_W - 1)) * SHC_W) | (tx & (SHC_W - 1));
            if (shc_tag[slot] != i) {
                shore_mask(tx, ty, shc_d + (size_t)slot * SHORE_BYTES, NULL);
                shc_tag[slot] = i;
                shore_gen_frame++;
            }
        }
        shore_row[ty] = (uint8_t)any;
    }
}
static inline const uint8_t *shore_slot(int tx, int ty)
{
    return shc_d + (size_t)((((ty) & (SHC_W - 1)) * SHC_W) | ((tx) & (SHC_W - 1))) * SHORE_BYTES;
}
