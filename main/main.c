// GTA prototype for ESP32-S3: platform part (display, input, Bluetooth, SD).
// The game is in game.c, which also builds on PC (host/sim.c).
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_system.h"
#include <stdarg.h>
#include "nvs_flash.h"

#include "pins.h"
#include "game.h"
#include "input_local.h"
#include "bt_gamepad.h"
#include "usb_gamepad.h"
#include "storage.h"

// Control sources (joystick and buttons always work)
#define USE_BT_GAMEPAD  1   // BLE gamepad
#define USE_USB_GAMEPAD 1   // wired gamepad in the "USB" port (GPIO19/20)

static const char *TAG = "gta";

#define CHUNK_LINES 30            // 8 strips of 30 lines per frame
static uint16_t *chunk[2];
static int cur_chunk = 0;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t trans_done;
static QueueHandle_t save_q;

// ================= Display =================
static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &woken);
    return woken == pdTRUE;
}

static void wait_lcd_idle(void)
{
    xSemaphoreTake(trans_done, portMAX_DELAY);
    xSemaphoreTake(trans_done, portMAX_DELAY);
    xSemaphoreGive(trans_done);
    xSemaphoreGive(trans_done);
}

static void lcd_init(void)
{
    // XPT2046 touch sits on the same bus - keep its CS inactive
    gpio_config_t cs = { .pin_bit_mask = 1ULL << PIN_TOUCH_CS, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cs);
    gpio_set_level(PIN_TOUCH_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SW * CHUNK_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));

    for (int i = 0; i < 2; i++) {
        chunk[i] = heap_caps_malloc(SW * CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(chunk[i]);
    }
    // turn the screen on only after something has been drawn (otherwise display memory "snow" is visible)
}

// ================= Boot screen and diagnostics =================
// The boot step number survives a reboot: if the board crashed, the next start shows exactly where.
#define BOOT_MAGIC 0x47544142u
static RTC_NOINIT_ATTR uint32_t boot_magic, boot_step;
static const char *const BOOT_STEP_NAME[] = { "START", "SD CARD", "WORLD.BIN", "GTA.PAK", "GAMEPADS", "GAME TASK", "FIRST FRAME", "RUNNING" };
enum { BS_START = 0, BS_SD, BS_WORLD, BS_PAK, BS_PADS, BS_TASK, BS_FRAME, BS_RUN };
static char boot_txt[1400];
static bool panel_on = false;

static void boot_show(void)
{
    for (int y = 0; y < SH; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES > SH) ? SH - y : CHUNK_LINES;
        xSemaphoreTake(trans_done, portMAX_DELAY);
        uint16_t *buf = chunk[cur_chunk];
        cur_chunk ^= 1;
        game_draw_boot(buf, y, lines, boot_txt);
        esp_lcd_panel_draw_bitmap(panel, 0, y, SW, y + lines, buf);
    }
    wait_lcd_idle();
    if (!panel_on) { esp_lcd_panel_disp_on_off(panel, true); panel_on = true; }
}
static void boot_log(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "boot: %s", line);
    size_t L = strlen(boot_txt);
    if (L + strlen(line) + 2 < sizeof(boot_txt)) { strcat(boot_txt, line); strcat(boot_txt, "\n"); }
    boot_show();
}
static void boot_set_step(uint32_t s) { boot_magic = BOOT_MAGIC; boot_step = s; }
static const char *reset_name(esp_reset_reason_t r, bool *bad)
{
    *bad = true;
    switch (r) {
    case ESP_RST_POWERON:  *bad = false; return "POWER ON";
    case ESP_RST_SW:       *bad = false; return "SOFTWARE RESET";
    case ESP_RST_EXT:      *bad = false; return "RESET BUTTON";
    case ESP_RST_USB:      *bad = false; return "USB RESET";
    case ESP_RST_PANIC:    return "CRASH (PANIC)";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT - WEAK POWER";
    case ESP_RST_DEEPSLEEP: *bad = false; return "DEEP SLEEP";
    default:               return "OTHER";
    }
}

// ================= SD: background saving (does not stall the frame) =================
static void save_task(void *arg)
{
    game_save_t s;
    while (1) {
        if (xQueueReceive(save_q, &s, portMAX_DELAY) == pdTRUE) {
            storage_save(&s);
            vTaskDelay(pdMS_TO_TICKS(3000));       // at most once every 3 s
        }
    }
}

static void take_screenshot(void)
{
    char name[20];
    wait_lcd_idle();
    if (!storage_shot_begin(SW, SH, name, sizeof(name))) {
        game_message("NO SD CARD", 1.5f);
        return;
    }
    game_begin_frame();
    for (int y = 0; y < SH; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES > SH) ? SH - y : CHUNK_LINES;
        game_render_chunk(chunk[0], y, lines);
        storage_shot_rows(chunk[0], SW * lines);
    }
    storage_shot_end();
    ESP_LOGI(TAG, "screenshot %s", name);
    game_message(name, 1.5f);
}

// ================= Game loop (core 1) =================
static void add_pad(game_input_t *in, const game_input_t *pad)
{
    // gamepad takes priority, but the joystick and buttons work too
    if (pad->mx != 0 || pad->my != 0) { in->mx = pad->mx; in->my = pad->my; }
    if (pad->aim) { in->ax = pad->ax; in->ay = pad->ay; in->aim = true; }
    in->held |= pad->held;
}

static void merge_input(game_input_t *in)
{
    game_input_t loc, pad;
    input_local_read(&loc);
    *in = loc;
#if USE_BT_GAMEPAD
    memset(&pad, 0, sizeof(pad));
    if (bt_gamepad_read(&pad)) add_pad(in, &pad);
#endif
#if USE_USB_GAMEPAD
    memset(&pad, 0, sizeof(pad));
    if (usb_gamepad_read(&pad)) add_pad(in, &pad);
#endif
}

static bool pad_status(const char **name)
{
#if USE_USB_GAMEPAD
    if (usb_gamepad_connected()) { *name = usb_gamepad_name(); return true; }
#endif
#if USE_BT_GAMEPAD
    if (bt_gamepad_connected()) { *name = bt_gamepad_name(); return true; }
#endif
    *name = NULL;
    return false;
}

// ---------- Vibration motor: 20 kHz PWM via LEDC ----------
static void vibro_init(void)
{
#if USE_VIBRO
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = PIN_VIBRO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&c);
#endif
}

static void vibro_set(uint8_t v)
{
#if USE_VIBRO
    static uint8_t last = 0;
    if (v == last) return;
    last = v;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, v < 30 ? 0 : v);   // below 30 the motor does not spin
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    (void)v;
#endif
}

static void game_task(void *arg)
{
    vibro_init();
    int64_t prev = esp_timer_get_time();
    int64_t last_fps = prev, last_status = 0;
    int frames = 0;
    int64_t render_us = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        float dt = (now - prev) / 1e6f;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;

        wait_lcd_idle();
        if (now - last_status > 250000) {
            last_status = now;
            const char *pn;
            bool pc = pad_status(&pn);
            game_set_status(pc, storage_ok(), pn);
        }
        game_input_t in;
        merge_input(&in);
        game_update(&in, dt);
        vibro_set(game_rumble());

        if (game_take_save_request()) {
            game_save_t s;
            game_get_save(&s);
            xQueueOverwrite(save_q, &s);
        }
        if (game_take_screenshot_request()) take_screenshot();

        // draw in strips: while one goes out via DMA, the next is prepared
        int64_t r0 = esp_timer_get_time();
        game_begin_frame();
        for (int y = 0; y < SH; y += CHUNK_LINES) {
            int lines = (y + CHUNK_LINES > SH) ? SH - y : CHUNK_LINES;
            xSemaphoreTake(trans_done, portMAX_DELAY);
            uint16_t *buf = chunk[cur_chunk];
            cur_chunk ^= 1;
            game_render_chunk(buf, y, lines);
            esp_lcd_panel_draw_bitmap(panel, 0, y, SW, y + lines, buf);
        }
        render_us += esp_timer_get_time() - r0;
        if (boot_step == BS_FRAME) boot_set_step(BS_RUN);

        frames++;
        if (now - last_fps >= 2000000) {
            ESP_LOGI(TAG, "FPS %.1f, frame %.1f ms, free heap %u KB (internal %u KB)",
                     frames * 1e6f / (float)(now - last_fps), render_us / 1000.0f / frames,
                     (unsigned)(esp_get_free_heap_size() / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            frames = 0;
            render_us = 0;
            last_fps = now;
        }
        vTaskDelay(1);    // give time to the idle task watchdog
    }
}

void app_main(void)
{
    // Bluetooth needs NVS to store pairing keys
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    trans_done = xSemaphoreCreateCounting(2, 2);
    save_q = xQueueCreate(1, sizeof(game_save_t));

    lcd_init();
    input_local_init();

    // ---- boot screen: reason for the last reset and the step where the board crashed ----
    bool bad;
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rn = reset_name(rr, &bad);
    uint32_t prev_step = (boot_magic == BOOT_MAGIC && boot_step < sizeof(BOOT_STEP_NAME) / sizeof(BOOT_STEP_NAME[0])) ? boot_step : 0xFF;
    boot_txt[0] = 0;
    boot_log("*GTA ESP32-S3");
    boot_log("v1.3.2  BOOT");
    if (bad) {
        boot_log("#LAST RESET: %s", rn);
        if (prev_step != 0xFF) boot_log("#FAILED AT STEP: %s", BOOT_STEP_NAME[prev_step]);
        vTaskDelay(pdMS_TO_TICKS(2500));                    // so there is time to read / photograph it
    } else boot_log("RESET: %s", rn);
    boot_log("PSRAM %u KB, INTERNAL %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 7000 * 1024) boot_log("#PSRAM NOT FOUND OR TOO SMALL - NEED 8 MB");

    boot_set_step(BS_SD);
    game_save_t save = {0};
    bool sd = storage_init();
    if (sd && storage_load(&save))
        ESP_LOGI(TAG, "save loaded: $%ld, kills %ld", (long)save.money, (long)save.kills);
    boot_log(sd ? "+SD CARD OK" : "#SD CARD NOT FOUND");
    xTaskCreatePinnedToCore(save_task, "save", 4096, NULL, 1, NULL, 0);

    {
        boot_set_step(BS_WORLD);
        int64_t t0 = esp_timer_get_time();
        void *wf = storage_world_open();                      // 1024x1024 world from SD (~3.6 MB, PSRAM)
        boot_log(wf ? "LOADING WORLD.BIN..." : "#WORLD.BIN NOT FOUND IN SD ROOT");
        if (wf) game_set_world_source(storage_world_read, wf);
        game_init(esp_random(), &save);
        storage_world_close(wf);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (game_world_loaded()) boot_log("+WORLD OK  %d MS", ms);
        else boot_log("#WORLD.BIN BROKEN OR MISSING");
        boot_log("PSRAM FREE %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    {
        boot_set_step(BS_PAK);
        boot_log("LOADING GTA.PAK...");
        size_t plen;
        const uint8_t *pak = storage_load_pak(&plen);       // graphics from SD -> PSRAM (2.6 MB, ~2 s)
        if (pak && game_set_pak(pak, plen)) boot_log("+GTA.PAK OK");
        else boot_log(pak ? "#GTA.PAK WRONG FORMAT" : "!GTA.PAK NOT LOADED - SIMPLE GRAPHICS");
    }

    // gamepads: Bluetooth takes a lot of internal RAM - start before the game task; an error does not crash the game
    boot_set_step(BS_PADS);
    boot_log("INTERNAL FREE %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
#if USE_USB_GAMEPAD
    usb_gamepad_start();
    boot_log("+USB GAMEPAD HOST OK");
#endif
    bool bt_ok = true;
#if USE_BT_GAMEPAD
    bt_ok = bt_gamepad_start();
    boot_log(bt_ok ? "+BLUETOOTH OK" : "!BLUETOOTH: NO MEMORY - USE JOYSTICK/USB PAD");
#endif
    boot_set_step(BS_TASK);
    boot_log("INTERNAL FREE %u KB, STARTING...", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    vTaskDelay(pdMS_TO_TICKS(bt_ok ? 600 : 2500));
    boot_set_step(BS_FRAME);
    if (xTaskCreatePinnedToCore(game_task, "game", 8192, NULL, 5, NULL, 1) != pdPASS &&
        xTaskCreatePinnedToCore(game_task, "game", 6144, NULL, 5, NULL, 1) != pdPASS) {
        boot_log("#NO MEMORY FOR GAME TASK");
        return;
    }
    if (!bt_ok) game_message("NO BLUETOOTH: LOW MEMORY", 4);
    ESP_LOGI(TAG, "start, free internal heap %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
