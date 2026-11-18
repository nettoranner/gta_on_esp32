// Host simulator: builds the game core on PC, runs a scenario and writes frames to PPM.
// gcc -O2 -o sim sim.c -lm && ./sim out_dir
#include "../main/game.c"
#include <time.h>

static uint16_t frame[SW * SH];

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < SW * SH; i++) {
        uint16_t v = frame[i];
        v = (uint16_t)((v >> 8) | (v << 8));
        uint8_t rgb[3] = { (uint8_t)((v >> 11) << 3), (uint8_t)(((v >> 5) & 63) << 2), (uint8_t)((v & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static double render_frame(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    game_begin_frame();
    for (int y = 0; y < SH; y += 20) game_render_chunk(frame + y * SW, y, 20);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    game_save_t sv = { 120, 0, 0, 0 };
    game_init(777, &sv);
    game_set_status(true, true, "Xbox Wireless Controller");
    const float dt = 1.0f / 30;
    char path[256];
    double rt_sum = 0; int rt_n = 0; double rt_max = 0;
    int shots = 0;
    int max_stars = 0, busted_n = 0, wasted_n = 0, maxcops = 0;

    for (int f = 0; f < 30 * 120; f++) {
        float t = f * dt;
        game_input_t in = {0};
        // --- scenario ---
        if (f == 20) in.held |= BTN_FIRE;                                  // start
        else if (t < 3) {}
        else if (t < 3.4f) { float dx = wrapd(cars[0].x - plx), dy = wrapd(cars[0].y - ply), d = sqrtf(dx*dx+dy*dy)+0.01f; if (d > 6) { in.mx = dx/d; in.my = dy/d; } }  // toward the car
        else if (t < 3.5f) in.held |= BTN_ENTER;
        else if (t < 12) { in.mx = cosf(cars[0].ang); in.my = sinf(cars[0].ang); if (t > 7) { in.mx = 0; in.my = 1; } }  // driving
        else if (t < 13.5f) { in.held |= BTN_RUN; if (t > 13.3f) in.held |= BTN_ENTER; }  // handbrake and exit
        else if (gstate == G_PLAY) {
            // bot: shoots the nearest people, runs from cops
            int tg = -1; float bd = 1e9;
            for (int i = 0; i < NACT; i++) {
                if (!act[i].kind || act[i].state == S_DEAD) continue;
                float d = dist2w(plx, ply, act[i].x, act[i].y);
                if (d < bd) { bd = d; tg = i; }
            }
            if (tg >= 0 && bd < 150 * 150) {
                float dx = wrapd(act[tg].x - plx), dy = wrapd(act[tg].y - ply);
                float d = sqrtf(dx * dx + dy * dy);
                in.mx = dx / d * 0.3f; in.my = dy / d * 0.3f;
                if ((f % 4) < 2) in.held |= BTN_FIRE;
            } else {
                in.mx = sinf(t * 0.3f); in.my = cosf(t * 0.2f);
            }
            if ((f % 300) == 150) in.held |= BTN_WEAPON;
        }
        

        game_update(&in, dt);
        if (stars > max_stars) max_stars = stars;
        static int pst = 0;
        if (gstate == G_BUSTED && pst != G_BUSTED) busted_n++;
        if (gstate == G_WASTED && pst != G_WASTED) wasted_n++;
        pst = gstate;
        int nc = count_kind(A_COP, true); if (nc > maxcops) maxcops = nc;

        double ms = render_frame();
        rt_sum += ms; rt_n++; if (ms > rt_max) rt_max = ms;
        if ((argc > 2 && f >= atoi(argv[2]) && f < atoi(argv[2]) + 450 && f % 2 == 0) || f % 45 == 0 || (gstate >= G_WASTED && state_t > 0.5f && state_t < 0.54f)) {
            snprintf(path, sizeof(path), "%s/f%05d.ppm", out, f);
            save_ppm(path);
            shots++;
        }
        { static int offroad = 0, near_ai = 0; int nr = 0;
          for (int i = 0; i < NCARS; i++) if (cars[i].mode == CAR_AI) {
              if (!lanes[MI(((int)cars[i].x) >> 4, ((int)cars[i].y) >> 4)]) offroad++;
              if (dist2w(cars[i].x, cars[i].y, plx, ply) < 400 * 400) nr++; }
          near_ai += nr;
          if (f == 30 * 120 - 1) printf("AI off-lane samples %d, avg AI cars within 400px %.1f\n", offroad, near_ai / (float)(f + 1)); }
        if (f % 150 == 0)
            printf("t=%5.1f st=%d hp=%3d stars=%d heat=%4.1f money=%ld kills=%ld cops=%d pcar=%d wpn=%d ammo=%d\n",
                   t, gstate, php, stars, heat, (long)money, (long)kills, nc, pcar, weapon, ammo[weapon]);
    }
    printf("render avg %.3f ms max %.3f ms; frames saved %d\n", rt_sum / rt_n, rt_max, shots);
    printf("max stars %d, busted %d, wasted %d, max cops on foot %d, textures %d\n", max_stars, busted_n, wasted_n, maxcops, ntex);
    return 0;
}
