#include <math.h>
#include "pad_map.h"

static float dz(float v, float d)
{
    if (fabsf(v) < d) return 0;
    return (v > 0) ? (v - d) / (1 - d) : (v + d) / (1 - d);
}

void pad_from_xbox(const xbox_state_t *s, game_input_t *o)
{
    o->mx = dz(s->lx, 0.18f);
    o->my = dz(s->ly, 0.18f);
    if (s->btn & (XB_UP | XB_DOWN | XB_LEFT | XB_RIGHT)) {
        o->mx = (s->btn & XB_RIGHT) ? 1 : (s->btn & XB_LEFT) ? -1 : 0;
        o->my = (s->btn & XB_DOWN) ? 1 : (s->btn & XB_UP) ? -1 : 0;
    }
    float rx = dz(s->rx, 0.3f), ry = dz(s->ry, 0.3f);
    o->aim = rx * rx + ry * ry > 0.09f;
    o->ax = rx; o->ay = ry;

    uint16_t h = 0;
    if (s->btn & XB_A) h |= BTN_FIRE;
    if (s->btn & XB_B) h |= BTN_ENTER;
    if (s->btn & XB_X) h |= BTN_WEAPON;
    if (s->btn & XB_Y) h |= BTN_RUN;
    if (s->btn & XB_LB) h |= BTN_WEAPON_PREV;
    if (s->btn & XB_RB) h |= BTN_RUN;
    if (s->btn & XB_START) h |= BTN_START;
    if (s->btn & XB_BACK) h |= BTN_SHOT;
    if (s->rt > 0.5f) h |= BTN_FIRE;
    if (s->lt > 0.5f) h |= BTN_RUN;
    o->held = h;
}

// HID button layout (button number - 1):
//  Xbox BLE: A=0 B=1 X=3 Y=4 LB=6 RB=7 View=10 Menu=11
//  Typical DInput: 0..3 = (A/B/X/Y in varying order), LB=4 RB=5 Select=8 Start=9
void pad_from_hid(const hid_pad_state_t *s, bool right_ok, game_input_t *o)
{
    float lx = dz(s->lx, 0.18f), ly = dz(s->ly, 0.18f);
    static const int8_t hx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int8_t hy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    if (s->hat >= 0 && s->hat < 8) { lx = hx[s->hat]; ly = hy[s->hat]; }
    o->mx = lx;
    o->my = ly;

    o->aim = false;
    if (s->has_right_stick && right_ok) {
        float rx = dz(s->rx, 0.3f), ry = dz(s->ry, 0.3f);
        if (rx * rx + ry * ry > 0.09f) { o->ax = rx; o->ay = ry; o->aim = true; }
    }

    uint32_t b = s->buttons;
    uint16_t h = 0;
    if (b & (1u << 0)) h |= BTN_FIRE;
    if (b & (1u << 1)) h |= BTN_ENTER;
    if (b & ((1u << 2) | (1u << 3))) h |= BTN_WEAPON;
    if (b & (1u << 4)) h |= BTN_RUN;
    if (b & (1u << 6)) h |= BTN_WEAPON_PREV;
    if (b & ((1u << 5) | (1u << 7))) h |= BTN_RUN;
    if (b & ((1u << 9) | (1u << 11))) h |= BTN_START;
    if (b & ((1u << 8) | (1u << 10))) h |= BTN_SHOT;
    if (s->has_triggers) {
        if (s->rt > 0.5f) h |= BTN_FIRE;
        if (s->lt > 0.5f) h |= BTN_RUN;
    }
    o->held = h;
}
