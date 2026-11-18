// PC version: same game.c as on ESP32-S3, but output goes to an SDL2 window.
// Controls: keyboard, mouse or any gamepad (XInput/DInput, Bluetooth or wired).
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "../main/game.h"
#include "../main/pad_map.h"

#define SCALE 3
#define CHUNK 30

static uint16_t frame[SW * SH];
static uint16_t rgb[SW * SH];

static void load_save(game_save_t *s)
{
    FILE *f = fopen("gtasave.dat", "rb");
    if (!f) return;
    if (fread(s, sizeof(*s), 1, f) != 1) memset(s, 0, sizeof(*s));
    fclose(f);
}

static void write_save(void)
{
    game_save_t s;
    game_get_save(&s);
    FILE *f = fopen("gtasave.dat", "wb");
    if (!f) return;
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

static void screenshot(void)
{
    static int n = 0;
    char name[32];
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(rgb, SW, SH, 16, SW * 2, SDL_PIXELFORMAT_RGB565);
    if (!s) return;
    for (;;) {
        snprintf(name, sizeof(name), "shot%04d.bmp", ++n);
        FILE *t = fopen(name, "rb");
        if (!t) break;
        fclose(t);
    }
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
    game_message(name, 1.5f);
}

static float axis(SDL_GameController *gc, SDL_GameControllerAxis a)
{
    int v = SDL_GameControllerGetAxis(gc, a);
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

static size_t world_rd(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("GTA ESP32-S3 - PC emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SW * SCALE, SH * SCALE, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, SW, SH);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SW, SH);

    game_save_t save = {0};
    load_save(&save);
    {   // world WORLD.BIN next to the exe; if missing, generated here (takes a few seconds)
        FILE *wf = fopen("WORLD.BIN", "rb");
        if (!wf) wf = fopen("world.bin", "rb");
        if (wf) game_set_world_source(world_rd, wf);
        game_init((uint32_t)SDL_GetTicks() ^ 0x9E3779B9u, &save);
        if (wf) fclose(wf);
    }
    {   // graphics from GTA.PAK next to the exe (as from the SD card on ESP32)
        static const char *names[] = { "GTA.PAK", "gta.pak" };
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(names[i], "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *buf = malloc(n);
            if (buf && fread(buf, 1, n, f) == (size_t)n) game_set_pak(buf, n);
            fclose(f);
            break;
        }
    }

    SDL_GameController *gc = NULL;
    bool esp_speed = true;          // 30 FPS cap, as on ESP32 with SPI at 40 MHz
    bool fullscreen = false;
    Uint64 prev = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    bool shot_key = false;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !gc) {
                gc = SDL_GameControllerOpen(e.cdevice.which);
                if (gc) game_message("GAMEPAD CONNECTED", 2);
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && gc &&
                       e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))) {
                SDL_GameControllerClose(gc);
                gc = NULL;
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_F1: esp_speed = !esp_speed; game_message(esp_speed ? "30 FPS (ESP)" : "60 FPS", 1); break;
                case SDLK_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                case SDLK_F12: shot_key = true; break;
                case SDLK_F2: game_debug(1); break;
                case SDLK_F3: game_debug(2); break;
                case SDLK_F4: game_debug(3); break;      // cinema mode
                case SDLK_F5: game_debug(4); break;      // replay of the last 10 s
                }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - prev) / freq);
        if (esp_speed && dt < 1.0f / 30) { SDL_Delay((Uint32)((1.0f / 30 - dt) * 1000)); continue; }
        prev = now;
        if (dt > 0.05f) dt = 0.05f;

        // ---- keyboard and mouse ----
        game_input_t in = {0};
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT])  in.mx -= 1;
        if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) in.mx += 1;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP])    in.my -= 1;
        if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN])  in.my += 1;
        if (in.mx && in.my) { in.mx *= 0.7071f; in.my *= 0.7071f; }
        if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_J] || k[SDL_SCANCODE_LCTRL]) in.held |= BTN_FIRE;
        if (k[SDL_SCANCODE_E] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_K])   in.held |= BTN_ENTER;
        if (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_TAB] || k[SDL_SCANCODE_L])      in.held |= BTN_WEAPON;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT])                   in.held |= BTN_RUN;
        if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_P])                        in.held |= BTN_START;
        if (shot_key) { in.held |= BTN_SHOT; shot_key = false; }

        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        if (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) {
            // mouse: aim from window center, LMB = fire
            int ww, wh;
            SDL_GetWindowSize(win, &ww, &wh);
            float ax = (mx - ww / 2.0f), ay = (my - wh / 2.0f);
            float l = sqrtf(ax * ax + ay * ay);
            if (l > 4) { in.ax = ax / l; in.ay = ay / l; in.aim = true; }
            in.held |= BTN_FIRE;
        }

        // ---- gamepad (Xbox layout, same as on ESP) ----
        if (gc) {
            xbox_state_t xs = {0};
            xs.lx = axis(gc, SDL_CONTROLLER_AXIS_LEFTX);
            xs.ly = axis(gc, SDL_CONTROLLER_AXIS_LEFTY);
            xs.rx = axis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
            xs.ry = axis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
            xs.lt = axis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            xs.rt = axis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            static const struct { SDL_GameControllerButton b; uint16_t x; } map[] = {
                { SDL_CONTROLLER_BUTTON_A, XB_A }, { SDL_CONTROLLER_BUTTON_B, XB_B },
                { SDL_CONTROLLER_BUTTON_X, XB_X }, { SDL_CONTROLLER_BUTTON_Y, XB_Y },
                { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XB_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XB_RB },
                { SDL_CONTROLLER_BUTTON_START, XB_START }, { SDL_CONTROLLER_BUTTON_BACK, XB_BACK },
                { SDL_CONTROLLER_BUTTON_DPAD_UP, XB_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XB_DOWN },
                { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XB_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XB_RIGHT },
            };
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                if (SDL_GameControllerGetButton(gc, map[i].b)) xs.btn |= map[i].x;
            game_input_t p = {0};
            pad_from_xbox(&xs, &p);
            if (p.mx || p.my) { in.mx = p.mx; in.my = p.my; }
            if (p.aim) { in.ax = p.ax; in.ay = p.ay; in.aim = true; }
            in.held |= p.held;
        }
        game_set_status(gc != NULL, true, gc ? SDL_GameControllerName(gc) : NULL);

        game_update(&in, dt);
        {   // rumble to the gamepad (if it supports vibration)
            static uint8_t last_r = 0;
            uint8_t r = game_rumble();
            if (gc && r != last_r) SDL_GameControllerRumble(gc, r * 257, r * 200, 120);
            last_r = r;
        }
        if (game_take_save_request()) write_save();

        // ---- render in the same strips as on ESP ----
        game_begin_frame();
        for (int y = 0; y < SH; y += CHUNK) {
            int lines = (y + CHUNK > SH) ? SH - y : CHUNK;
            game_render_chunk(frame + y * SW, y, lines);
        }
        for (int i = 0; i < SW * SH; i++) rgb[i] = (uint16_t)((frame[i] >> 8) | (frame[i] << 8));
        if (game_take_screenshot_request()) screenshot();

        SDL_UpdateTexture(tex, NULL, rgb, SW * 2);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    write_save();
    if (gc) SDL_GameControllerClose(gc);
    SDL_Quit();
    return 0;
}
