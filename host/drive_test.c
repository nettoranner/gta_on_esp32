// Car handling test: throttle, steering, brake, reverse; straight road + wanted level from shooting
#include "../main/game.c"
static void place(void)
{   // put the car at the start of the longest horizontal straight road
    int bx = 0, by = 0, bl = 0;
    for (int ty = 0; ty < MAP; ty++) { int l = 0; for (int tx = 0; tx < MAP; tx++) { if (tile_kind(tx, ty) == T_ROAD) { l++; if (l > bl) { bl = l; bx = tx - l + 1; by = ty; } } else l = 0; } }
    cars[pcar].x = (bx + 300) * 16 + 8; cars[pcar].y = by * 16 + 8; cars[pcar].ang = 0; cars[pcar].speed = 0;
    printf("-- road y=%d x=%d len=%d\n", by, bx, bl);
}
static void run(const char *name, float mx, float my, int held, float secs, int *f)
{
    place();
    for (int i = 0; i < NCARS; i++) if (i != pcar) { cars[i].mode = CAR_OFF; cars[i].x = 10; cars[i].y = 10; }
    const float dt = 1.0f / 30;
    float a0 = cars[pcar].ang, x0 = cars[pcar].x, y0 = cars[pcar].y;
    float vmax = 0;
    for (int k = 0; k < (int)(secs * 30); k++, (*f)++) {
        game_input_t in = {0}; in.mx = mx; in.my = my; in.held = held;
        game_update(&in, dt);
        if (pcar < 0) { printf("  %s: left the car!\n", name); return; }
        if (fabsf(cars[pcar].speed) > vmax) vmax = fabsf(cars[pcar].speed);
    }
    float da = angle_diff(cars[pcar].ang, a0);
    printf("%-22s speed=%6.1f vmax=%6.1f dAng=%6.1f deg  moved=(%5.0f,%5.0f)\n", name, cars[pcar].speed, vmax, da * 57.3f,
           wrapd(cars[pcar].x - x0), wrapd(cars[pcar].y - y0));
}
int main(void)
{
    game_save_t sv = { 120, 0, 0, 0 };
    game_init(777, &sv);
    int f = 0;
    const float dt = 1.0f / 30;
    for (; f < 100; f++) {   // walk to car 0 and get in
        game_input_t in = {0};
        float dx = wrapd(cars[0].x - plx), dy = wrapd(cars[0].y - ply), d = sqrtf(dx*dx+dy*dy)+0.01f;
        if (f == 5) in.held |= BTN_FIRE;
        if (d > 6) { in.mx = dx/d; in.my = dy/d; } else if (f > 60) in.held |= BTN_ENTER;
        game_update(&in, dt);
        if (pcar >= 0) break;
    }
    printf("pcar=%d col=%d\n", pcar, pcar >= 0 ? cars[pcar].col : -1);
    if (pcar < 0) return 1;
    cars[pcar].hp = 9999; cars[pcar].flat = 0;
    run("throttle up 3s", 0, -1, 0, 3, &f);
    run("throttle+right 1.5s", 1, -1, 0, 1.5f, &f);
    run("throttle+left 1.5s", -1, -1, 0, 1.5f, &f);
    run("brake 1.5s", 0, 1, 0, 1.5f, &f);
    run("reverse 2s", 0, 1, 0, 2, &f);
    run("reverse+right 1s", 1, 1, 0, 1, &f);
    run("throttle 2s", 0, -1, 0, 2, &f);
    run("handbrake 1.5s", 0, 0, BTN_RUN, 1.5f, &f);
    run("steer in place 1s", 1, 0, 0, 1, &f);
    return 0;
}
