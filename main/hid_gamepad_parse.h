// Mini HID Report Descriptor parser for gamepads (no ESP-IDF dependencies).
// Finds X/Y axes (left stick), Z/Rz or Rx/Ry (right stick), the D-pad (Hat),
// buttons and triggers in the descriptor, then parses input reports of any BLE gamepad using that layout.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t  report_id;
    uint16_t bit_off;
    uint8_t  bits;
    int32_t  lmin, lmax;
    bool     present;
} hid_field_t;

enum { HF_X = 0, HF_Y, HF_Z, HF_RX, HF_RY, HF_RZ, HF_HAT, HF_ACCEL, HF_BRAKE, HF_COUNT };

#define HID_MAX_BUTTONS 16

typedef struct {
    hid_field_t f[HF_COUNT];
    hid_field_t btn[HID_MAX_BUTTONS];
    bool has_report_ids;
    uint8_t main_report_id;      // report that carries X/Y
} hid_layout_t;

typedef struct {
    float lx, ly, rx, ry;        // -1..1
    float lt, rt;                // 0..1
    int   hat;                   // -1 = none, 0..7 (0 = up, clockwise)
    uint32_t buttons;            // bit i = button i+1
    bool  has_right_stick;
    bool  has_triggers;
} hid_pad_state_t;

bool hid_parse_layout(const uint8_t *desc, size_t len, hid_layout_t *out);
// data = report WITHOUT the report id byte (as esp_hidh delivers it for BLE)
bool hid_parse_report(const hid_layout_t *l, uint8_t report_id, const uint8_t *data, size_t len, hid_pad_state_t *st);
