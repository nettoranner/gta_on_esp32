// ============================================================================
//  game_plus.inc — extensions: phone-driven story, bank robbery, 6 wanted stars
//  (roadblocks, spikes, SWAT, helicopter), side jobs (taxi, police, firefighter),
//  gang reputation, radio, cinema mode, replay, screen shake/vibration motor and effects.
//  Included at the end of game.c (single translation unit — shared static context).
// ============================================================================

static void find_pad(void);
static void ph_park_at_pad(void);
static void draw_pad(void);
static void draw_chopper(float wx, float wy, float ang, float rot, float alt, bool police);
static void heli_player_update(float dt);

// ---------------------------------------------------------------- shake and vibration
static float shake_a = 0, shake_x = 0, shake_y = 0;
static float rumble_v = 0;
static void plus_shake(float a)
{
    if (a > shake_a) shake_a = a;
    if (a * 40 > rumble_v) rumble_v = a * 40;
    if (rumble_v > 255) rumble_v = 255;
}
uint8_t game_rumble(void) { return (uint8_t)rumble_v; }

// ---------------------------------------------------------------- cinema mode
static bool cinema = false;

// ---------------------------------------------------------------- phones and story
enum { JS_NONE = 0, JS_STORY, JS_GREEN, JS_PURPLE };
static int job_src = JS_NONE, job_type = -1, job_pay = 0;
static int story_step = 0;              // story missions completed
#define STORY_N 7
static const struct { uint8_t type; int16_t pay; const char *brief; } STORY[STORY_N] = {
    { M_DELIVER, 300,  "MR.K: A PACKAGE NEEDS A DRIVER" },
    { M_STEAL,   500,  "MR.K: BRING ME THE GOLD CAR" },
    { M_HIT,     600,  "MR.K: A MAN OWES ME. END IT" },
    { M_ESCAPE,  700,  "MR.K: 3 STARS, THEN VANISH" },
    { M_RACE,    800,  "MR.K: 6 CHECKPOINTS, 80 SEC" },
    { M_BANK,    1000, "MR.K: THE BANK. YOU KNOW IT" },
    { M_BOSS,    2000, "MR.K: KILL THE GANG BOSS" },
};
#define NPHONE 3
static float phone_x[NPHONE], phone_y[NPHONE];
#define PHONE_AT(k) LM_AT[LM_PHONE0 + (k)]      // center, green ghetto, purple ghetto (game_world.inc)
static float brief_t = 0;
static char brief[40] = "";
static float big_t = 0;                 // «MISSION PASSED»
static char big_msg[24] = "";
static int gang_rep[2] = { 0, 0 };      // -100..100
static int war_gang = 0, war_need = 0, war_kills = 0;
static float job_timer = 0;
static int race_n = 0;
static int boss_idx = -1;

// ---------------------------------------------------------------- bank
enum { H_NONE = 0, H_DRILL, H_ESCAPE };
static int heist = H_NONE, heist_done = 0;
static float heist_t = 0, heist_cd = 0;
#define HEIST_TIME 10.0f
#define HEIST_PAY  2500

// ---------------------------------------------------------------- 4-6 stars
#define NSPIKE 2
static struct { float x, y, ang, t; } spike[NSPIKE];
static int rb_car[2] = { -1, -1 };
static float rb_t = 0, rb_cd = 0;
static struct { float x, y, vx, vy, rot, cd, spx, spy, hp, down; bool on; } heli;
static float swat_cd = 0, heli_cd = 0;

// ---------------------------------------------------------------- dropped weapons
#define NDROP 12
static struct { float x, y, t; uint8_t kind; int16_t amount; } drops[NDROP];

// ---------------------------------------------------------------- player helicopter
static struct { float x, y, vx, vy, ang, rot, alt, hp, gone_t; bool parked; } ph;
static float pad_x, pad_y;

// ---------------------------------------------------------------- side jobs and radio
enum { SJ_NONE = 0, SJ_TAXI, SJ_VIGIL, SJ_FIRE };
static int side = SJ_NONE, side_score = 0, last_pcar = -2;
static int fare = -1;                   // pedestrian hailing a taxi
static bool fare_in = false;
static float fare_x, fare_y, fare_t = 0, fare_cd = 0;
static int vig[3] = { -1, -1, -1 };
static float vig_cd = 0;
static bool hose_on = false;
static float hose_t = 0;
static int radio = 0;
static float radio_t = 0;
static const char *RADIO[] = { "KGTA ROCK", "NEVA FM", "CHIKKO RADIO", "DEEP 104", "TALK 99", "RADIO OFF" };
#define NRADIO 6

// ---------------------------------------------------------------- replay
// Compact frame snapshot: cars and people in full, bullets and particles — packed
typedef struct { int16_t x, y; uint8_t type, t; } rp_part_t;
typedef struct { int16_t x, y; int8_t vx, vy; uint8_t owner; } rp_bul_t;
typedef struct {
    car_t cars[NCARS];
    actor_t act[NACT];
    fx_t fx[NFX];
    rp_bul_t bul[NBUL];
    rp_part_t parts[NPART];
    float plx, ply, pang, camX, camY, g_time, flash_t, hurt_t, hx, hy, hrot, hspx, hspy;
    float fire_x, fire_y, fire_hp, ph_x, ph_y, ph_ang, ph_rot, ph_alt, hdown;
    int16_t pcar, fire_lot;
    uint8_t gstate, stars, weapon, p_moving, fire_stage, heli_on, in_heli, ph_parked;
} rp_frame_t;
#define RP_FRAMES 300                   // 10 s when recording at 30 per second
static rp_frame_t *rp_buf = NULL;
static int rp_cap = 0, rp_head = 0, rp_count = 0;
static float rp_acc = 0;
static bool replay = false;
static float rp_pos = 0, rp_speed = 1;
static rp_frame_t *rp_live = NULL;      // live state while viewing
static part_t *rp_live_parts = NULL;
static bullet_t *rp_live_bul = NULL;
static bool rp_live_cinema = false;

static void rp_alloc(void)
{
    if (rp_buf) return;
    for (int n = RP_FRAMES; n >= 60 && !rp_buf; n /= 2) {
        rp_buf = malloc((size_t)n * sizeof(rp_frame_t));
        if (rp_buf) rp_cap = n;
    }
    rp_live = malloc(sizeof(rp_frame_t));
    rp_live_parts = malloc(sizeof(parts));
    rp_live_bul = malloc(sizeof(bul));
    if (!rp_live || !rp_live_parts || !rp_live_bul) { free(rp_buf); rp_buf = NULL; rp_cap = 0; }
}

static void rp_capture(rp_frame_t *f)
{
    memcpy(f->cars, cars, sizeof(cars));
    memcpy(f->act, act, sizeof(act));
    memcpy(f->fx, fx, sizeof(fx));
    for (int i = 0; i < NBUL; i++) {
        f->bul[i].x = (int16_t)bul[i].x; f->bul[i].y = (int16_t)bul[i].y;
        f->bul[i].vx = (int8_t)(bul[i].vx / 4); f->bul[i].vy = (int8_t)(bul[i].vy / 4);
        f->bul[i].owner = bul[i].life > 0 ? (uint8_t)(bul[i].owner + 1) : 0;
    }
    for (int i = 0; i < NPART; i++) {
        const part_t *p = &parts[i];
        f->parts[i].x = (int16_t)p->x; f->parts[i].y = (int16_t)p->y; f->parts[i].type = p->type;
        f->parts[i].t = p->life > 0 ? (uint8_t)(1 + 254 * (1 - p->life / p->max)) : 0;
    }
    f->plx = plx; f->ply = ply; f->pang = pang; f->camX = camX; f->camY = camY; f->g_time = g_time;
    f->flash_t = flash_t; f->hurt_t = hurt_t;
    f->hx = heli.x; f->hy = heli.y; f->hrot = heli.rot; f->hspx = heli.spx; f->hspy = heli.spy; f->heli_on = heli.on;
    f->fire_x = fire_x; f->fire_y = fire_y; f->fire_hp = fire_hp; f->fire_lot = (int16_t)fire_lot; f->fire_stage = (uint8_t)fire_stage;
    f->ph_x = ph.x; f->ph_y = ph.y; f->ph_ang = ph.ang; f->ph_rot = ph.rot; f->ph_alt = ph.alt; f->in_heli = in_heli; f->ph_parked = ph.parked; f->hdown = heli.down;
    f->pcar = (int16_t)pcar; f->gstate = (uint8_t)gstate; f->stars = (uint8_t)stars; f->weapon = (uint8_t)weapon; f->p_moving = p_moving;
}

static void rp_apply(const rp_frame_t *f)
{
    memcpy(cars, f->cars, sizeof(cars));
    memcpy(act, f->act, sizeof(act));
    memcpy(fx, f->fx, sizeof(fx));
    for (int i = 0; i < NBUL; i++) {
        bul[i].x = f->bul[i].x; bul[i].y = f->bul[i].y;
        bul[i].vx = f->bul[i].vx * 4.0f; bul[i].vy = f->bul[i].vy * 4.0f;
        bul[i].life = f->bul[i].owner ? 0.5f : 0;
        bul[i].owner = f->bul[i].owner ? f->bul[i].owner - 1 : 0;
    }
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        p->x = f->parts[i].x; p->y = f->parts[i].y; p->type = f->parts[i].type;
        p->max = 1; p->life = f->parts[i].t ? 1 - (f->parts[i].t - 1) / 254.0f : 0;
        if (f->parts[i].t && p->life < 0.01f) p->life = 0.01f;
    }
    plx = f->plx; ply = f->ply; pang = f->pang; camX = f->camX; camY = f->camY; g_time = f->g_time;
    flash_t = f->flash_t; hurt_t = f->hurt_t;
    heli.x = f->hx; heli.y = f->hy; heli.rot = f->hrot; heli.spx = f->hspx; heli.spy = f->hspy; heli.on = f->heli_on;
    fire_x = f->fire_x; fire_y = f->fire_y; fire_hp = f->fire_hp; fire_lot = f->fire_lot; fire_stage = f->fire_stage;
    ph.x = f->ph_x; ph.y = f->ph_y; ph.ang = f->ph_ang; ph.rot = f->ph_rot; ph.alt = f->ph_alt; in_heli = f->in_heli; ph.parked = f->ph_parked; heli.down = f->hdown;
    pcar = f->pcar; gstate = f->gstate; stars = f->stars; weapon = f->weapon; p_moving = f->p_moving;
}

static void plus_record(float dt)
{
    if (!rp_buf) rp_alloc();
    if (!rp_buf) return;
    rp_acc += dt;
    if (rp_acc < 1.0f / 30) return;
    rp_acc -= 1.0f / 30;
    if (rp_acc > 0.1f) rp_acc = 0;
    rp_capture(&rp_buf[rp_head]);
    rp_head = (rp_head + 1) % rp_cap;
    if (rp_count < rp_cap) rp_count++;
}

static void replay_start(void)
{
    if (!rp_buf || rp_count < 30 || replay) { game_message("NOTHING TO REPLAY", 1.5f); return; }
    rp_capture(rp_live);
    memcpy(rp_live_parts, parts, sizeof(parts));
    memcpy(rp_live_bul, bul, sizeof(bul));
    rp_live_cinema = cinema;
    replay = true;
    rp_pos = 0;
    rp_speed = 1;
}

static void replay_stop(void)
{
    if (!replay) return;
    rp_apply(rp_live);
    memcpy(parts, rp_live_parts, sizeof(parts));
    memcpy(bul, rp_live_bul, sizeof(bul));
    cinema = rp_live_cinema;
    replay = false;
    gstate = G_PAUSE;
}

// In replay mode game_update only advances the "film"
static void replay_tick(float dt)
{
    if (pressed & BTN_START) { replay_stop(); return; }
    if (pressed & BTN_FIRE) {               // 1x -> 0.5x -> 0.25x
        rp_speed = (rp_speed > 0.9f) ? 0.5f : (rp_speed > 0.4f) ? 0.25f : 1.0f;
    }
    if (pressed & BTN_ENTER) rp_pos = 0;    // restart
    rp_pos += dt * 30 * rp_speed;
    if (rp_pos >= rp_count - 1) rp_pos = 0; // loop
}

// ---------------------------------------------------------------- initialization
static void plus_init(void)
{
    for (int k = 0; k < NPHONE; k++) {
        int tx, ty;
        if (!find_near(PHONE_AT(k)[0], PHONE_AT(k)[1], T_SIDEWALK, &tx, &ty)) { tx = PHONE_AT(k)[0]; ty = PHONE_AT(k)[1]; }
        phone_x[k] = tx * 16 + 8; phone_y[k] = ty * 16 + 8;
    }
    job_src = JS_NONE; job_type = -1; heist = H_NONE; heli.on = false; heli_cd = 0;
    for (int k = 0; k < NDROP; k++) drops[k].t = 0;
    in_heli = false;
    find_pad();
    ph_park_at_pad();
    for (int i = 0; i < NSPIKE; i++) spike[i].t = 0;
    rb_car[0] = rb_car[1] = -1;
    side = SJ_NONE; fare = -1; fare_in = false;
    replay = false; rp_count = 0; rp_head = 0;
}

static void plus_load(int32_t flags)
{
    story_step = (flags >> 1) & 7;
    if (story_step > STORY_N) story_step = STORY_N;
    gang_rep[0] = (int)((flags >> 8) & 0xFF) - 128;
    gang_rep[1] = (int)((flags >> 16) & 0xFF) - 128;
    if (!(flags & (1 << 24))) gang_rep[0] = gang_rep[1] = 0;   // old save
}

static int32_t plus_save_flags(void)
{
    return (story_step << 1) | ((gang_rep[0] + 128) << 8) | ((gang_rep[1] + 128) << 16) | (1 << 24);
}

static void rep_add(int g, int d)       // g: 0 green, 1 purple
{
    gang_rep[g] += d;
    if (gang_rep[g] > 100) gang_rep[g] = 100;
    if (gang_rep[g] < -100) gang_rep[g] = -100;
}

// ---------------------------------------------------------------- missions (new types)
static void ghetto_center(int g, float *x, float *y)
{
    int tx, ty;
    find_near(LM_AT[g == 1 ? LM_GANG1 : LM_GANG2][0], LM_AT[g == 1 ? LM_GANG1 : LM_GANG2][1], T_SIDEWALK, &tx, &ty);
    *x = tx * 16 + 8; *y = ty * 16 + 8;
}

static void race_next_checkpoint(void)
{
    float bx = (race_n == 0) ? plx : mis_x, by = (race_n == 0) ? ply : mis_y;
    for (int k = 0; k < 300; k++) {
        float a = frnd() * 2 * PIf, d = 260 + frnd() * 120;
        int tx = (((int)(bx + cosf(a) * d)) & WMASK) >> 4, ty = (((int)(by + sinf(a) * d)) & WMASK) >> 4;
        if (tile_kind(tx, ty) != T_ROAD) continue;
        mis_x = tx * 16 + 8; mis_y = ty * 16 + 8;
        return;
    }
    pick_far_point(bx, by, 260, &mis_x, &mis_y);
}

static void spawn_boss(void)
{
    int g = (gang_rep[0] <= gang_rep[1]) ? 1 : 2;      // boss of the gang that likes you less
    float x, y;
    ghetto_center(g, &x, &y);
    boss_idx = spawn_gang(g, x, y, false);
    if (boss_idx >= 0) {
        act[boss_idx].hp = 400;
        act[boss_idx].flags = AF_BOSS;
        act[boss_idx].color = 7;
        for (int k = 0; k < 3; k++) {
            float gx, gy;
            if (point_near(x, y, 12, 40, &gx, &gy)) spawn_gang(g, gx, gy, false);
        }
    }
    war_gang = g;
}

static void plus_mission_start(int type)
{
    mtype = type;
    stage = 0;
    mtarget = -1;
    switch (type) {
    case M_ESCAPE:
        mis_x = plx; mis_y = ply;
        game_message("GET 3 STARS", 2.5f);
        break;
    case M_RACE:
        race_n = 0; job_timer = 80;
        race_next_checkpoint();
        game_message("RACE! CHECKPOINT 1/6", 2.5f);
        break;
    case M_BANK:
        mis_x = pois[POI_BANK].dx; mis_y = pois[POI_BANK].dy;
        heist_cd = 0;
        game_message("GO TO THE BANK", 2.5f);
        break;
    case M_BOSS:
        spawn_boss();
        if (boss_idx >= 0) { mis_x = act[boss_idx].x; mis_y = act[boss_idx].y; }
        game_message("FIND THE BOSS IN THE GHETTO", 2.5f);
        break;
    case M_GANGWAR: {
        ghetto_center(war_gang, &mis_x, &mis_y);
        war_kills = 0; war_need = 5; job_timer = 150;
        game_message(war_gang == 1 ? "KILL 5 GREENS" : "KILL 5 PURPLES", 2.5f);
        break;
    }
    }
}

static const char *plus_mission_text(void)
{
    static char t[40];
    switch (mtype) {
    case M_ESCAPE: return stage == 0 ? "GET 3 STARS" : "LOSE THE COPS";
    case M_RACE:   snprintf(t, sizeof(t), "CHECKPOINT %d/6  %ds", race_n + 1, (int)job_timer); return t;
    case M_BANK:   return heist == H_DRILL ? "CRACKING THE VAULT..." : heist == H_ESCAPE ? "LOSE THE COPS WITH THE CASH" : "ROB THE BANK";
    case M_BOSS:   return "KILL THE GANG BOSS";
    case M_GANGWAR: snprintf(t, sizeof(t), "GANG WAR %d/%d  %ds", war_kills, war_need, (int)job_timer); return t;
    }
    return "";
}

static void job_fail(const char *why)
{
    game_message(why, 2.5f);
    job_src = JS_NONE; job_type = -1;
    if (boss_idx >= 0 && act[boss_idx].kind == A_GANG && (act[boss_idx].flags & AF_BOSS)) act[boss_idx].flags = 0;
    boss_idx = -1;
    mission_start(M_DELIVER);
}

static void plus_mission_update(void)
{
    switch (mtype) {
    case M_ESCAPE:
        mis_x = plx; mis_y = ply;
        if (stage == 0 && stars >= 3) { stage = 1; game_message("NOW LOSE THEM!", 2); }
        else if (stage == 1 && stars == 0) mission_done(job_pay, "CLEAN ESCAPE");
        break;
    case M_RACE:
        if (dist2w(mis_x, mis_y, plx, ply) < 30 * 30) {
            race_n++;
            if (race_n >= 6) { mission_done(job_pay + (int)job_timer * 10, "RACE WON"); break; }
            job_timer += 6;
            race_next_checkpoint();
            char s2[32]; snprintf(s2, sizeof(s2), "CHECKPOINT %d/6", race_n + 1); game_message(s2, 1.2f);
        }
        break;
    case M_BANK:
        mis_x = pois[POI_BANK].dx; mis_y = pois[POI_BANK].dy;
        break;
    case M_BOSS: {
        actor_t *b = (boss_idx >= 0) ? &act[boss_idx] : NULL;
        if (!b || b->kind != A_GANG || !(b->flags & AF_BOSS)) { spawn_boss(); break; }
        mis_x = b->x; mis_y = b->y;
        if (b->state == S_DEAD) { b->flags = 0; boss_idx = -1; mission_done(job_pay, "BOSS DOWN"); }
        // guards get angry when the player is nearby
        if (dist2w(b->x, b->y, plx, ply) < 200 * 200) gang_hate[b->gang - 1] = fmaxf(gang_hate[b->gang - 1], 3);
        break;
    }
    case M_GANGWAR:
        if (war_kills >= war_need) {
            int own = (job_src == JS_GREEN) ? 0 : 1;
            rep_add(own, 20); rep_add(1 - own, -15);
            mission_done(job_pay, "GANG WAR WON");
        }
        break;
    }
}

// mission_done: return which mission to take next
static int plus_mission_done(int pay)
{
    (void)pay;
    int next = (mtype < M_COUNT) ? (mtype + 1) % M_COUNT : M_DELIVER;
    if (job_src != JS_NONE) {
        if (pay < job_pay) money += job_pay - pay;           // story rate instead of the usual one
        if (job_src == JS_STORY) {
            story_step++;
            if (story_step >= STORY_N) { strcpy(big_msg, "THE CITY IS YOURS!"); money += 5000; }
            else strcpy(big_msg, "MISSION PASSED!");
        } else strcpy(big_msg, "GANG JOB DONE!");
        big_t = 3;
        job_src = JS_NONE; job_type = -1;
        next = M_DELIVER;
        save_req = true;
    }
    return next;
}

static void phone_answer(int k)
{
    if (job_src != JS_NONE) { game_message("FINISH THE CURRENT JOB", 1.5f); return; }
    if (k == 0) {
        if (story_step >= STORY_N) { game_message("MR.K: NO MORE WORK. ENJOY", 2); return; }
        job_src = JS_STORY;
        job_type = STORY[story_step].type;
        job_pay = STORY[story_step].pay;
        strcpy(brief, STORY[story_step].brief);
    } else {
        int g = k;                                   // 1 green, 2 purple
        if (gang_rep[g - 1] < -30) { game_message(g == 1 ? "GREENS: GET LOST!" : "PURPLES: GET LOST!", 2); return; }
        job_src = (g == 1) ? JS_GREEN : JS_PURPLE;
        job_type = M_GANGWAR;
        job_pay = 400;
        war_gang = 3 - g;                            // fight the rivals
        strcpy(brief, g == 1 ? "GREENS: HIT THE PURPLES" : "PURPLES: HIT THE GREENS");
    }
    brief_t = 3.5f;
    if (job_type >= M_COUNT) plus_mission_start(job_type);
    else {
        mission_start(job_type);
        reward = job_pay;
    }
}

// ---------------------------------------------------------------- bank
static void heist_start(void)
{
    if (heist != H_NONE) return;
    if (heist_cd > 0) { game_message("THE VAULT IS EMPTY. COME LATER", 2); return; }
    heist = H_DRILL;
    heist_t = 0;
    game_message("CRACKING THE VAULT! STAY HERE", 2.5f);
}

static void heist_update(float dt)
{
    if (heist_cd > 0) heist_cd -= dt;
    if (heist == H_DRILL) {
        heist_t += dt;
        float d2 = dist2w(plx, ply, pois[POI_BANK].dx, pois[POI_BANK].dy);
        if (pcar >= 0 || d2 > 60 * 60) { heist = H_NONE; heist_cd = 30; game_message("HEIST ABORTED", 2); return; }
        if (heist_t > 2 && stars < 3) { heat = fmaxf(heat, STAR_T[3]); add_heat(0.01f); if (heist_t < 2.1f) game_message("ALARM!", 1.5f); }
        if (((int)(heist_t * 4)) & 1) scare_civs(plx, ply, 150);
        if (heist_t >= HEIST_TIME) {
            heist = H_ESCAPE;
            heat = fmaxf(heat, STAR_T[4] + 0.5f); add_heat(0.01f);
            emit_burst(plx, ply, 10, 60, 0.6f, PT_SPARK);
            game_message("GOT THE CASH! LOSE THE COPS", 2.5f);
        }
    } else if (heist == H_ESCAPE) {
        if (stars == 0) {
            heist = H_NONE; heist_cd = 180; heist_done++;
            money += HEIST_PAY;
            strcpy(big_msg, "HEIST COMPLETE!"); big_t = 3;
            char s2[32]; snprintf(s2, sizeof(s2), "+$%d", HEIST_PAY); game_message(s2, 2.5f);
            save_req = true;
            if (mtype == M_BANK && job_src == JS_STORY) mission_done(job_pay, "BANK JOB");
        }
    }
}

// ---------------------------------------------------------------- 4-6 stars
static void place_roadblock(void)
{
    float hx = cosf(pang), hy = sinf(pang);
    if (pcar >= 0 && fabsf(cars[pcar].speed) > 20) { hx = cosf(cars[pcar].ang); hy = sinf(cars[pcar].ang); }
    for (int k = 0; k < 40; k++) {
        float d = 260 + frnd() * 80, a = atan2f(hy, hx) + (frnd() - 0.5f) * 0.8f;
        int tx = (((int)(plx + cosf(a) * d)) & WMASK) >> 4, ty = (((int)(ply + sinf(a) * d)) & WMASK) >> 4;
        int dir = lane_dir(lanes[MI(tx, ty)]);
        if (dir < 0) continue;
        float cx = tx * 16 + 8, cy = ty * 16 + 8;
        if (on_screen(cx, cy, 10)) continue;
        // two free police cars across the road
        int n = 0;
        for (int i = NCIVCARS; i < POLICE_END && n < 2; i++) {
            car_t *c = &cars[i];
            if (i == pcar || c->mode == CAR_COP || on_screen(c->x, c->y, 40)) continue;
            car_reset(c);
            float off = (n == 0) ? -9 : 9;
            float px = -DYv[dir], py = DXv[dir];                // across the lane
            c->x = wrapf(cx + px * off + DXv[dir] * n * 2); c->y = wrapf(cy + py * off + DYv[dir] * n * 2);
            c->ang = (dir + 1) * PIf / 2 + (n ? 0.25f : -0.2f);
            c->mode = CAR_PARKED; c->speed = 0; c->col = 8; c->police = 1; c->hp = 140; c->flat = 0;
            rb_car[n++] = i;
        }
        if (!n) return;
        // spikes on the player's side
        float sgn = (DXv[dir] * wrapd(plx - cx) + DYv[dir] * wrapd(ply - cy)) > 0 ? 1 : -1;   // player's side
        float sx = wrapf(cx + DXv[dir] * 40 * sgn), sy = wrapf(cy + DYv[dir] * 40 * sgn);
        if (tile_kind(((int)sx) >> 4, ((int)sy) >> 4) == T_ROAD) {
            spike[0].x = sx; spike[0].y = sy; spike[0].ang = (dir + 1) * PIf / 2; spike[0].t = 40;
            float sx2 = wrapf(sx - DYv[dir] * 16), sy2 = wrapf(sy + DXv[dir] * 16);      // second strip — on the adjacent lane
            if (tile_kind(((int)sx2) >> 4, ((int)sy2) >> 4) == T_ROAD) { spike[1] = spike[0]; spike[1].x = sx2; spike[1].y = sy2; }
        }
        // officers nearby
        for (int j = 0; j < 2; j++) {
            float x, y;
            if (point_near(cx, cy, 16, 40, &x, &y)) spawn_cop(x, y, 1.5f);
        }
        rb_t = 45;
        return;
    }
}

static void update_heat_extras(float dt)
{
    // roadblocks and spikes from 4 stars
    rb_cd -= dt;
    if (rb_t > 0) {
        rb_t -= dt;
        bool gone = false;
        for (int n = 0; n < 2; n++) if (rb_car[n] >= 0 && cars[rb_car[n]].mode != CAR_PARKED) rb_car[n] = -1;
        if (rb_car[0] < 0 && rb_car[1] < 0) gone = true;
        if (rb_t <= 0 || stars < 4 || gone) {
            for (int n = 0; n < 2; n++) if (rb_car[n] >= 0 && stars >= 4) cop_car_start_chase(rb_car[n]);   // they leave and join the chase
            rb_car[0] = rb_car[1] = -1; rb_t = 0;
        }
    } else if (stars >= 4 && rb_cd <= 0) { place_roadblock(); rb_cd = 30; }
    for (int i = 0; i < NSPIKE; i++) {
        if (spike[i].t <= 0) continue;
        spike[i].t -= dt;
        if (stars == 0) spike[i].t = 0;
        if (pcar >= 0 && !cars[pcar].flat && fabsf(cars[pcar].speed) > 25) {
            float dx = wrapd(plx - spike[i].x), dy = wrapd(ply - spike[i].y);
            float ca = cosf(spike[i].ang), sa = sinf(spike[i].ang);
            float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            if (fabsf(u) < 16 && fabsf(v) < 6) {
                cars[pcar].flat = 1;
                emit_burst(plx, ply, 10, 90, 0.4f, PT_SPARK);
                plus_shake(4);
                game_message("SPIKES! TIRES BLOWN", 2);
            }
        }
    }
    if (pcar >= 0 && cars[pcar].flat && fabsf(cars[pcar].speed) > 30 && (rnd() & 3) == 0)
        emit(plx - cosf(pang) * 9, ply - sinf(pang) * 9, frnd() * 40 - 20, frnd() * 40 - 20, 0.2f, PT_SPARK);

    // SWAT from 5 stars
    swat_cd -= dt;
    if (stars >= 5 && swat_cd <= 0) {
        int n = 0;
        for (int i = 0; i < NACT; i++) if (act[i].kind == A_COP && (act[i].flags & AF_SWAT) && act[i].state != S_DEAD) n++;
        if (n < (stars >= 6 ? 6 : 4)) {
            float x, y;
            offscreen_point(T_SIDEWALK, 190, 280, &x, &y);
            int j = spawn_cop(x, y, 0.3f);
            if (j >= 0) { act[j].flags = AF_SWAT; act[j].hp = 150; }
            if (point_near(x, y, 6, 14, &x, &y)) { j = spawn_cop(x, y, 0.5f); if (j >= 0) { act[j].flags = AF_SWAT; act[j].hp = 150; } }
        }
        swat_cd = 5;
    }

    // helicopter from 5 stars, shoots at 6
    if (heli_cd > 0) heli_cd -= dt;
    if (stars >= 5 && !heli.on && heli_cd <= 0) {
        float a = frnd() * 2 * PIf;
        heli.x = wrapf(plx + cosf(a) * 320); heli.y = wrapf(ply + sinf(a) * 320);
        heli.vx = heli.vy = 0; heli.on = true; heli.cd = 3; heli.spx = plx; heli.spy = ply;
        heli.hp = 100; heli.down = 0;
        game_message("POLICE CHOPPER!", 2);
    }
    if (heli.on && heli.down > 0) {                 // shot down: spins and falls
        heli.down -= dt;
        heli.rot += dt * 10;
        float sp = sqrtf(heli.vx * heli.vx + heli.vy * heli.vy) * 0.97f + 30;
        float a = atan2f(heli.vy, heli.vx) + dt * 9;
        heli.vx = cosf(a) * sp * 0.5f; heli.vy = sinf(a) * sp * 0.5f;
        heli.x = wrapf(heli.x + heli.vx * dt); heli.y = wrapf(heli.y + heli.vy * dt);
        if ((rnd() & 1)) emit(heli.x, heli.y, frnd() * 20 - 10, frnd() * 20 - 10, 1.5f, PT_SMOKE);
        if (heli.down <= 0) {
            heli.on = false; heli_cd = 50;
            explode(heli.x, heli.y, false);
            emit_burst(heli.x, heli.y, 16, 160, 1.0f, PT_DEBRIS);
            add_decal(heli.x, heli.y, 1);
            plus_shake(8);
            money += 1000; save_req = true;
            if (stars > 4) { heat = STAR_T[4] + 0.5f; stars = 4; }
            strcpy(big_msg, "CHOPPER DOWN! +$1000"); big_t = 3;
        }
        return;
    }
    if (heli.on) {
        // player hits (from the ground, a car or the helicopter)
        for (int i = 0; i < NBUL; i++) {
            bullet_t *b = &bul[i];
            if (b->life <= 0 || b->owner != 0 || dist2w(b->x, b->y, heli.x, heli.y) > 13 * 13) continue;
            heli.hp -= b->dmg * 0.6f;
            emit_burst(b->x, b->y, 3, 70, 0.25f, PT_SPARK);
            b->life = 0;
        }
        if (heli.hp < 50 && (rnd() % 3) == 0) emit(heli.x, heli.y, frnd() * 10 - 5, frnd() * 10 - 5, 1.2f, PT_SMOKE);
        if (heli.hp <= 0) { heli.down = 1.6f; game_message("CHOPPER IS GOING DOWN!", 2); return; }
        float tx = plx, ty = ply;
        bool leave = stars < 5;
        if (leave) { tx = plx + 2000; ty = ply - 2000; }
        float dx = wrapd(tx - heli.x), dy = wrapd(ty - heli.y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
        float want = leave ? 200 : (d > 60 ? 165 : d * 2);
        heli.vx += (dx / d * want - heli.vx) * dt * 1.2f;
        heli.vy += (dy / d * want - heli.vy) * dt * 1.2f;
        if (heli.hp < 50) { heli.vx += (frnd() - 0.5f) * 300 * dt; heli.vy += (frnd() - 0.5f) * 300 * dt; }
        heli.x = wrapf(heli.x + heli.vx * dt); heli.y = wrapf(heli.y + heli.vy * dt);
        heli.rot += dt * 30;
        heli.spx = wrapf(heli.spx + wrapd(plx - heli.spx) * dt * 2.5f);
        heli.spy = wrapf(heli.spy + wrapd(ply - heli.spy) * dt * 2.5f);
        if (leave && !on_screen(heli.x, heli.y, 80)) heli.on = false;
        heli.cd -= dt;
        if (stars >= 6 && !leave && heli.cd <= 0 && d < 160) {
            for (int k = 0; k < 3; k++) {
                float ang = atan2f(wrapd(ply - heli.y), wrapd(plx - heli.x)) + (frnd() - 0.5f) * 0.3f;
                bullet_src = -2;                       // from height — above the rooftops
                fire_bullet(heli.x, heli.y, ang, 340, 6, 1);
                bullet_src = -1;
            }
            heli.cd = 1.3f;
        }
    }
}

// ---------------------------------------------------------------- side jobs
static void side_start(int kind)
{
    side = kind; side_score = 0;
    fare = -1; fare_in = false; fare_cd = 1;
    vig[0] = vig[1] = vig[2] = -1; vig_cd = 1;
    static const char *names[] = { "", "TAXI JOB: PICK UP FARES", "VIGILANTE: STOP THE CRIMINALS", "FIREFIGHTER: PUT OUT FIRES" };
    game_message(names[kind], 2.5f);
}

static void side_update(float dt)
{
    // start/end of a side job on vehicle change
    if (pcar != last_pcar) {
        if (side == SJ_TAXI && fare_in) game_message("THE FARE RAN OFF", 1.5f);
        if (side != SJ_NONE && side_score > 0) { char s2[32]; snprintf(s2, sizeof(s2), "JOB OVER: %d DONE", side_score); game_message(s2, 2); }
        side = SJ_NONE;
        if (fare >= 0 && act[fare].kind) act[fare].flags &= ~AF_FARE;
        fare = -1; fare_in = false;
        if (pcar >= 0) {
            car_t *c = &cars[pcar];
            if (pcar == FIRE_IDX) side_start(SJ_FIRE);
            else if (c->police) side_start(SJ_VIGIL);
            else if (c->col == 2) side_start(SJ_TAXI);
            else if (radio < NRADIO - 1) radio_t = 3;
        }
        last_pcar = pcar;
    }
    if (side == SJ_TAXI) {
        car_t *c = &cars[pcar];
        if (!fare_in) {
            if (fare >= 0 && (!act[fare].kind || act[fare].state == S_DEAD || !(act[fare].flags & AF_FARE) || dist2w(act[fare].x, act[fare].y, plx, ply) > 420 * 420)) {
                if (act[fare].kind) act[fare].flags &= ~AF_FARE;
                fare = -1;
            }
            if (fare < 0 && (fare_cd -= dt) <= 0) {
                fare_cd = 2;
                float best = 1e9f;
                for (int i = 0; i < NACT; i++) {
                    actor_t *a = &act[i];
                    if (a->kind != A_CIV || a->state != S_WALK || a->flags) continue;
                    float d2 = dist2w(a->x, a->y, plx, ply);
                    if (d2 > 90 * 90 && d2 < 260 * 260 && d2 < best) { best = d2; fare = i; }
                }
                if (fare >= 0) { act[fare].flags |= AF_FARE; act[fare].state = S_IDLE; act[fare].timer = 60; }
            }
            if (fare >= 0) {
                actor_t *a = &act[fare];
                a->state = S_IDLE; a->timer = 60;
                a->ang = atan2f(wrapd(ply - a->y), wrapd(plx - a->x));
                if (dist2w(a->x, a->y, plx, ply) < 26 * 26 && fabsf(c->speed) < 25) {
                    a->kind = A_NONE;
                    fare_in = true;
                    pick_far_point(plx, ply, 280, &fare_x, &fare_y);
                    float d = sqrtf(dist2w(fare_x, fare_y, plx, ply));
                    fare_t = d / 55 + 12;
                    game_message("TAKE ME TO THE GREEN MARK", 2);
                }
            }
        } else {
            fare_t -= dt;
            if (fare_t <= 0) { fare_in = false; fare = -1; game_message("TOO SLOW! THE FARE LEFT", 2); return; }
            if (dist2w(fare_x, fare_y, plx, ply) < 30 * 30 && fabsf(c->speed) < 40) {
                int pay = 25 + (int)fare_t * 3;
                money += pay; side_score++;
                char s2[32]; snprintf(s2, sizeof(s2), "FARE PAID +$%d", pay); game_message(s2, 2);
                fare_in = false; fare = -1; fare_cd = 2;
                int j = free_actor();
                if (j >= 0) { spawn_civ(&act[j], false); act[j].x = plx; act[j].y = ply; point_near(plx, ply, 10, 20, &act[j].x, &act[j].y); }
                save_req = true;
            }
        }
    } else if (side == SJ_VIGIL) {
        int alive = 0;
        for (int k = 0; k < 3; k++) {
            if (vig[k] < 0) continue;
            actor_t *a = &act[vig[k]];
            if (a->kind != A_GANG || dist2w(a->x, a->y, plx, ply) > 650 * 650) { vig[k] = -1; continue; }
            if (a->state == S_DEAD) {
                money += 150; side_score++;
                heat -= 0.5f; if (heat < 0) heat = 0; stars = stars_for(heat);
                game_message("CRIMINAL DOWN +$150", 1.8f);
                vig[k] = -1; save_req = true;
                continue;
            }
            alive++;
        }
        if (!alive && (vig_cd -= dt) <= 0) {
            vig_cd = 4;
            float x, y;
            offscreen_point(T_SIDEWALK, 260, 380, &x, &y);
            int g = 1 + (rnd() & 1);
            for (int k = 0; k < 2; k++) {
                float gx = x, gy = y;
                if (k) point_near(x, y, 6, 16, &gx, &gy);
                vig[k] = spawn_gang(g, gx, gy, false);
            }
            game_message("CRIMINALS SPOTTED - RED ARROW", 2);
        }
    } else if (side == SJ_FIRE) {
        // water cannon: fire ("shoot" button) in the fire truck
        hose_on = (cur_in.held & BTN_FIRE) != 0;
        if (hose_on) {
            car_t *c = &cars[pcar];
            float ca = cosf(c->ang), sa = sinf(c->ang);
            for (int k = 0; k < 2; k++) emit(c->x + ca * 14, c->y + sa * 14, ca * 160 + frnd() * 30 - 15, sa * 160 + frnd() * 30 - 15, 0.5f, PT_WATER);
            if (fire_stage == 1) {
                float dx = wrapd(fire_x - c->x), dy = wrapd(fire_y - c->y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
                if (d < 110 && (dx * ca + dy * sa) / d > 0.6f) {
                    fire_hp -= 22 * dt; hose_t = 1;
                    if (fire_hp <= 0) {
                        money += 300; side_score++; save_req = true;
                        strcpy(big_msg, "FIRE OUT! +$300"); big_t = 2.5f;
                    }
                }
            }
            // and disperses the crowd/extinguishes burning cars
            for (int i = 0; i < NCARS; i++) {
                car_t *o = &cars[i];
                if (i == pcar || o->mode == CAR_WRECK || o->hp > 0) continue;
                float dx = wrapd(o->x - c->x), dy = wrapd(o->y - c->y), d = sqrtf(dx * dx + dy * dy) + 0.01f;
                if (d < 90 && (dx * ca + dy * sa) / d > 0.7f) { o->hp = 20; o->burn_t = 0; }
            }
        }
        if (hose_t > 0) hose_t -= dt;
    }
    // radio
    if (radio_t > 0) radio_t -= dt;
}

static void plus_radio_next(void)
{
    radio = (radio + 1) % NRADIO;
    radio_t = 2.5f;
}

// ---------------------------------------------------------------- gangs
static void gang_update(float dt)
{
    static float t = 0;
    t -= dt;
    if (t > 0) return;
    t = 0.5f;
    for (int g = 0; g < 2; g++)
        if (gang_rep[g] <= -30) {          // enemies: shoot on sight
            for (int i = 0; i < NACT; i++)
                if (act[i].kind == A_GANG && act[i].gang == g + 1 && act[i].state != S_DEAD && dist2w(act[i].x, act[i].y, plx, ply) < 150 * 150) {
                    gang_hate[g] = fmaxf(gang_hate[g], 2);
                    break;
                }
        }
    // friends (reputation 30+) cover you from the police
    if (stars == 0) return;
    for (int i = 0; i < NACT; i++) {
        actor_t *a = &act[i];
        if (a->kind != A_GANG || a->state == S_DEAD || gang_rep[a->gang - 1] < 30 || gang_hate[a->gang - 1] > 0) continue;
        if (dist2w(a->x, a->y, plx, ply) > 160 * 160 || (rnd() % 3)) continue;
        int best = -1; float bd = 140 * 140;
        for (int j = 0; j < NACT; j++)
            if (act[j].kind == A_COP && act[j].state != S_DEAD) {
                float d2 = dist2w(a->x, a->y, act[j].x, act[j].y);
                if (d2 < bd && line_clear(a->x, a->y, act[j].x, act[j].y)) { bd = d2; best = j; }
            }
        if (best < 0) continue;
        a->ang = atan2f(wrapd(act[best].y - a->y), wrapd(act[best].x - a->x));
        bullet_src = i;
        fire_bullet(a->x + cosf(a->ang) * 6, a->y + sinf(a->ang) * 6, a->ang + (frnd() - 0.5f) * 0.2f, 300, 12, 2);
        bullet_src = -1;
    }
}

// Hook from kill_actor (before rewards)
static float knock_ang = -99, knock_sp = 0;
static void plus_on_kill(int i, bool by_player)
{
    actor_t *a = &act[i];
    if (knock_ang > -90) { a->ang = knock_ang; a->cd = knock_sp; knock_ang = -99; }
    else a->cd = 0;
    // weapons from cops and gangsters
    if (a->kind == A_COP || a->kind == A_GANG) {
        int kind = PK_PISTOL, amount = 12;
        if (a->kind == A_COP && (a->flags & AF_SWAT)) { kind = PK_UZI; amount = 60; }
        else if (a->kind == A_GANG) {
            int r = rnd() % 10;
            if (a->flags & AF_BOSS) { kind = PK_SHOTGUN; amount = 20; }
            else if (r < 3) { kind = PK_UZI; amount = 40; }
            else if (r < 4) { kind = PK_SHOTGUN; amount = 6; }
        }
        int slot = 0; float oldest = 1e9f;
        for (int k = 0; k < NDROP; k++) {
            if (drops[k].t <= 0) { slot = k; break; }
            if (drops[k].t < oldest) { oldest = drops[k].t; slot = k; }
        }
        drops[slot].x = wrapf(a->x + frnd() * 8 - 4); drops[slot].y = wrapf(a->y + frnd() * 8 - 4);
        drops[slot].kind = (uint8_t)kind; drops[slot].amount = (int16_t)amount; drops[slot].t = 25;
    }
    if (!by_player) return;
    if (a->kind == A_GANG) {
        int g = a->gang - 1;
        rep_add(g, -6); rep_add(1 - g, 2);
        if (mtype == M_GANGWAR && a->gang == war_gang) {
            war_kills++;
            char s2[24]; snprintf(s2, sizeof(s2), "%d/%d", war_kills, war_need); game_message(s2, 1);
        }
    }
}

// Corpse is thrown back by the shot and leaves a blood trail
static void plus_dead_tick(actor_t *a, float dt)
{
    if (a->cd <= 0) return;
    move_slide(&a->x, &a->y, cosf(a->ang) * a->cd, sinf(a->ang) * a->cd, dt);
    a->cd -= 260 * dt;
    if ((rnd() & 3) == 0) add_decal(a->x, a->y, 2);
}

// ---------------------------------------------------------------- player helicopter
#define PH_MAX 190.0f
static void ph_park_at_pad(void)
{
    ph.x = pad_x; ph.y = pad_y; ph.vx = ph.vy = 0; ph.ang = -PIf / 2; ph.alt = 0; ph.hp = 120; ph.parked = true; ph.gone_t = 0;
}

static void find_pad(void)
{
    // open 3x3 tile area with no buildings, water or roads — toward the south of the square
    for (int r = 0; r < 30; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int cx = LM_AT[LM_HELI][0] + dx, cy = LM_AT[LM_HELI][1] + dy, ok = 1;
                for (int y = -1; y <= 1 && ok; y++)
                    for (int x = -1; x <= 1 && ok; x++) {
                        int k = tile_kind((cx + x) & (MAP - 1), (cy + y) & (MAP - 1));
                        if (k == T_BUILDING || k == T_WATER || k == T_ROAD) ok = 0;
                    }
                if (ok) { pad_x = (cx & (MAP - 1)) * 16 + 8; pad_y = (cy & (MAP - 1)) * 16 + 8; return; }
            }
    int tx, ty;
    find_near(LM_AT[LM_HELI][0], LM_AT[LM_HELI][1], T_SIDEWALK, &tx, &ty);
    pad_x = tx * 16 + 8; pad_y = ty * 16 + 8;
}

static bool plus_heli_action(void)
{
    if (in_heli) {
        float sp = sqrtf(ph.vx * ph.vx + ph.vy * ph.vy);
        if (sp > 45) { game_message("SLOW DOWN TO LAND", 1.2f); return true; }
        if (!walkable_xy(ph.x, ph.y)) { game_message("CAN'T LAND HERE", 1.2f); return true; }
        in_heli = false; ph.parked = true; ph.alt = 0; ph.vx = ph.vy = 0;
        float x, y;
        if (point_near(ph.x, ph.y, 14, 24, &x, &y)) { plx = x; ply = y; }
        emit_burst(ph.x, ph.y, 12, 70, 0.6f, PT_SMOKE);
        game_message("LANDED", 1);
        return true;
    }
    if (pcar >= 0 || !ph.parked || ph.hp <= 0) return false;
    if (dist2w(ph.x, ph.y, plx, ply) > 26 * 26) return false;
    if (nearest_car() >= 0 && dist2w(ph.x, ph.y, plx, ply) > 18 * 18) return false;
    in_heli = true; ph.parked = false; ph.alt = 0.05f;
    plx = ph.x; ply = ph.y;
    if (ph.x == pad_x && ph.y == pad_y && stars < 2) { heat = fmaxf(heat, STAR_T[2]); add_heat(0.01f); game_message("CHOPPER STOLEN!", 2); }
    else game_message("TAKE OFF! B/K2 - LAND", 2);
    return true;
}

static void plus_heli_control(const game_input_t *in, float dt)
{
    if (ph.alt < 1) { ph.alt += dt * 0.7f; if (ph.alt > 1) ph.alt = 1; }
    float k = ph.alt;                                    // at takeoff the controls are still sluggish
    float tx = in->mx * PH_MAX, ty = in->my * PH_MAX;
    if (in->held & BTN_RUN) { tx *= 1.3f; ty *= 1.3f; }   // afterburner
    ph.vx += (tx - ph.vx) * dt * 1.6f * k;
    ph.vy += (ty - ph.vy) * dt * 1.6f * k;
    ph.x = wrapf(ph.x + ph.vx * dt); ph.y = wrapf(ph.y + ph.vy * dt);
    float sp = sqrtf(ph.vx * ph.vx + ph.vy * ph.vy);
    if (sp > 25) {                                       // nose follows the direction of travel
        float ta = atan2f(ph.vy, ph.vx);
        ph.ang += angle_diff(ta, ph.ang) * dt * 3;
    }
    ph.rot += dt * (12 + 20 * ph.alt);
    plx = ph.x; ply = ph.y; pang = ph.ang;
    p_moving = false;
    if (ph.alt < 0.6f && (rnd() & 1)) emit(ph.x + frnd() * 40 - 20, ph.y + frnd() * 40 - 20, frnd() * 60 - 30, frnd() * 60 - 30, 0.5f, PT_SMOKE);
    if (ph.hp < 40 && (rnd() % 3) == 0) emit(ph.x, ph.y, frnd() * 10 - 5, frnd() * 10 - 5, 1.2f, PT_SMOKE);
}

static void plus_heli_fire(void)
{
    // machine gun along the heading (or the right stick), above the rooftops, no ammo needed
    fire_cd = 0.075f;
    float a = pang + (frnd() - 0.5f) * 0.08f;
    bullet_src = -2;
    fire_bullet(ph.x + cosf(ph.ang) * 10, ph.y + sinf(ph.ang) * 10, a, 460, 22, 0);
    bullet_src = -1;
    flash_t = 0.05f;
    emit(ph.x, ph.y, -sinf(a) * 60, cosf(a) * 60, 0.5f, PT_CASING);
}

static void plus_heli_damage(int dmg)
{
    if (!in_heli || invuln_t > 0) return;
    ph.hp -= dmg;
    hurt_t = 0.2f;
    plus_shake(1.5f);
    if (ph.hp <= 0) {                                    // crashed
        in_heli = false; ph.parked = false; ph.gone_t = 40;
        explode(ph.x, ph.y, false);
        emit_burst(ph.x, ph.y, 16, 160, 1.0f, PT_DEBRIS);
        php = 0; gstate = G_WASTED; state_t = 0;
    }
}

static void plus_cam_lead(float *tlx, float *tly)
{
    if (!in_heli) return;
    *tlx = ph.vx * 0.45f; *tly = ph.vy * 0.45f;
}

static void heli_player_update(float dt)
{
    if (in_heli) return;
    // thrown far away or wrecked — back on the pad after a while
    if (!ph.parked) {
        ph.gone_t -= dt;
        if (ph.gone_t <= 0 && !on_screen(pad_x, pad_y, 60)) ph_park_at_pad();
    } else if ((ph.x != pad_x || ph.y != pad_y) && dist2w(ph.x, ph.y, plx, ply) > 900 * 900 && !on_screen(ph.x, ph.y, 60)) ph_park_at_pad();
    else if (ph.rot > 0) ph.rot += dt * 2;              // rotor spins down by inertia
}

// ---------------------------------------------------------------- Lagoon
// stashes (coordinates and rewards — in game_shore.inc): island on the lake, southern islands, spit, lighthouse cape
static float stash_t[NSTASH];           // > 0 — stash is empty (cooldown)
static int cur_area = -1;
static int area_at(float x, float y)        // 0 countryside, 1 old city, 2 port city, 3 airport, 4 port
{
    int tx = (((int)x) & WMASK) >> 4, ty = (((int)y) & WMASK) >> 4;
    if (tx >= AP_X0 - 4 && tx <= AP_X1 + 4 && ty >= AP_Y0 - 4 && ty <= AP_Y1 + 6) return 3;
    if (tx >= CA_X0 && tx <= CA_X1 && ty >= CA_Y0 && ty <= CA_Y1) return 1;
    if (tx >= CB_X0 - 30 && tx <= CB_X1 + 10 && ty >= CB_Y1 && ty <= 860) return 4;
    if (tx >= CB_X0 && tx <= CB_X1 && ty >= CB_Y0 && ty <= CB_Y1) return 2;
    return 0;
}
static void lagoon_update(float dt)
{
    static const char *const AREA_NAME[5] = { "COUNTRYSIDE", "OLD TOWN", "PORT CITY", "AIRPORT", "DOCKS" };
    int now = area_at(plx, ply);
    if (now != cur_area) {
        if (cur_area >= 0) { strcpy(big_msg, AREA_NAME[now]); big_t = 2; }
        cur_area = now;
    }
    for (int k = 0; k < NSTASH; k++) {
        if (stash_t[k] > 0) stash_t[k] -= dt;
        else if (ON_FOOT && dist2w(plx, ply, stash_x[k], stash_y[k]) < 14 * 14) {
            money += STASH_PAY[k]; php = 100;
            ammo[W_UZI] += 120; ammo[W_SHOTGUN] += 20;
            stash_t[k] = 600; save_req = true;
            snprintf(big_msg, sizeof(big_msg), "%s +$%d", STASH_NAME[k], (int)STASH_PAY[k]); big_t = 3;
        }
    }
}

static void draw_one_stash(int k)
{
    if (stash_t[k] > 0) return;
    float sx = scr_x(stash_x[k]), sy = scr_y(stash_y[k]);
    if (sy < g_y0 - 20 || sy >= g_y1 + 20 || sx < -20 || sx > SW + 20) return;
    int b = (int)(sinf(g_time * 4) * 1.5f);
    fill_rect((int)sx - 7, (int)sy - 5 + b, 15, 11, C_BLACK);
    fill_rect((int)sx - 6, (int)sy - 4 + b, 13, 9, rgb565(150, 90, 40));
    fill_rect((int)sx - 6, (int)sy - 1 + b, 13, 2, C_YELLOW);
    fill_rect((int)sx - 1, (int)sy - 2 + b, 3, 4, C_YELLOW);
    if (((int)(g_time * 3)) & 1) ring(sx, sy, 12, 1, C_YELLOW);
}
static void draw_stash(void) { for (int k = 0; k < NSTASH; k++) draw_one_stash(k); }

// lighthouse: rotating beam at night
static void draw_lighthouse_beam(int k)
{
    if (light_x < 0) return;
    float sx = scr_x(light_x), sy = scr_y(light_y);
    if (sy < g_y0 - 150 || sy >= g_y1 + 150 || sx < -150 || sx > SW + 150) return;
    float a = g_time * 0.9f;
    light_pool(sx, sy, 16, k, 255, 240, 170);
    for (int s = 1; s <= 13; s++)
        light_pool(sx + cosf(a) * s * 9, sy + sinf(a) * s * 9, 6 + s * 1.2f, k * (16 - s) / 26, 255, 236, 160);
}

// ---------------------------------------------------------------- main tick
static void plus_update(float dt)
{
    if (shake_a > 0) { shake_a -= dt * 18; if (shake_a < 0) shake_a = 0; }
    if (rumble_v > 0) { rumble_v -= dt * 500; if (rumble_v < 0) rumble_v = 0; }
    if (brief_t > 0) brief_t -= dt;
    if (big_t > 0) big_t -= dt;

    // story mission failed (e.g. the golden car was destroyed)
    if (job_src != JS_NONE && mtype != job_type) { job_src = JS_NONE; job_type = -1; game_message("JOB FAILED - CALL AGAIN", 2.5f); }
    if (job_src != JS_NONE && (mtype == M_RACE || mtype == M_GANGWAR)) {
        job_timer -= dt;
        if (job_timer <= 0) job_fail("OUT OF TIME!");
    }
    // phones
    if (ON_FOOT && (pressed & BTN_ENTER))
        for (int k = 0; k < NPHONE; k++)
            if (dist2w(phone_x[k], phone_y[k], plx, ply) < 16 * 16) { phone_answer(k); break; }

    // pick up dropped weapons
    for (int k = 0; k < NDROP; k++) {
        if (drops[k].t <= 0) continue;
        drops[k].t -= dt;
        if (ON_FOOT && dist2w(drops[k].x, drops[k].y, plx, ply) < 11 * 11) {
            int w = drops[k].kind == PK_UZI ? W_UZI : drops[k].kind == PK_SHOTGUN ? W_SHOTGUN : W_PISTOL;
            ammo[w] += drops[k].amount;
            if (weapon == W_FIST || weapon == W_PISTOL || w != W_PISTOL) weapon = w;
            char s2[24]; snprintf(s2, sizeof(s2), "%s +%d", WEAP[w].name, drops[k].amount); game_message(s2, 1);
            drops[k].t = 0;
        }
    }
    heli_player_update(dt);
    lagoon_update(dt);
    heist_update(dt);
    update_heat_extras(dt);
    side_update(dt);
    gang_update(dt);
}

// after death/arrest
static void plus_on_respawn(void)
{
    if (heist != H_NONE) { heist = H_NONE; heist_cd = 30; }
    if (job_src != JS_NONE) job_fail("JOB FAILED");
    for (int i = 0; i < NSPIKE; i++) spike[i].t = 0;
    heli.on = false;
    if (in_heli) { in_heli = false; ph.parked = false; ph.gone_t = 30; }
    rb_car[0] = rb_car[1] = -1; rb_t = 0;
    if (side == SJ_TAXI && fare >= 0 && act[fare].kind) act[fare].flags &= ~AF_FARE;
    side = SJ_NONE; fare = -1; fare_in = false; last_pcar = -2;
}

// ---------------------------------------------------------------- frame
static void plus_frame(void)
{
    if (replay && rp_count > 0) {
        int k = (int)rp_pos;
        int idx = (rp_head - rp_count + k + rp_cap) % rp_cap;
        rp_apply(&rp_buf[idx]);
        cam_ix = (int)floorf(camX);
        cam_iy = (int)floorf(camY);
    }
}

static void plus_apply_shake(void)
{
    if (shake_a > 0.3f && !replay) {
        shake_x = (frnd() - 0.5f) * shake_a * 2;
        shake_y = (frnd() - 0.5f) * shake_a * 2;
        cam_ix += (int)shake_x; cam_iy += (int)shake_y;
    }
}

// ---------------------------------------------------------------- rendering
static void draw_seg(float x0, float y0, float x1, float y1, uint16_t c)
{
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        put_px((int)(x0 + (x1 - x0) * t), (int)(y0 + (y1 - y0) * t), c);
    }
}

static void draw_arrow_over(float x, float y, uint16_t c)
{
    int bob = (int)(2 * sinf(g_time * 8));
    int ix = (int)x, iy = (int)y - 16 + bob;
    for (int k = 0; k < 5; k++) fill_rect(ix - 4 + k, iy + k, 9 - 2 * k, 1, c);
    fill_rect(ix - 1, iy - 4, 3, 4, c);
}

// glints on the water, ripples
static void draw_water_glints(void)
{
    int tx0 = (cam_ix - SW / 2) >> 4, ty0 = (cam_iy - SH / 2) >> 4;
    int ph = (int)(g_time * 3);
    for (int ty = ty0; ty <= ty0 + SH / 16 + 1; ty++) {
        float sy0 = scr_y(ty * 16.0f);
        if (sy0 + 16 < g_y0 || sy0 >= g_y1) continue;
        for (int tx = tx0; tx <= tx0 + SW / 16 + 1; tx++) {
            if (tile_kind(tx & (MAP - 1), ty & (MAP - 1)) != T_WATER || BM_GET(bm_shore, MI(tx, ty))) continue;
            uint32_t h = hash2(tx * 7 + ph, ty * 13);
            if ((h & 3) != 0) continue;
            float sx0 = scr_x(tx * 16.0f);
            int gx = (int)sx0 + (h >> 4) % 13, gy = (int)sy0 + (h >> 8) % 13;
            uint16_t c = (night > 0.5f) ? rgb565(120, 140, 190) : C_WHITE;
            put_px(gx, gy, c); put_px(gx + 1, gy, c); put_px(gx + 2, gy, c);
            if (h & 64) { put_px(gx + 1, gy + 1, c); }
        }
    }
}

static void plus_draw_ground(void)
{
    draw_water_glints();
    draw_pad();
    draw_stash();
    if (ph.parked) draw_chopper(ph.x, ph.y, ph.ang, ph.rot, 0, false);
    for (int k = 0; k < NDROP; k++) {
        if (drops[k].t <= 0 || (drops[k].t < 5 && (((int)(g_time * 8)) & 1))) continue;
        pickup_t tmp = { drops[k].x, drops[k].y, 0, drops[k].kind, 1 };
        draw_pickup(&tmp);
    }
    // spikes
    for (int i = 0; i < NSPIKE; i++) {
        if (spike[i].t <= 0) continue;
        float sx = scr_x(spike[i].x), sy = scr_y(spike[i].y);
        if (sy < g_y0 - 20 || sy >= g_y1 + 20 || sx < -20 || sx > SW + 20) continue;
        float ca = cosf(spike[i].ang), sa = sinf(spike[i].ang);
        for (int u = -15; u <= 15; u++) {
            float x = sx + ca * u, y = sy + sa * u;
            put_px((int)x, (int)y, C_DARK);
            if (!(u & 1)) { put_px((int)(x - sa * 2), (int)(y + ca * 2), C_GREY); put_px((int)(x + sa * 2), (int)(y - ca * 2), C_GREY); }
        }
    }
    // payphones
    for (int k = 0; k < NPHONE; k++) {
        float sx = scr_x(phone_x[k]), sy = scr_y(phone_y[k]);
        if (sy < g_y0 - 24 || sy >= g_y1 + 24 || sx < -20 || sx > SW + 20) continue;
        uint16_t c = (k == 0) ? rgb565(60, 140, 255) : (k == 1) ? C_GANG1 : C_GANG2;
        fill_rect((int)sx - 4, (int)sy - 6, 9, 12, C_BLACK);
        fill_rect((int)sx - 3, (int)sy - 5, 7, 10, c);
        fill_rect((int)sx - 2, (int)sy - 3, 5, 4, C_GLASS);
        bool avail = job_src == JS_NONE && (k != 0 || story_step < STORY_N);
        if (avail && (((int)(g_time * 4)) & 1)) {
            ring(sx, sy, 11, 1, c);
            draw_text_sh((int)sx - 1, (int)sy - 16, "!", 2, C_YELLOW);
        }
    }
    // taxi: the hailing person and the drop-off point
    if (side == SJ_TAXI) {
        uint16_t g = rgb565(60, 255, 90);
        if (!fare_in && fare >= 0 && act[fare].kind) {
            float sx = scr_x(act[fare].x), sy = scr_y(act[fare].y);
            ring(sx, sy, 9 + sinf(g_time * 8), 1.5f, g);
        }
        if (fare_in) {
            float sx = scr_x(fare_x), sy = scr_y(fare_y);
            ring(sx, sy, 12 + 2 * sinf(g_time * 6), 2.5f, g);
        }
    }
}

// light reflections on wet asphalt (night, rain)
static void plus_draw_lights(int k)
{
    draw_lighthouse_beam(k);
    if (!raining || k <= 10) return;
    for (int vi = 0; vi < nvl; vi++) { int i = vlamp[vi];
        float sx = scr_x(lamps[i].x), sy = scr_y(lamps[i].y);
        if (sy < g_y0 - 50 || sy >= g_y1 + 10 || sx < -10 || sx > SW + 10) continue;
        for (int s = 1; s <= 4; s++) light_pool(sx + (s & 1), sy + 6 + s * 6, 5 - s * 0.6f, k * (5 - s) / 5, 255, 210, 140);
    }
    for (int i = 0; i < NCARS; i++) {
        car_t *c = &cars[i];
        if (c->mode == CAR_WRECK || c->mode == CAR_PARKED || c->mode == CAR_OFF) continue;
        float sx = scr_x(c->x), sy = scr_y(c->y);
        if (sy < g_y0 - 40 || sy >= g_y1 + 40 || sx < -40 || sx > SW + 40) continue;
        float ca = cosf(c->ang), sa = sinf(c->ang);
        // red tail lights smear on the wet road
        light_pool(sx - ca * 15, sy - sa * 15 + 4, 6, k / 2, 255, 40, 30);
        if (c->mode == CAR_COP && c->police) light_pool(sx, sy + 10, 10, k / 3, (((int)(g_time * 6)) & 1) ? 255 : 40, 40, (((int)(g_time * 6)) & 1) ? 40 : 255);
    }
}

// smoke from industrial-zone chimneys and wisps over houses at night
static void draw_chimneys(void)
{
    for (int i = 0; i < nvis; i++) {
        const lot_t *l = &g_lots[vis[i].lot];
        bool ind = l->style == ST_INDUSTRY;
        if (!ind && !(l->style == ST_HOUSE && night > 0.5f && (vis[i].lot % 5) == 0)) continue;
        // chimney in the lot corner (accounting for building height)
        float hk = vis[i].hk;
        float mx = vis[i].a0 + 6 + (vis[i].lot * 7 % 3) * 6, my = vis[i].b0 + 6;
        float sx = SW / 2 + mx * (1 + hk), sy = SH / 2 + my * (1 + hk);
        if (sy < g_y0 - 60 || sy - 40 >= g_y1 || sx < -20 || sx > SW + 20) continue;
        if (ind) { fill_rect((int)sx - 2, (int)sy - 2, 5, 5, rgb565(90, 60, 50)); fill_rect((int)sx - 1, (int)sy - 1, 3, 3, C_DARK); }
        for (int p = 0; p < 6; p++) {
            float t = fmodf(g_time * 0.35f + p / 6.0f + vis[i].lot * 0.13f, 1.0f);
            float px = sx + t * 22 + sinf(t * 6 + p) * 3, py = sy - t * 30;
            float r = 2 + t * (ind ? 6 : 4);
            int ya = (int)(py - r), yb = (int)(py + r);
            for (int y = ya; y <= yb; y++) {
                if (y < g_y0 || y >= g_y1) continue;
                for (int x = (int)(px - r); x <= (int)(px + r); x++) {
                    if ((unsigned)x >= SW) continue;
                    float dx = x - px, dy = y - py;
                    if (dx * dx + dy * dy > r * r) continue;
                    if (((x + y) & 1) || (t > 0.6f && ((x ^ y) & 2))) continue;
                    g_buf[(y - g_y0) * SW + x] = (t < 0.25f && ind) ? C_DARK : C_SMOKE;
                }
            }
        }
    }
}

static void draw_chopper(float wx, float wy, float ang, float rot, float alt, bool police)
{
    float sx = scr_x(wx), sy = scr_y(wy);
    if (sy < g_y0 - 60 || sy >= g_y1 + 70 || sx < -60 || sx > SW + 60) return;
    float ca = cosf(ang), sa = sinf(ang);
    // shadow: the higher, the further down-right
    float ox = 3 + 23 * alt, oy = 4 + 30 * alt;
    shadow_circle(sx + ox, sy + oy, 8);
    shadow_circle(sx + ox - ca * 12, sy + oy - sa * 12, 3);
    uint16_t b0 = police ? rgb565(25, 35, 70) : rgb565(120, 20, 20), b1 = police ? rgb565(50, 70, 130) : rgb565(220, 50, 40);
    // tail boom and rotor
    for (int k = 6; k < 22; k++) fill_circle(sx - ca * k, sy - sa * k, k < 10 ? 2.5f : 1.5f, b0);
    if (!police) fill_circle(sx - ca * 15, sy - sa * 15, 1.5f, C_WHITE);
    float tx = sx - ca * 22, ty = sy - sa * 22;
    float tr = rot * 1.7f;
    draw_seg(tx - cosf(tr) * 4, ty - sinf(tr) * 4, tx + cosf(tr) * 4, ty + sinf(tr) * 4, C_GREY);
    // fuselage
    fill_circle(sx, sy, 8, b0);
    fill_circle(sx - 1, sy - 1, 6, b1);
    fill_circle(sx + ca * 4, sy + sa * 4, 3.5f, C_GLASS);
    fill_rect((int)(sx - sa * 7), (int)(sy + ca * 7), 2, 2, C_WHITE);
    fill_rect((int)(sx + sa * 7), (int)(sy - ca * 7), 2, 2, C_WHITE);
    // beacon light
    if (((int)(g_time * 3)) & 1) fill_rect((int)(sx - ca * 9) - 1, (int)(sy - sa * 9) - 1, 3, 3, police ? C_RED : C_WHITE);
    // main rotor: 4 blurred blades
    for (int b = 0; b < 4; b++) {
        float a = rot + b * PIf / 2;
        draw_seg(sx, sy, sx + cosf(a) * 22, sy + sinf(a) * 22, C_DARK);
        draw_seg(sx, sy, sx + cosf(a + 0.12f) * 21, sy + sinf(a + 0.12f) * 21, C_GREY);
    }
    fill_circle(sx, sy, 2, C_DARK);
}

static void draw_heli(void)
{
    if (!heli.on) return;
    float alt = heli.down > 0 ? heli.down / 1.6f : 1;
    float ang = atan2f(heli.vy, heli.vx + 0.001f);
    draw_chopper(heli.x, heli.y, ang, heli.rot, alt, true);
}

// helipad and parked helicopter
static void draw_pad(void)
{
    float sx = scr_x(pad_x), sy = scr_y(pad_y);
    if (sy < g_y0 - 30 || sy >= g_y1 + 30 || sx < -30 || sx > SW + 30) return;
    fill_circle(sx, sy, 22, rgb565(70, 74, 80));
    ring(sx, sy, 21, 2, C_YELLOW);
    uint16_t w = C_WHITE;
    fill_rect((int)sx - 7, (int)sy - 8, 3, 17, w); fill_rect((int)sx + 5, (int)sy - 8, 3, 17, w); fill_rect((int)sx - 4, (int)sy - 1, 9, 3, w);
}

static void plus_draw_air(void)
{
    draw_chimneys();
    // helicopter searchlight (night — bright circle, day — faint)
    if (heli.on) {
        int k = (int)(night * 150) + 40;
        if (k > 200) k = 200;
        light_pool(scr_x(heli.spx), scr_y(heli.spy), 34, k, 255, 255, 230);
    }
    draw_heli();
    if (in_heli) {
        draw_chopper(ph.x, ph.y, ph.ang, ph.rot, ph.alt, false);
        if (flash_t > 0) fill_circle(scr_x(ph.x) + cosf(pang) * 14, scr_y(ph.y) + sinf(pang) * 14, 3, C_YELLOW);
    }
    // arrows above targets
    if (side == SJ_VIGIL)
        for (int k = 0; k < 3; k++)
            if (vig[k] >= 0 && act[vig[k]].kind == A_GANG && act[vig[k]].state != S_DEAD)
                draw_arrow_over(scr_x(act[vig[k]].x), scr_y(act[vig[k]].y), C_RED);
    if (side == SJ_TAXI && !fare_in && fare >= 0 && act[fare].kind) {
        float sx = scr_x(act[fare].x), sy = scr_y(act[fare].y);
        draw_arrow_over(sx, sy, rgb565(60, 255, 90));
        if (((int)(g_time * 3)) & 1) draw_text_sh((int)sx - 7, (int)sy - 28, "TAXI!", 1, C_YELLOW);
    }
    if (mtype == M_BOSS && boss_idx >= 0 && act[boss_idx].kind == A_GANG && act[boss_idx].state != S_DEAD) {
        float sx = scr_x(act[boss_idx].x), sy = scr_y(act[boss_idx].y);
        draw_text_sh((int)sx - 7, (int)sy - 16, "BOSS", 1, C_RED);
    }
}

static void bar(int x, int y, int w, float f, uint16_t c)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    fill_rect(x, y, w, 6, C_BLACK);
    fill_rect(x + 1, y + 1, (int)((w - 2) * f), 4, c);
}

static void plus_draw_hud(void)
{
    // radio: station name and "equalizer"
    if (pcar >= 0 && radio < NRADIO - 1 && side == SJ_NONE) {
        if (radio_t > 0) {
            shade_rect(SW / 2 - 70, 58, 140, 16);
            draw_text_c(62, RADIO[radio], 2, rgb565(120, 220, 255));
        }
        for (int b = 0; b < 5; b++) {
            int h = 2 + (int)(5 * fabsf(sinf(g_time * (5 + b * 1.7f) + b)));
            fill_rect(80 + b * 3, 20 - h, 2, h, rgb565(120, 220, 255));
        }
    } else if (pcar >= 0 && radio_t > 0 && radio == NRADIO - 1) draw_text_c(62, "RADIO OFF", 2, C_GREY);

    // bank robbery
    if (heist == H_DRILL) {
        draw_text_c(SH - 74, "CRACKING THE VAULT", 1, C_YELLOW);
        bar(SW / 2 - 50, SH - 66, 100, heist_t / HEIST_TIME, C_YELLOW);
    } else if (heist == H_ESCAPE) draw_text_sh(6, 44, "$$ CASH BAG $$", 1, ((int)(g_time * 4)) & 1 ? C_GREEN : C_YELLOW);

    // side jobs
    char s[40];
    if (side == SJ_TAXI) {
        if (fare_in) snprintf(s, sizeof(s), "TAXI  FARE %ds  DONE %d", (int)fare_t, side_score);
        else snprintf(s, sizeof(s), "TAXI  FIND A FARE  DONE %d", side_score);
        draw_text_c(SH - 40, s, 1, rgb565(255, 220, 60));
    } else if (side == SJ_VIGIL) {
        snprintf(s, sizeof(s), "VIGILANTE  DONE %d", side_score);
        draw_text_c(SH - 40, s, 1, rgb565(120, 170, 255));
    } else if (side == SJ_FIRE) {
        snprintf(s, sizeof(s), fire_stage == 1 ? "FIRE ON THE MAP! A = WATER" : "FIREFIGHTER  DONE %d", side_score);
        draw_text_c(SH - 40, s, 1, C_ORANGE);
    }
    if (pcar >= 0 && cars[pcar].flat) draw_text_sh(6, 52, "FLAT TIRES", 1, C_RED);
    if (in_heli) {
        fill_rect(6, SH - 42, 62, 6, C_BLACK);
        fill_rect(7, SH - 41, (int)(fmaxf(ph.hp, 0) / 120 * 60), 4, ph.hp < 40 ? C_RED : rgb565(220, 50, 40));
        draw_text_sh(72, SH - 43, "B - LAND", 1, C_WHITE);
    } else if (ON_FOOT && ph.parked && dist2w(ph.x, ph.y, plx, ply) < 26 * 26 && gstate == G_PLAY) draw_text_c(SH - 52, "CHOPPER: B OR K2", 2, C_WHITE);
    if (heli.on && heli.down <= 0 && heli.hp < 100) bar(SW / 2 - 30, 22, 60, heli.hp / 100, rgb565(80, 120, 255));

    // taxi driver: arrow to the drop-off point
    if (side == SJ_TAXI && fare_in) {
        float dx = wrapd(fare_x - camX), dy = wrapd(fare_y - camY);
        if (fabsf(dx) > SW / 2 - 10 || fabsf(dy) > SH / 2 - 10) {
            float k = fminf((SW / 2 - 14) / (fabsf(dx) + 0.001f), (SH / 2 - 14) / (fabsf(dy) + 0.001f));
            fill_circle(SW / 2 + dx * k, SH / 2 + dy * k, 6, C_BLACK);
            fill_circle(SW / 2 + dx * k, SH / 2 + dy * k, 4, rgb565(60, 255, 90));
        }
    }
    // phone call
    if (brief_t > 0) {
        shade_rect(10, 64, SW - 20, 30);
        draw_text_sh(16, 68, "PHONE:", 1, rgb565(60, 140, 255));
        draw_text_c(80, brief, 1, C_WHITE);
    }
    if (big_t > 0) {
        shade_rect(0, 96, SW, 32);
        draw_text_c(102, big_msg, 3, (((int)(g_time * 6)) & 1) ? C_YELLOW : C_GREEN);
    }
    // injury — red frame
    if (hurt_t > 0) {
        uint16_t c = C_RED;
        fill_rect(0, 0, SW, 3, c); fill_rect(0, SH - 3, SW, 3, c); fill_rect(0, 0, 3, SH, c); fill_rect(SW - 3, 0, 3, SH, c);
    }
}

// Map icons: phones and bank
static void plus_map_markers(int x0, int y0, int w, int h, float cwx, float cwy, int num, int den)
{
    int mx, my;
    for (int k = 0; k < NPHONE; k++) {
        bool avail = job_src == JS_NONE && (k != 0 || story_step < STORY_N);
        if (!avail) continue;
        uint16_t c = (k == 0) ? rgb565(60, 140, 255) : (k == 1) ? C_GANG1 : C_GANG2;
        if (map_pt(x0, y0, w, h, cwx, cwy, num, den, phone_x[k], phone_y[k], &mx, &my, false)) draw_icon(mx, my, IC_PHONE, c);
    }
    if (side == SJ_TAXI && fare_in && map_pt(x0, y0, w, h, cwx, cwy, num, den, fare_x, fare_y, &mx, &my, true)) {
        fill_rect(mx - 2, my - 2, 5, 5, C_BLACK); fill_rect(mx - 1, my - 1, 3, 3, rgb565(60, 255, 90));
    }
    if (heli.on && map_pt(x0, y0, w, h, cwx, cwy, num, den, heli.x, heli.y, &mx, &my, false)) fill_rect(mx - 1, my - 1, 3, 3, C_BLUE);
    if (ph.parked && map_pt(x0, y0, w, h, cwx, cwy, num, den, ph.x, ph.y, &mx, &my, false)) draw_icon(mx, my, IC_HELI, rgb565(220, 50, 40));
    for (int k = 0; k < NSTASH; k++)
        if (stash_t[k] <= 0 && map_pt(x0, y0, w, h, cwx, cwy, num, den, stash_x[k], stash_y[k], &mx, &my, false)) { fill_rect(mx - 2, my - 2, 5, 5, C_BLACK); fill_rect(mx - 1, my - 1, 3, 3, C_YELLOW); }
}

// Pause screen: story and reputation statistics
static void plus_draw_pause(int rx, int ry)
{
    char s[24];
    snprintf(s, sizeof(s), "STORY %d/%d", story_step, STORY_N); draw_text_sh(rx, ry, s, 1, rgb565(60, 140, 255));
    for (int g = 0; g < 2; g++) {
        int y = ry + 10 + g * 9;
        draw_text_sh(rx, y, g ? "PURPLE" : "GREEN", 1, g ? C_GANG2 : C_GANG1);
        fill_rect(rx + 30, y, 62, 5, C_BLACK);
        fill_rect(rx + 61, y, 1, 5, C_GREY);
        int v = gang_rep[g] * 30 / 100;
        if (v > 0) fill_rect(rx + 62, y + 1, v, 3, C_GREEN);
        else if (v < 0) fill_rect(rx + 61 + v, y + 1, -v, 3, C_RED);
    }
}

// Cinema: black bars at top and bottom; replay — caption
static void plus_draw_overlay(void)
{
    if (replay) {
        fill_rect(0, 0, SW, 16, C_BLACK); fill_rect(0, SH - 16, SW, 16, C_BLACK);
        if (((int)(g_time * 2)) & 1 || 1) {
            fill_circle(12, 8, 4, (((int)(rp_pos / 15)) & 1) ? C_RED : C_DARK);
            draw_text(22, 5, "REPLAY", 1, C_WHITE);
        }
        char s[48];
        snprintf(s, sizeof(s), "%s  A SLOW  B RESTART  START EXIT", rp_speed > 0.9f ? "1X" : rp_speed > 0.4f ? "0.5X" : "0.25X");
        draw_text(70, SH - 11, s, 1, C_GREY);
        bar(SW - 80, 5, 70, rp_pos / (float)(rp_count - 1), C_WHITE);
        return;
    }
    if (cinema && gstate == G_PLAY) { fill_rect(0, 0, SW, 18, C_BLACK); fill_rect(0, SH - 18, SW, 18, C_BLACK); }
}

// Pause: X — replay, B — cinema mode
static void plus_pause_input(void)
{
    if (pressed & BTN_WEAPON) { replay_start(); return; }
    if (pressed & BTN_ENTER) { cinema = !cinema; gstate = G_PLAY; game_message(cinema ? "" : "CINEMA OFF", 1); }
}

static bool plus_is_roadblock(int i) { return rb_t > 0 && (rb_car[0] == i || rb_car[1] == i); }
static bool plus_heist_busy(void) { return heist != H_NONE; }
