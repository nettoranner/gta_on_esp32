// Highway chase: 4 stars, the player's car drives along HW1 from city A to city B (checks pathfinding in the 256x256 window)
#include "../main/game.c"
static size_t f_rd(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }
int main(int argc, char **argv)
{
    game_save_t sv = { 500, 0, 0, 0, 0 };
    FILE *wf = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (wf) game_set_world_source(f_rd, wf);
    game_init(99, &sv);
    const float dt = 1.0f / 30;
    for (int f = 0; f < 30; f++) { game_input_t in = {0}; if (f == 5) in.held = BTN_FIRE; game_update(&in, dt); }
    pcar = 0; cars[0].mode = CAR_PLAYER; cars[0].x = 410 * 16; cars[0].y = 497 * 16 + 8; cars[0].ang = 0; cars[0].speed = 0; cars[0].hp = 9999;
    plx = cars[0].x; ply = cars[0].y;
    add_heat(15.5f);
    int maxcop = 0, chase = 0;
    for (int f = 0; f < 30 * 40; f++) {
        game_input_t in = {0};
        in.my = -1;
        if (pcar >= 0) { float dy = (497 * 16 + 8) - cars[pcar].y; in.mx = dy > 3 ? 0.3f : dy < -3 ? -0.3f : 0; }
        game_update(&in, dt);
        int nc = 0; for (int i = NCIVCARS; i < POLICE_END; i++) if (cars[i].mode == CAR_COP) nc++;
        if (nc > maxcop) maxcop = nc;
        chase += nc;
        if (f % 150 == 0) printf("t=%4.1f st=%d stars=%d player tile %d,%d cop cars %d hp %d\n", f * dt, gstate, stars, ((int)plx) >> 4, ((int)ply) >> 4, nc, php);
    }
    printf("max cop cars chasing %d, avg %.2f\n", maxcop, chase / (30.0f * 40));
    return 0;
}
