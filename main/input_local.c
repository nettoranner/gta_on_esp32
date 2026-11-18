#include <math.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "input_local.h"
#include "pins.h"

static const char *TAG = "input";
static adc_oneshot_unit_handle_t adc;
static int cx = 2048, cy = 2048;
static bool joy_ok = false;

static int read_avg(adc_channel_t ch, int n)
{
    int sum = 0, v = 0;
    for (int i = 0; i < n; i++) { adc_oneshot_read(adc, ch, &v); sum += v; }
    return sum / n;
}

void input_local_init(void)
{
    // buttons: to ground, internal pull-up to 3.3 V
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_K1) | (1ULL << PIN_K2) | (1ULL << PIN_K3) | (1ULL << PIN_K4) | (1ULL << PIN_JOY_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, JOY_ADC_X, &ccfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, JOY_ADC_Y, &ccfg));

    // center calibration: do not touch the joystick at power-on
    cx = read_avg(JOY_ADC_X, 32);
    cy = read_avg(JOY_ADC_Y, 32);
    joy_ok = USE_JOYSTICK && cx > 800 && cx < 3300 && cy > 800 && cy < 3300;
    ESP_LOGI(TAG, "joystick center x=%d y=%d -> %s", cx, cy, joy_ok ? "OK" : "not connected?");
}

bool input_local_joy_present(void) { return joy_ok; }

static float norm(int v, int c)
{
    float f = (v >= c) ? (float)(v - c) / (4095 - c) : (float)(v - c) / c;
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    if (fabsf(f) < JOY_DEADZONE) return 0;
    return (f > 0) ? (f - JOY_DEADZONE) / (1 - JOY_DEADZONE) : (f + JOY_DEADZONE) / (1 - JOY_DEADZONE);
}

void input_local_read(game_input_t *o)
{
    o->mx = o->my = 0;
    o->aim = false;
    o->held = 0;
    if (joy_ok) {
        int rx = read_avg(JOY_ADC_X, 2), ry = read_avg(JOY_ADC_Y, 2);
        float x = norm(rx, cx), y = norm(ry, cy);
#if JOY_SWAP_XY
        float t = x; x = y; y = t;
#endif
#if JOY_INVERT_X
        x = -x;
#endif
#if JOY_INVERT_Y
        y = -y;
#endif
        o->mx = x;
        o->my = y;
    }
    if (!gpio_get_level(PIN_K1)) o->held |= BTN_FIRE;
    if (!gpio_get_level(PIN_K2)) o->held |= BTN_ENTER;
    if (!gpio_get_level(PIN_K3)) o->held |= BTN_WEAPON;
    if (!gpio_get_level(PIN_K4)) o->held |= BTN_RUN;

    // joystick button: short press = pause, hold 1 s = screenshot to SD
    static int64_t down_since = 0;
    static bool long_fired = false;
    bool sw = !gpio_get_level(PIN_JOY_SW);
    int64_t now = esp_timer_get_time();
    static bool start_pulse = false;
    if (sw) {
        if (!down_since) { down_since = now; long_fired = false; }
        if (!long_fired && now - down_since > 1000000) { long_fired = true; o->held |= BTN_SHOT; }
    } else if (down_since) {
        if (!long_fired) start_pulse = true;
        down_since = 0;
    }
    if (start_pulse) { o->held |= BTN_START; start_pulse = false; }
}
