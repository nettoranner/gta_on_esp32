// BLE gamepad via ESP-IDF esp_hidh (Bluedroid).
// ESP32-S3 supports only Bluetooth LE: gamepads with BLE HID work
// (Xbox Series / Xbox One with firmware 5.x+, 8BitDo in BLE mode, etc.).
// Classic-BT-only gamepads (DualShock 4, DualSense, many cheap ones) are invisible to the S3.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_gap_ble_api.h"
#include "bt_gamepad.h"
#include "hid_gamepad_parse.h"
#include "pad_map.h"

static const char *TAG = "pad";

#define MAX_MAPS 3

static hid_layout_t layouts[MAX_MAPS];
static bool layout_ok[MAX_MAPS];
static hid_pad_state_t pad;
static portMUX_TYPE pad_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool connected = false, connecting = false;
static volatile int64_t last_report_us = 0;
static char pad_name[32] = "";
static bool right_ok = false;   // right stick seen at center = it is a stick, not a trigger

static void on_open(esp_hidh_dev_t *dev)
{
    size_t n = 0;
    esp_hid_raw_report_map_t *maps = NULL;
    memset(layout_ok, 0, sizeof(layout_ok));
    if (esp_hidh_dev_report_maps_get(dev, &n, &maps) == ESP_OK) {
        for (size_t i = 0; i < n && i < MAX_MAPS; i++) {
            layout_ok[i] = hid_parse_layout(maps[i].data, maps[i].len, &layouts[i]);
            ESP_LOGI(TAG, "report map %u: %u bytes, gamepad layout %s", (unsigned)i, maps[i].len,
                     layout_ok[i] ? "OK" : "not found");
        }
    }
    const char *nm = esp_hidh_dev_name_get(dev);
    strncpy(pad_name, nm ? nm : "GAMEPAD", sizeof(pad_name) - 1);
    taskENTER_CRITICAL(&pad_mux);
    memset(&pad, 0, sizeof(pad));
    pad.hat = -1;
    right_ok = false;
    taskEXIT_CRITICAL(&pad_mux);
    connected = true;
    connecting = false;
    ESP_LOGI(TAG, "connected: %s", pad_name);
}

static void hidh_callback(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) on_open(p->open.dev);
        else { ESP_LOGW(TAG, "open failed"); connecting = false; }
        break;
    case ESP_HIDH_INPUT_EVENT: {
        int m = p->input.map_index;
        if (m < 0 || m >= MAX_MAPS || !layout_ok[m]) break;
        hid_pad_state_t s;
        taskENTER_CRITICAL(&pad_mux);
        s = pad;
        taskEXIT_CRITICAL(&pad_mux);
        if (hid_parse_report(&layouts[m], p->input.report_id, p->input.data, p->input.length, &s)) {
            if (s.has_right_stick && fabsf(s.rx) < 0.25f && fabsf(s.ry) < 0.25f) right_ok = true;
            taskENTER_CRITICAL(&pad_mux);
            pad = s;
            taskEXIT_CRITICAL(&pad_mux);
            last_report_us = esp_timer_get_time();
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "disconnected");
        connected = false;
        connecting = false;
        break;
    default:
        break;
    }
}

static bool name_looks_like_pad(const char *n)
{
    if (!n) return false;
    static const char *keys[] = { "Xbox", "Controller", "Gamepad", "GamePad", "8Bit", "8BitDo", "Joy", "Pad", "PRO", "GameSir", "Flydigi" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strstr(n, keys[i])) return true;
    return false;
}

static void scan_task(void *arg)
{
    while (1) {
        if (connected || connecting) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        size_t n = 0;
        esp_hid_scan_result_t *res = NULL;
        if (esp_hid_scan(4, &n, &res) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        esp_hid_scan_result_t *best = NULL;
        int best_score = -1;
        for (esp_hid_scan_result_t *r = res; r; r = r->next) {
            if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
            int score = 0;
            uint16_t app = r->ble.appearance;
            if (app == 0x03C4 || app == 0x03C3) score += 10;          // gamepad / joystick
            if (app == 0x03C1 || app == 0x03C2) score -= 20;          // keyboard / mouse
            if (r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK) score += 10;
            if (name_looks_like_pad(r->name)) score += 5;
            score += (r->rssi + 100) / 20;
            ESP_LOGI(TAG, "found %s app=0x%04x rssi=%d score=%d", r->name ? r->name : "?", app, r->rssi, score);
            if (score > best_score && score >= 0) { best_score = score; best = r; }
        }
        if (best) {
            ESP_LOGI(TAG, "connecting to %s", best->name ? best->name : "?");
            connecting = true;
            esp_hidh_dev_open(best->bda, best->transport, best->ble.addr_type);
        }
        esp_hid_scan_results_free(res);
        // wait for the connection result
        for (int i = 0; i < 150 && connecting; i++) vTaskDelay(pdMS_TO_TICKS(100));
        connecting = false;
    }
}

bool bt_gamepad_start(void)
{
    esp_err_t err = esp_hid_gap_init(HID_HOST_MODE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT init failed: %s", esp_err_to_name(err)); return false; }

    // pairing without PIN entry, with bonding - needed by Xbox and many others
    esp_ble_auth_req_t auth = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (err != ESP_OK) { ESP_LOGE(TAG, "GATTC register failed: %s", esp_err_to_name(err)); return false; }
    esp_hidh_config_t cfg = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "HID host init failed: %s — playing without a BT gamepad", esp_err_to_name(err)); return false; }
    // BT tasks live on core 0, the game on core 1
    if (xTaskCreatePinnedToCore(scan_task, "pad_scan", 4096, NULL, 2, NULL, 0) != pdPASS) { ESP_LOGE(TAG, "no memory for scan task"); return false; }
    ESP_LOGI(TAG, "BLE gamepad host started");
    return true;
}

bool bt_gamepad_connected(void) { return connected; }
const char *bt_gamepad_name(void) { return pad_name; }

bool bt_gamepad_read(game_input_t *o)
{
    if (!connected) return false;
    hid_pad_state_t s;
    taskENTER_CRITICAL(&pad_mux);
    s = pad;
    taskEXIT_CRITICAL(&pad_mux);
    pad_from_hid(&s, right_ok, o);
    return true;
}
