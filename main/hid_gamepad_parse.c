#include "hid_gamepad_parse.h"
#include <string.h>

#define MAX_USAGES 16
#define MAX_IDS    16

static int32_t item_signed(uint32_t v, int size)
{
    if (size == 1) return (int8_t)v;
    if (size == 2) return (int16_t)v;
    return (int32_t)v;
}

bool hid_parse_layout(const uint8_t *d, size_t len, hid_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t usage_page = 0, rsize = 0, rcount = 0;
    int32_t lmin = 0, lmax = 0;
    uint8_t rid = 0;
    uint32_t usages[MAX_USAGES];
    int nus = 0;
    uint32_t umin = 0, umax = 0;
    bool urange = false;
    // push/pop stack for globals
    struct { uint32_t up, rs, rc; int32_t mn, mx; uint8_t id; } stack[4];
    int sp = 0;
    // current bit offset in each report
    uint8_t ids[MAX_IDS] = {0};
    uint16_t pos[MAX_IDS] = {0};
    int nids = 1;     // ids[0] = 0 = report without id

    size_t i = 0;
    while (i < len) {
        uint8_t p = d[i++];
        if (p == 0xFE) {                        // long item
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        int size = p & 3;
        if (size == 3) size = 4;
        int type = (p >> 2) & 3, tag = (p >> 4) & 0xF;
        if (i + size > len) break;
        uint32_t v = 0;
        for (int k = 0; k < size; k++) v |= (uint32_t)d[i + k] << (8 * k);
        i += size;

        if (type == 1) {                       // Global
            switch (tag) {
            case 0x0: usage_page = v; break;
            case 0x1: lmin = item_signed(v, size); break;
            case 0x2: lmax = item_signed(v, size); break;
            case 0x7: rsize = v; break;
            case 0x8: {
                rid = (uint8_t)v;
                out->has_report_ids = true;
                bool found = false;
                for (int k = 0; k < nids; k++) if (ids[k] == rid) found = true;
                if (!found && nids < MAX_IDS) { ids[nids] = rid; pos[nids] = 0; nids++; }
                break;
            }
            case 0x9: rcount = v; break;
            case 0xA: if (sp < 4) { stack[sp].up = usage_page; stack[sp].rs = rsize; stack[sp].rc = rcount;
                                    stack[sp].mn = lmin; stack[sp].mx = lmax; stack[sp].id = rid; sp++; } break;
            case 0xB: if (sp > 0) { sp--; usage_page = stack[sp].up; rsize = stack[sp].rs; rcount = stack[sp].rc;
                                    lmin = stack[sp].mn; lmax = stack[sp].mx; rid = stack[sp].id; } break;
            }
        } else if (type == 2) {                // Local
            uint32_t full = (size == 4) ? v : ((usage_page << 16) | v);
            switch (tag) {
            case 0x0: if (nus < MAX_USAGES) usages[nus++] = full; break;
            case 0x1: umin = full; urange = true; break;
            case 0x2: umax = full; urange = true; break;
            }
        } else if (type == 0) {                // Main
            if (tag == 0x8) {                  // Input
                int slot = 0;
                for (int k = 0; k < nids; k++) if (ids[k] == rid) slot = k;
                uint16_t bit = pos[slot];
                bool constant = v & 1, variable = (v & 2) != 0;
                if (!constant && variable) {
                    for (uint32_t n = 0; n < rcount; n++) {
                        uint32_t u;
                        if (nus > 0) u = usages[n < (uint32_t)nus ? n : (uint32_t)nus - 1];
                        else if (urange) u = umin + n;
                        else u = 0;
                        if (urange && nus == 0 && u > umax) u = umax;
                        hid_field_t fld = { rid, (uint16_t)(bit + n * rsize), (uint8_t)rsize, lmin, lmax, true };
                        uint16_t page = u >> 16, id = u & 0xFFFF;
                        int which = -1;
                        if (page == 0x01) {
                            switch (id) {
                            case 0x30: which = HF_X; break;  case 0x31: which = HF_Y; break;
                            case 0x32: which = HF_Z; break;  case 0x33: which = HF_RX; break;
                            case 0x34: which = HF_RY; break; case 0x35: which = HF_RZ; break;
                            case 0x39: which = HF_HAT; break;
                            }
                        } else if (page == 0x02) {
                            if (id == 0xC4) which = HF_ACCEL;
                            if (id == 0xC5) which = HF_BRAKE;
                        } else if (page == 0x09 && id >= 1 && id <= HID_MAX_BUTTONS) {
                            if (!out->btn[id - 1].present) out->btn[id - 1] = fld;
                        }
                        if (which >= 0 && !out->f[which].present) {
                            out->f[which] = fld;
                            if (which == HF_X) out->main_report_id = rid;
                        }
                    }
                }
                pos[slot] = bit + rsize * rcount;
            }
            // any main item resets the locals
            nus = 0; urange = false; umin = umax = 0;
        }
    }
    return out->f[HF_X].present && out->f[HF_Y].present;
}

static uint32_t get_bits(const uint8_t *data, size_t len, uint16_t off, uint8_t bits)
{
    uint32_t v = 0;
    for (int b = 0; b < bits; b++) {
        uint32_t bi = off + b;
        if ((bi >> 3) >= len) break;
        if (data[bi >> 3] & (1u << (bi & 7))) v |= 1u << b;
    }
    return v;
}

static bool read_field(const hid_field_t *f, uint8_t rid, const uint8_t *data, size_t len, int32_t *out, int32_t *mn, int32_t *mx)
{
    if (!f->present || f->report_id != rid) return false;
    if (((f->bit_off + f->bits + 7) >> 3) > len) return false;
    uint32_t raw = get_bits(data, len, f->bit_off, f->bits);
    int32_t lo = f->lmin, hi = f->lmax;
    int32_t val;
    if (hi < lo || (lo >= 0)) {
        // unsigned field (or a wrongly recorded maximum, e.g. 0xFFFF as -1)
        if (hi < lo) { lo = 0; hi = (f->bits >= 32) ? 0x7FFFFFFF : (int32_t)((1u << f->bits) - 1); }
        val = (int32_t)raw;
    } else {
        // signed field: sign-extend
        if (f->bits < 32 && (raw & (1u << (f->bits - 1)))) val = (int32_t)(raw | (~0u << f->bits));
        else val = (int32_t)raw;
    }
    *out = val; *mn = lo; *mx = hi;
    return true;
}

static bool axis(const hid_field_t *f, uint8_t rid, const uint8_t *data, size_t len, float *out)
{
    int32_t v, lo, hi;
    if (!read_field(f, rid, data, len, &v, &lo, &hi) || hi == lo) return false;
    float x = ((float)(v - lo) / (float)(hi - lo)) * 2.0f - 1.0f;
    if (x < -1) x = -1;
    if (x > 1) x = 1;
    *out = x;
    return true;
}

bool hid_parse_report(const hid_layout_t *l, uint8_t rid, const uint8_t *data, size_t len, hid_pad_state_t *st)
{
    bool any = false;
    any |= axis(&l->f[HF_X], rid, data, len, &st->lx);
    any |= axis(&l->f[HF_Y], rid, data, len, &st->ly);

    // right stick: Rx/Ry, otherwise Z/Rz
    const hid_field_t *rxf = &l->f[HF_RX], *ryf = &l->f[HF_RY];
    if (!(rxf->present && ryf->present)) { rxf = &l->f[HF_Z]; ryf = &l->f[HF_RZ]; }
    if (rxf->present && ryf->present) {
        st->has_right_stick = true;
        any |= axis(rxf, rid, data, len, &st->rx);
        any |= axis(ryf, rid, data, len, &st->ry);
    }
    float t;
    if (axis(&l->f[HF_ACCEL], rid, data, len, &t)) { st->rt = (t + 1) * 0.5f; st->has_triggers = true; any = true; }
    if (axis(&l->f[HF_BRAKE], rid, data, len, &t)) { st->lt = (t + 1) * 0.5f; st->has_triggers = true; any = true; }

    int32_t v, lo, hi;
    if (read_field(&l->f[HF_HAT], rid, data, len, &v, &lo, &hi)) {
        int h = v - lo;
        int range = hi - lo + 1;
        if (range == 8 && h >= 0 && h < 8) st->hat = h;
        else if (range == 4 && h >= 0 && h < 4) st->hat = h * 2;
        else st->hat = -1;
        any = true;
    }
    bool btn_seen = false;
    uint32_t b = st->buttons;
    for (int i = 0; i < HID_MAX_BUTTONS; i++) {
        if (read_field(&l->btn[i], rid, data, len, &v, &lo, &hi)) {
            btn_seen = true;
            if (v) b |= 1u << i; else b &= ~(1u << i);
        }
    }
    if (btn_seen) { st->buttons = b; any = true; }
    return any;
}
