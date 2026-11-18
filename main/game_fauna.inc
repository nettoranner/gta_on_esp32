// game_fauna.inc - wildlife: cows, sheep, horses, deer, chickens, dogs, cats, pigeons, gulls, ducks and GEESE.
// Included at the end of game.c (single translation unit). Animals spawn around the player by tile "biome"
// (type + district palette tone): fields and meadows, forest, farmyard, city, seashore, ponds and lakes.
//
// Easter egg: geese. Get right up to them (or shoot near / at them) and the whole flock gets angry, rushes the player
// faster than a running person and kills with one peck. WASTED becomes GOOSED.

enum { AN_NONE = 0, AN_COW, AN_SHEEP, AN_HORSE, AN_DEER, AN_CHICKEN, AN_DOG, AN_CAT, AN_PIGEON, AN_GULL, AN_DUCK, AN_GOOSE, AN_TYPES };
enum { AS_IDLE = 0, AS_WALK, AS_FLEE, AS_FLY, AS_ANGRY, AS_DEAD };
#define NANIM 44
typedef struct { float x, y, ang, t, z, fx, fy; uint8_t type, state, col; int8_t hp; float phase; } anim_t;
static anim_t anim[NANIM];
static float anim_spawn_t = 0, goose_msg_t = 0;

//                                COW  SHEEP HORSE DEER CHICK DOG  CAT  PIGEON GULL DUCK GOOSE
static const float AN_WALK[AN_TYPES] = { 0, 9,   12,   16,   18,  14,   30,  24,  16,    14,  10,  13 };
static const float AN_RUN[AN_TYPES]  = { 0, 38,  55,   95,   125, 50,   85,  95,  90,    70,  32,  150 };
static const float AN_SHY[AN_TYPES]  = { 0, 26,  48,   55,   110, 34,   0,   60,  42,    46,  40,  0 };   // fear distance from a player on foot
static const uint8_t AN_CAP[AN_TYPES] = { 0, 6,  8,    3,    4,   8,    3,   3,   10,    8,   8,   7 };

static inline int tile_tone_rt(int tx, int ty)
{
    int row = MV_PAL(map_v[MI(tx, ty)]);
    return row >= R_TONE0 ? row - R_TONE0 : row == 0 ? TN_NORMAL : -1;
}
static inline bool tone_country(int t) { return t == TN_MEADOW || t == TN_WHEAT || t == TN_CROP || t == TN_PLOUGH || t == TN_RURAL || t == TN_FOREST; }
static inline bool tone_city(int t) { return t == TN_NORMAL || t == TN_CLEAN || t == TN_RICH || t == TN_WORN || t == TN_DIRTY || t == TN_GRIME || t == TN_WARM || t == TN_WARM_CLEAN || t == TN_WARM_DIRTY; }

static bool near_kind(int tx, int ty, int r, int kind, int sea)    // sea: -1 any water, 0 = not sea, 1 = sea
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int x = (tx + dx) & (MAP - 1), y = (ty + dy) & (MAP - 1);
            if (tile_kind(x, y) != kind) continue;
            if (sea < 0) return true;
            if ((int)BM_GET(bm_sea, MI(x, y)) == sea) return true;
        }
    return false;
}

// Can the animal stand at this point (calm behavior = its own habitat)
static bool an_ok(int type, float x, float y, bool loose)
{
    int tx = (((int)x) & WMASK) >> 4, ty = (((int)y) & WMASK) >> 4;
    int k = tile_kind(tx, ty), tn = tile_tone_rt(tx, ty);
    if (k == T_BUILDING) return false;
    if (type == AN_DUCK) return k == T_WATER && !BM_GET(bm_sea, MI(tx, ty));
    if (k == T_WATER) return false;
    if (loose) return true;                                  // fleeing / angry: anywhere, as long as it's not a wall or water
    switch (type) {
    case AN_COW: case AN_SHEEP: case AN_HORSE: return k == T_PARK && tone_country(tn) && tn != TN_FOREST;
    case AN_DEER:    return k == T_PARK && tone_country(tn);
    case AN_CHICKEN: return (k == T_PLAZA || k == T_PARK) && tn == TN_RURAL;
    case AN_GOOSE:   return k == T_PARK || k == T_PLAZA || k == T_SIDEWALK;
    case AN_GULL:    return k != T_ROAD;
    default:         return k == T_SIDEWALK || k == T_PLAZA || k == T_PARK;
    }
}

static int count_anim(int type) { int n = 0; for (int i = 0; i < NANIM; i++) if (anim[i].type == type) n++; return n; }
static int free_anim(void) { for (int i = 0; i < NANIM; i++) if (!anim[i].type) return i; return -1; }

static void spawn_group(int type, float x, float y, int n)
{
    for (int k = 0; k < n && count_anim(type) < AN_CAP[type]; k++) {
        int i = free_anim();
        if (i < 0) return;
        float px = x, py = y;
        for (int a = 0; a < 8; a++) {                           // flock: nearby, but not all at one point
            float qx = x + (frnd() - 0.5f) * 30, qy = y + (frnd() - 0.5f) * 30;
            if (an_ok(type, qx, qy, false)) { px = qx; py = qy; break; }
        }
        anim_t *m = &anim[i];
        memset(m, 0, sizeof(*m));
        m->type = (uint8_t)type; m->x = wrapf(px); m->y = wrapf(py);
        m->ang = frnd() * 2 * PIf; m->t = frnd() * 3; m->hp = (type == AN_COW || type == AN_HORSE) ? 30 : (type == AN_DEER || type == AN_DOG || type == AN_SHEEP) ? 15 : 1;
        m->col = (uint8_t)(rnd() & 3); m->phase = frnd() * 10;
        m->state = AS_IDLE;
        if (type == AN_GULL && (rnd() & 1)) { m->state = AS_FLY; m->z = 26; m->fx = px; m->fy = py; }   // circles over the shore
    }
}

// New animals beyond the screen edge, by terrain type
static void fauna_spawn(void)
{
    for (int attempt = 0; attempt < 6; attempt++) {
        float a = frnd() * 2 * PIf, d = 210 + frnd() * 120;
        float x = wrapf(plx + cosf(a) * d), y = wrapf(ply + sinf(a) * d);
        if (on_screen(x, y, 30)) continue;
        int tx = ((int)x) >> 4, ty = ((int)y) >> 4;
        int k = tile_kind(tx, ty), tn = tile_tone_rt(tx, ty);
        uint32_t r = rnd() % 100;
        if (k == T_WATER) {
            bool land = near_kind(tx, ty, 6, T_PARK, -1) || near_kind(tx, ty, 6, T_PLAZA, -1) || near_kind(tx, ty, 6, T_SIDEWALK, -1);
            if (!BM_GET(bm_sea, MI(tx, ty))) { if (land) { spawn_group(AN_DUCK, x, y, 2 + rnd() % 2); return; } }
            else if (r < 45 && land) { spawn_group(AN_GULL, x, y, 1 + rnd() % 2); return; }
            continue;
        }
        if (k == T_BUILDING || k == T_ROAD) continue;
        bool sea = near_kind(tx, ty, 3, T_WATER, 1), pond = near_kind(tx, ty, 3, T_WATER, 0);
        if (sea && r < 60) { spawn_group(AN_GULL, x, y, 1 + rnd() % 3); return; }
        if (pond && k == T_PARK && r < 55) { spawn_group(AN_GOOSE, x, y, 3 + rnd() % 3); return; }
        if (tn == TN_RURAL || tn == TN_MEADOW || tn == TN_WHEAT || tn == TN_CROP || tn == TN_PLOUGH) {   // farmyard nearby: chickens
            float fx = 0, fy = 0; bool yard = false;
            for (int dy = -8; dy <= 8 && !yard; dy++)
                for (int dx = -8; dx <= 8; dx++) {
                    int qx = (tx + dx) & (MAP - 1), qy = (ty + dy) & (MAP - 1);
                    if (tile_kind(qx, qy) == T_PLAZA && tile_tone_rt(qx, qy) == TN_RURAL) { yard = true; fx = qx * 16 + 8; fy = qy * 16 + 8; break; }
                }
            if (yard && r < 70) { spawn_group(AN_CHICKEN, fx, fy, 3 + rnd() % 3); return; }
        }
        if (k == T_PARK && tn == TN_FOREST) { if (r < 35) { spawn_group(AN_DEER, x, y, 1 + rnd() % 2); return; } continue; }
        if (k == T_PARK && tn == TN_MEADOW) {
            if (r < 45) spawn_group(AN_COW, x, y, 2 + rnd() % 3);
            else if (r < 85) spawn_group(AN_SHEEP, x, y, 3 + rnd() % 4);
            else spawn_group(AN_HORSE, x, y, 1 + rnd() % 2);
            return;
        }
        if (k == T_PARK && (tn == TN_WHEAT || tn == TN_CROP || tn == TN_PLOUGH)) { if (r < 12) { spawn_group(AN_DEER, x, y, 1); return; } continue; }
        if (tone_city(tn)) {
            if ((k == T_PLAZA || k == T_SIDEWALK) && r < 55) { spawn_group(AN_PIGEON, x, y, 3 + rnd() % 4); return; }
            if (r < 75) { spawn_group(AN_DOG, x, y, 1); return; }
            if (r < 92) { spawn_group(AN_CAT, x, y, 1); return; }
        }
    }
}

static void anim_kill(anim_t *m, bool by_player)
{
    if (m->state == AS_DEAD) return;
    m->state = AS_DEAD; m->t = 14; m->z = 0;
    emit_burst(m->x, m->y, (m->type == AN_COW || m->type == AN_HORSE) ? 8 : 4, 50, 0.5f, (m->type >= AN_CHICKEN && m->type != AN_DOG && m->type != AN_CAT) ? PT_DEBRIS : PT_BLOOD);
    if (m->type != AN_PIGEON && m->type != AN_GULL && m->type != AN_DUCK && m->type != AN_CHICKEN) add_decal(m->x, m->y, 2);
    if (m->type == AN_GOOSE && by_player) {                   // the whole flock avenges a goose
        for (int i = 0; i < NANIM; i++) {
            anim_t *g = &anim[i];
            if (g->type == AN_GOOSE && g->state != AS_DEAD && dist2w(g->x, g->y, m->x, m->y) < 200 * 200) { g->state = AS_ANGRY; g->t = 25; }
        }
    }
}

// Anger the goose flock near a point
static void geese_anger(float x, float y, float r)
{
    bool any = false;
    for (int i = 0; i < NANIM; i++) {
        anim_t *g = &anim[i];
        if (g->type != AN_GOOSE || g->state == AS_DEAD) continue;
        if (dist2w(g->x, g->y, x, y) < r * r) {
            for (int j = 0; j < NANIM; j++) {            // and everyone near it
                anim_t *h = &anim[j];
                if (h->type == AN_GOOSE && h->state != AS_DEAD && dist2w(h->x, h->y, g->x, g->y) < 120 * 120) {
                    if (h->state != AS_ANGRY) any = true;
                    h->state = AS_ANGRY; h->t = 25;
                }
            }
        }
    }
    if (any && goose_msg_t <= 0) { game_message("HONK! THE GEESE ARE ANGRY - RUN!", 2.5f); goose_msg_t = 10; plus_shake(2); }
}

// Noise (gunshot): animals scatter, birds take off; if the player shot near geese, the geese go berserk
static void fauna_noise(float x, float y, float r, bool by_player)
{
    for (int i = 0; i < NANIM; i++) {
        anim_t *m = &anim[i];
        if (!m->type || m->state == AS_DEAD || m->state == AS_ANGRY) continue;
        if (dist2w(m->x, m->y, x, y) > r * r) continue;
        if (m->type == AN_GOOSE) continue;
        if (m->type == AN_PIGEON || m->type == AN_GULL) { m->state = AS_FLY; m->t = 0; m->fx = x; m->fy = y; }
        else if (m->type != AN_DOG || frnd() < 0.7f) { m->state = AS_FLEE; m->t = 3 + frnd() * 2; m->fx = x; m->fy = y; }
    }
    if (by_player) geese_anger(x, y, 90);
}

// Bullet: true = hit an animal
static bool fauna_bullet(float x, float y, bool by_player)
{
    for (int i = 0; i < NANIM; i++) {
        anim_t *m = &anim[i];
        if (!m->type || m->state == AS_DEAD || m->z > 6) continue;
        float rr = (m->type == AN_COW || m->type == AN_HORSE) ? 7 : (m->type == AN_PIGEON || m->type == AN_CHICKEN || m->type == AN_DUCK) ? 3.5f : 5;
        if (dist2w(x, y, m->x, m->y) > rr * rr) continue;
        m->hp -= 10;
        emit_burst(m->x, m->y, 3, 40, 0.3f, PT_BLOOD);
        if (m->type == AN_GOOSE && by_player) geese_anger(m->x, m->y, 30);
        if (m->hp <= 0) anim_kill(m, by_player);
        else if (m->state != AS_ANGRY) { m->state = AS_FLEE; m->t = 4; m->fx = x; m->fy = y; }
        return true;
    }
    return false;
}

static void fauna_explode(float x, float y, bool by_player)
{
    for (int i = 0; i < NANIM; i++) {
        anim_t *m = &anim[i];
        if (m->type && m->state != AS_DEAD && dist2w(x, y, m->x, m->y) < 40 * 40) anim_kill(m, by_player);
    }
    fauna_noise(x, y, 220, by_player);
}

static void fauna_reset(void) { memset(anim, 0, sizeof(anim)); anim_spawn_t = 0; goosed = false; }

static void an_move(anim_t *m, float vx, float vy, float dt, bool loose)
{
    float nx = wrapf(m->x + vx * dt), ny = wrapf(m->y + vy * dt);
    if (an_ok(m->type, nx, m->y, loose)) m->x = nx; else m->ang += PIf * 0.6f;
    if (an_ok(m->type, m->x, ny, loose)) m->y = ny; else m->ang += PIf * 0.6f;
}

static void fauna_update(float dt)
{
    if (goose_msg_t > 0) goose_msg_t -= dt;
    anim_spawn_t -= dt;
    if (anim_spawn_t <= 0 && gstate == G_PLAY) { anim_spawn_t = 0.35f; fauna_spawn(); }
    float pcx = pcar >= 0 ? cars[pcar].x : plx, pcy = pcar >= 0 ? cars[pcar].y : ply;
    for (int i = 0; i < NANIM; i++) {
        anim_t *m = &anim[i];
        if (!m->type) continue;
        float d2 = dist2w(m->x, m->y, pcx, pcy);
        if (d2 > 560.0f * 560.0f && !on_screen(m->x, m->y, 40)) { m->type = AN_NONE; continue; }   // far away: remove
        m->phase += dt;
        if (m->state == AS_DEAD) { m->t -= dt; if (m->t <= 0) m->type = AN_NONE; continue; }
        // cars run animals over (except flying and swimming ones)
        if (m->z < 4 && m->type != AN_DUCK) {
            for (int c = 0; c < NCARS; c++) {
                car_t *cc = &cars[c];
                if (cc->mode == CAR_WRECK || cc->mode == CAR_OFF || fabsf(cc->speed) < 25) continue;
                float rr = (m->type == AN_COW || m->type == AN_HORSE) ? 12 : 9;
                if (dist2w(m->x, m->y, cc->x, cc->y) < rr * rr) {
                    bool byp = c == pcar;
                    anim_kill(m, byp);
                    if (m->type == AN_COW || m->type == AN_HORSE) { cc->speed *= 0.4f; damage_car(c, 6, false); if (byp) plus_shake(3); }
                    if (m->type == AN_GOOSE && byp) geese_anger(m->x, m->y, 150);
                    break;
                }
            }
            if (m->state == AS_DEAD) continue;
        }
        float pd = sqrtf(d2);
        float dx = wrapd(pcx - m->x), dy = wrapd(pcy - m->y);
        int ty = m->type;
        // ---- geese ----
        if (ty == AN_GOOSE) {
            if (m->state != AS_ANGRY && ON_FOOT && gstate == G_PLAY && pd < 22) geese_anger(m->x, m->y, 4);
            if (m->state == AS_ANGRY) {
                m->t -= dt;
                if (m->t <= 0 || pd > 460 || gstate != G_PLAY) { m->state = AS_WALK; m->t = 2; }
                else {
                    m->ang = atan2f(dy, dx);
                    float stop = pcar >= 0 ? 15 : 0;                  // near a car: hiss and peck the bumper
                    if (pd > stop + 2) an_move(m, cosf(m->ang) * AN_RUN[ty], sinf(m->ang) * AN_RUN[ty], dt, true);
                    if (ON_FOOT && pd < 7.5f && gstate == G_PLAY && invuln_t <= 0) {
                        armor = 0;
                        hurt_player(999);
                        if (gstate == G_WASTED) { goosed = true; game_message("HONK HONK HONK!", 3); emit_burst(plx, ply, 10, 60, 0.6f, PT_DEBRIS); }
                    }
                    continue;
                }
            }
        }
        // ---- birds in flight ----
        if (m->state == AS_FLY) {
            m->t += dt;
            if (ty == AN_GULL && m->z >= 20 && m->t > 2.5f) {               // gull circles
                m->ang += dt * 0.9f;
                an_move(m, cosf(m->ang) * 45, sinf(m->ang) * 45, dt, true);
                if (an_ok(ty, m->x, m->y, false) && frnd() < dt * 0.05f) { m->state = AS_IDLE; m->z = 0; m->t = 3; }
                continue;
            }
            if (m->z < 24) m->z += dt * 30;
            float ax = wrapd(m->x - m->fx), ay = wrapd(m->y - m->fy);
            float a = atan2f(ay, ax);
            m->ang = a;
            m->x = wrapf(m->x + cosf(a) * AN_RUN[ty] * dt);
            m->y = wrapf(m->y + sinf(a) * AN_RUN[ty] * dt);
            if (m->t > 4 && !on_screen(m->x, m->y, 20)) m->type = AN_NONE;          // flew away
            continue;
        }
        // ---- fear: player on foot close, or a fast car ----
        bool threat = (ON_FOOT && pd < AN_SHY[ty]) || (pcar >= 0 && fabsf(cars[pcar].speed) > 40 && pd < AN_SHY[ty] * 1.6f + 20);
        if (ty == AN_DOG) threat = threat || (pcar >= 0 && fabsf(cars[pcar].speed) > 40 && pd < 60);
        if (threat && m->state != AS_FLEE) {
            if (ty == AN_PIGEON || ty == AN_GULL) { m->state = AS_FLY; m->t = 0; m->fx = pcx; m->fy = pcy; continue; }
            m->state = AS_FLEE; m->t = 2.5f + frnd() * 2; m->fx = pcx; m->fy = pcy;
        }
        switch (m->state) {
        case AS_FLEE: {
            m->t -= dt;
            float a = atan2f(wrapd(m->y - m->fy), wrapd(m->x - m->fx)) + sinf(m->phase * 3) * 0.3f;
            m->ang = a;
            an_move(m, cosf(a) * AN_RUN[ty], sinf(a) * AN_RUN[ty], dt, ty != AN_DUCK);
            if (m->t <= 0) { m->state = AS_WALK; m->t = 2; }
            break;
        }
        case AS_IDLE:
            m->t -= dt;
            // dog: approaches the player and barks
            if (ty == AN_DOG && ON_FOOT && pd < 90 && pd > 22) { m->ang = atan2f(dy, dx); an_move(m, cosf(m->ang) * 40, sinf(m->ang) * 40, dt, false); }
            if (m->t <= 0) { m->state = AS_WALK; m->t = 1.5f + frnd() * 3; m->ang += (frnd() - 0.5f) * 2.5f; }
            break;
        default:   // AS_WALK
            m->t -= dt;
            an_move(m, cosf(m->ang) * AN_WALK[ty], sinf(m->ang) * AN_WALK[ty], dt, false);
            if (!an_ok(ty, m->x, m->y, false)) {             // wandered off to the wrong place (after fleeing): slowly head back
                an_move(m, cosf(m->ang) * AN_WALK[ty], sinf(m->ang) * AN_WALK[ty], dt, true);
            }
            if (m->t <= 0) { m->state = AS_IDLE; m->t = 1 + frnd() * ((ty == AN_COW || ty == AN_SHEEP) ? 6 : 3); }
            break;
        }
    }
}

// ---------------------------------------------------------------- drawing
static HOT void blob(float cx, float cy, float ang, float a, float b, uint16_t c, bool shade)
{
    float R = a > b ? a : b;
    int ya = (int)floorf(cy - R), yb = (int)ceilf(cy + R);
    if (yb < g_y0 || ya >= g_y1) return;
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1 - 1) yb = g_y1 - 1;
    float ca = cosf(ang), sa = sinf(ang), ia = 1.0f / (a * a), ib = 1.0f / (b * b);
    for (int y = ya; y <= yb; y++) {
        uint16_t *d = g_buf + (y - g_y0) * SW;
        float dy = y + 0.5f - cy;
        for (int x = (int)floorf(cx - R); x <= (int)ceilf(cx + R); x++) {
            if ((unsigned)x >= SW) continue;
            float dx = x + 0.5f - cx;
            float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            if (u * u * ia + v * v * ib > 1.0f) continue;
            d[x] = shade ? darken(d[x]) : c;
        }
    }
}

static void draw_animal(const anim_t *m)
{
    float x = scr_x(m->x), y = scr_y(m->y) - m->z;
    if (y + 20 < g_y0 || y - 20 >= g_y1 || x < -20 || x > SW + 20) return;
    float a = m->ang, ca = cosf(a), sa = sinf(a);
    bool dead = m->state == AS_DEAD;
    bool mv = m->state == AS_WALK || m->state == AS_FLEE || m->state == AS_ANGRY;
    float bob = mv ? sinf(m->phase * 14) * 0.5f : 0;
    static const float AN_SCALE[AN_TYPES] = { 1, 1.3f, 1.15f, 1.3f, 1.2f, 1.1f, 1.1f, 1, 1.1f, 1.1f, 1.1f, 1.3f };
    const float k = AN_SCALE[m->type];                  // scale: bigger = more visible at 320x240
    #define FX(u, v) (x + (ca * (u) - sa * (v)) * k)
    #define FY(u, v) (y + (sa * (u) + ca * (v)) * k)
    #define blob(cx_, cy_, an_, a_, b_, c_, s_) blob((cx_), (cy_), (an_), (a_) * k, (b_) * k, (c_), (s_))
    uint16_t c1, c2;
    switch (m->type) {
    case AN_COW:
        c1 = m->col & 1 ? tint565(PK(240, 238, 230)) : tint565(PK(126, 82, 52));
        c2 = m->col & 1 ? tint565(PK(30, 30, 32)) : tint565(PK(240, 238, 230));
        blob(x + 2, y + 2, a, 6.5f, 3.6f, 0, true);
        blob(x, y, a, 6.5f, 3.6f, c1, false);
        blob(FX(-2 + bob, 1), FY(-2 + bob, 1), a, 2, 1.6f, c2, false);
        blob(FX(2, -1.5f), FY(2, -1.5f), a, 1.5f, 1.2f, c2, false);
        if (!dead) { blob(FX(7, 0), FY(7, 0), a, 2.2f, 1.8f, tint565(PK(60, 44, 36)), false); put_px((int)FX(9, 0), (int)FY(9, 0), tint565(PK(230, 170, 160))); }
        break;
    case AN_HORSE:
        c1 = m->col & 1 ? tint565(PK(110, 70, 40)) : tint565(PK(60, 44, 34));
        blob(x + 2, y + 2, a, 6.5f, 2.6f, 0, true);
        blob(x, y, a, 6.5f, 2.6f, c1, false);
        blob(FX(-6.5f, 0), FY(-6.5f, 0), a, 2, 0.8f, tint565(PK(30, 22, 18)), false);   // tail
        if (!dead) { blob(FX(7.5f + bob, 0), FY(7.5f + bob, 0), a, 2.6f, 1.4f, c1, false); blob(FX(5, 0), FY(5, 0), a, 2.5f, 0.7f, tint565(PK(30, 22, 18)), false); }
        break;
    case AN_SHEEP:
        blob(x + 1.5f, y + 1.5f, a, 4, 3.4f, 0, true);
        blob(x, y, a, 4, 3.4f, tint565(PK(232, 228, 214)), false);
        put_px((int)FX(-1, -1), (int)FY(-1, -1), tint565(PK(250, 248, 240)));
        if (!dead) blob(FX(4.2f + bob, 0), FY(4.2f + bob, 0), a, 1.6f, 1.3f, tint565(PK(40, 36, 34)), false);
        break;
    case AN_DEER:
        blob(x + 2, y + 2, a, 5, 2.2f, 0, true);
        blob(x, y, a, 5, 2.2f, tint565(PK(150, 96, 54)), false);
        blob(FX(-4.5f, 0), FY(-4.5f, 0), a, 1.2f, 1, tint565(PK(240, 236, 226)), false);
        if (!dead) {
            blob(FX(6 + bob, 0), FY(6 + bob, 0), a, 1.8f, 1.2f, tint565(PK(130, 82, 46)), false);
            uint16_t an = tint565(PK(90, 66, 40));                                 // horns
            put_px((int)FX(6, -2), (int)FY(6, -2), an); put_px((int)FX(5, -3), (int)FY(5, -3), an);
            put_px((int)FX(6, 2), (int)FY(6, 2), an); put_px((int)FX(5, 3), (int)FY(5, 3), an);
        }
        break;
    case AN_CHICKEN:
        blob(x + 1, y + 1, a, 2.2f, 1.8f, 0, true);
        blob(x, y, a, 2.2f, 1.8f, m->col & 1 ? tint565(PK(246, 244, 236)) : tint565(PK(176, 110, 54)), false);
        if (!dead) put_px((int)FX(2.5f + bob, 0), (int)FY(2.5f + bob, 0), tint565(PK(220, 40, 30)));
        break;
    case AN_DOG:
        c1 = m->col == 0 ? tint565(PK(140, 96, 52)) : m->col == 1 ? tint565(PK(36, 32, 30)) : m->col == 2 ? tint565(PK(220, 200, 160)) : tint565(PK(110, 110, 112));
        blob(x + 1.5f, y + 1.5f, a, 4, 1.8f, 0, true);
        blob(x, y, a, 4, 1.8f, c1, false);
        if (!dead) { blob(FX(4.5f + bob, 0), FY(4.5f + bob, 0), a, 1.7f, 1.4f, c1, false); blob(FX(-4.5f, sinf(m->phase * 12)), FY(-4.5f, sinf(m->phase * 12)), a, 1.6f, 0.6f, c1, false); }
        if (!dead && m->state == AS_IDLE && ON_FOOT && dist2w(m->x, m->y, plx, ply) < 90 * 90 && ((int)(m->phase * 2)) % 3 == 0) draw_text_sh((int)x - 10, (int)y - 12, "WOOF", 1, C_WHITE);
        break;
    case AN_CAT:
        c1 = m->col & 1 ? tint565(PK(230, 140, 60)) : tint565(PK(96, 96, 100));
        blob(x + 1, y + 1, a, 2.8f, 1.4f, 0, true);
        blob(x, y, a, 2.8f, 1.4f, c1, false);
        if (!dead) { blob(FX(3, 0), FY(3, 0), a, 1.3f, 1.3f, c1, false); blob(FX(-4, 1), FY(-4, 1), a + 0.6f, 2, 0.5f, c1, false); }
        break;
    case AN_PIGEON: case AN_GULL: {
        bool gull = m->type == AN_GULL;
        c1 = gull ? tint565(PK(246, 246, 244)) : tint565(PK(126, 128, 142));
        c2 = gull ? tint565(PK(150, 156, 166)) : tint565(PK(88, 90, 104));
        if (m->state == AS_FLY && !dead) {
            float sh = m->z * 0.6f + 2;
            blob(x + sh, y + m->z + sh * 0.5f, a, 2, 1, 0, true);
            float fl = 0.6f + 0.4f * sinf(m->phase * 22);                             // wing flaps
            float span = (gull ? 6.5f : 4.5f) * fl;
            blob(x, y, a + PIf / 2, span, 1.3f, c2, false);
            blob(x, y, a, gull ? 3 : 2.2f, 1.2f, c1, false);
        } else {
            blob(x + 1, y + 1, a, gull ? 3 : 2, 1.4f, 0, true);
            blob(x, y, a, gull ? 3 : 2, gull ? 1.6f : 1.3f, c1, false);
            blob(FX(-0.5f, 0), FY(-0.5f, 0), a, gull ? 2 : 1.2f, 0.8f, c2, false);
            if (!dead) put_px((int)FX(gull ? 3.5f : 2.5f, 0), (int)FY(gull ? 3.5f : 2.5f, 0), gull ? tint565(PK(240, 200, 40)) : tint565(PK(80, 140, 110)));
        }
        break;
    }
    case AN_DUCK:
        blob(x, y, a, 3, 2, tint565(PK(150, 116, 76)), false);
        if (!dead) { blob(FX(2.6f, 0), FY(2.6f, 0), a, 1.3f, 1.2f, m->col & 1 ? tint565(PK(40, 120, 70)) : tint565(PK(130, 100, 70)), false); put_px((int)FX(4, 0), (int)FY(4, 0), tint565(PK(240, 180, 40))); }
        if (((int)(m->phase * 3)) & 1) put_px((int)FX(-4, 0), (int)FY(-4, 0), pal[0][P_WATER_L]);                   // ripples
        break;
    case AN_GOOSE: {
        bool angry = m->state == AS_ANGRY;
        uint16_t wh = tint565(PK(248, 248, 244)), gr = tint565(PK(200, 202, 206)), be = tint565(PK(250, 140, 30));
        blob(x + 2, y + 2, a, 4, angry ? 5.5f : 2.6f, 0, true);
        if (angry) blob(x, y, a + PIf / 2, 6 + sinf(m->phase * 20), 1.6f, gr, false);     // wings spread
        blob(x, y, a, 4, 2.6f, wh, false);
        if (!dead) {
            float nk = angry ? 6.5f : 4.5f;                                           // neck stretched forward
            blob(FX(nk * 0.6f + bob, 0), FY(nk * 0.6f + bob, 0), a, nk * 0.5f, 0.9f, wh, false);
            blob(FX(nk + 0.5f, 0), FY(nk + 0.5f, 0), a, 1.4f, 1.3f, wh, false);
            put_px((int)FX(nk + 2.2f, 0), (int)FY(nk + 2.2f, 0), be); put_px((int)FX(nk + 3, 0), (int)FY(nk + 3, 0), be);
            if (angry) {
                put_px((int)FX(nk + 0.8f, -1), (int)FY(nk + 0.8f, -1), C_RED);
                if (((int)(m->phase * 3 + m->x)) % 4 == 0) draw_text_sh((int)x - 10, (int)y - 13, "HONK", 1, C_YELLOW);
            }
        }
        break;
    }
    }
    #undef FX
    #undef FY
    #undef blob
}

static void fauna_draw(void)          // on the ground (under cars and people)
{
    for (int i = 0; i < NANIM; i++) if (anim[i].type && anim[i].state != AS_FLY) draw_animal(&anim[i]);
}
static void fauna_draw_air(void)      // flying birds: above rooftops
{
    for (int i = 0; i < NANIM; i++) if (anim[i].type && anim[i].state == AS_FLY) draw_animal(&anim[i]);
}
