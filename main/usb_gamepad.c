// Wired USB gamepad via the built-in USB OTG of the ESP32-S3 (the "USB" port, GPIO19/20).
// Supported:
//  * XInput (Xbox 360 and compatible: class 0xFF/0x5D/0x01) - the "PC/XInput" mode of most gamepads;
//  * Xbox One / Series wired (GIP: 0xFF/0x47/0xD0);
//  * plain HID gamepad (DInput mode) - layout taken from the HID descriptor.
// Note: the ESP32-S3 USB port does not supply 5 V itself - VBUS power must be provided to the gamepad (see README).

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb_gamepad.h"
#include "hid_gamepad_parse.h"
#include "pad_map.h"

static const char *TAG = "usbpad";

enum { PAD_NONE = 0, PAD_XINPUT, PAD_GIP, PAD_HID };

static usb_host_client_handle_t client;
static usb_device_handle_t dev;
static volatile uint8_t new_addr = 0;
static volatile bool dev_gone = false;
static volatile int kind = PAD_NONE;
static uint8_t iface_num, ep_in, ep_out;
static uint16_t ep_in_mps;
static usb_transfer_t *xin, *xctrl, *xout;
static SemaphoreHandle_t ctrl_done;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static xbox_state_t xs;                 // XInput / GIP
static hid_layout_t hl;                 // HID
static hid_pad_state_t hs;
static bool hid_right_ok = false;
static char name[24] = "USB PAD";

// ---------------- report parsing ----------------
static inline int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline float ax16(int16_t v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

static void parse_xinput(const uint8_t *d, int n)
{
    if (n < 14 || d[0] != 0x00 || d[1] < 0x14) return;
    uint16_t b = d[2] | (d[3] << 8);
    xbox_state_t s = {0};
    if (b & 0x0001) s.btn |= XB_UP;
    if (b & 0x0002) s.btn |= XB_DOWN;
    if (b & 0x0004) s.btn |= XB_LEFT;
    if (b & 0x0008) s.btn |= XB_RIGHT;
    if (b & 0x0010) s.btn |= XB_START;
    if (b & 0x0020) s.btn |= XB_BACK;
    if (b & 0x0100) s.btn |= XB_LB;
    if (b & 0x0200) s.btn |= XB_RB;
    if (b & 0x1000) s.btn |= XB_A;
    if (b & 0x2000) s.btn |= XB_B;
    if (b & 0x4000) s.btn |= XB_X;
    if (b & 0x8000) s.btn |= XB_Y;
    s.lt = d[4] / 255.0f;
    s.rt = d[5] / 255.0f;
    s.lx = ax16(rd16(d + 6));
    s.ly = -ax16(rd16(d + 8));        // XInput Y axis points up
    s.rx = ax16(rd16(d + 10));
    s.ry = -ax16(rd16(d + 12));
    taskENTER_CRITICAL(&mux); xs = s; taskEXIT_CRITICAL(&mux);
}

static void parse_gip(const uint8_t *d, int n)
{
    if (n < 18 || d[0] != 0x20) return;
    xbox_state_t s = {0};
    uint8_t b0 = d[4], b1 = d[5];
    if (b0 & 0x04) s.btn |= XB_START;
    if (b0 & 0x08) s.btn |= XB_BACK;
    if (b0 & 0x10) s.btn |= XB_A;
    if (b0 & 0x20) s.btn |= XB_B;
    if (b0 & 0x40) s.btn |= XB_X;
    if (b0 & 0x80) s.btn |= XB_Y;
    if (b1 & 0x01) s.btn |= XB_UP;
    if (b1 & 0x02) s.btn |= XB_DOWN;
    if (b1 & 0x04) s.btn |= XB_LEFT;
    if (b1 & 0x08) s.btn |= XB_RIGHT;
    if (b1 & 0x10) s.btn |= XB_LB;
    if (b1 & 0x20) s.btn |= XB_RB;
    s.lt = (d[6] | (d[7] << 8)) / 1023.0f;
    s.rt = (d[8] | (d[9] << 8)) / 1023.0f;
    s.lx = ax16(rd16(d + 10));
    s.ly = -ax16(rd16(d + 12));
    s.rx = ax16(rd16(d + 14));
    s.ry = -ax16(rd16(d + 16));
    taskENTER_CRITICAL(&mux); xs = s; taskEXIT_CRITICAL(&mux);
}

static void parse_hid(const uint8_t *d, int n)
{
    uint8_t rid = 0;
    if (hl.has_report_ids) { if (n < 1) return; rid = d[0]; d++; n--; }
    hid_pad_state_t s;
    taskENTER_CRITICAL(&mux); s = hs; taskEXIT_CRITICAL(&mux);
    if (hid_parse_report(&hl, rid, d, n, &s)) {
        if (s.has_right_stick && s.rx > -0.25f && s.rx < 0.25f && s.ry > -0.25f && s.ry < 0.25f) hid_right_ok = true;
        taskENTER_CRITICAL(&mux); hs = s; taskEXIT_CRITICAL(&mux);
    }
}

// ---------------- transfers ----------------
static void in_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (kind == PAD_XINPUT) parse_xinput(t->data_buffer, t->actual_num_bytes);
        else if (kind == PAD_GIP) parse_gip(t->data_buffer, t->actual_num_bytes);
        else if (kind == PAD_HID) parse_hid(t->data_buffer, t->actual_num_bytes);
    }
    if (kind != PAD_NONE && !dev_gone && t->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        t->num_bytes = ep_in_mps;
        usb_host_transfer_submit(t);
    }
}

static void ctrl_cb(usb_transfer_t *t) { xSemaphoreGive(ctrl_done); }
static void out_cb(usb_transfer_t *t) { (void)t; }

// synchronous control transfer (we pump client events ourselves)
static int ctrl_sync(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    usb_setup_packet_t *s = (usb_setup_packet_t *)xctrl->data_buffer;
    s->bmRequestType = bm;
    s->bRequest = req;
    s->wValue = val;
    s->wIndex = idx;
    s->wLength = len;
    xctrl->num_bytes = sizeof(usb_setup_packet_t) + len;
    xctrl->device_handle = dev;
    xctrl->bEndpointAddress = 0;
    xctrl->callback = ctrl_cb;
    xSemaphoreTake(ctrl_done, 0);
    if (usb_host_transfer_submit_control(client, xctrl) != ESP_OK) return -1;
    for (int i = 0; i < 100; i++) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        if (xSemaphoreTake(ctrl_done, 0) == pdTRUE)
            return xctrl->status == USB_TRANSFER_STATUS_COMPLETED ? xctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t) : -1;
    }
    return -1;
}

static void send_out(const uint8_t *data, int len)
{
    if (!ep_out || !xout) return;
    memcpy(xout->data_buffer, data, len);
    xout->num_bytes = len;
    xout->device_handle = dev;
    xout->bEndpointAddress = ep_out;
    xout->callback = out_cb;
    usb_host_transfer_submit(xout);
}

// ---------------- connection ----------------
static bool try_interface(const usb_config_desc_t *cfg, const usb_intf_desc_t *intf, int offset)
{
    int k = PAD_NONE;
    if (intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == 0x5D && intf->bInterfaceProtocol == 0x01) k = PAD_XINPUT;
    else if (intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == 0x47 && intf->bInterfaceProtocol == 0xD0) k = PAD_GIP;
    else if (intf->bInterfaceClass == 0x03) k = PAD_HID;
    if (k == PAD_NONE) return false;

    uint8_t in = 0, out = 0;
    uint16_t mps = 0;
    for (int e = 0; e < intf->bNumEndpoints; e++) {
        int off = offset;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, cfg->wTotalLength, &off);
        if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_INTR) continue;
        if (USB_EP_DESC_GET_EP_DIR(ep)) { if (!in) { in = ep->bEndpointAddress; mps = USB_EP_DESC_GET_MPS(ep); } }
        else if (!out) out = ep->bEndpointAddress;
    }
    if (!in) return false;
    if (usb_host_interface_claim(client, dev, intf->bInterfaceNumber, intf->bAlternateSetting) != ESP_OK) return false;

    if (k == PAD_HID) {
        // HID report descriptor length - from the class HID descriptor that follows right after the interface
        uint16_t rlen = 0;
        int off = offset;
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)intf;
        while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;
            if (d->bDescriptorType == 0x21 && d->bLength >= 9) { const uint8_t *p = (const uint8_t *)d; rlen = p[7] | (p[8] << 8); break; }
        }
        if (rlen == 0 || rlen > 1000) rlen = 512;
        ctrl_sync(0x21, 0x0A, 0, intf->bInterfaceNumber, 0);                  // SET_IDLE(0)
        int got = ctrl_sync(0x81, 0x06, 0x2200, intf->bInterfaceNumber, rlen); // GET_DESCRIPTOR(Report)
        if (got <= 0 || !hid_parse_layout(xctrl->data_buffer + sizeof(usb_setup_packet_t), got, &hl)) {
            ESP_LOGI(TAG, "HID interface %d is not a gamepad", intf->bInterfaceNumber);
            usb_host_interface_release(client, dev, intf->bInterfaceNumber);
            return false;
        }
        memset(&hs, 0, sizeof(hs));
        hs.hat = -1;
        hid_right_ok = false;
    }

    iface_num = intf->bInterfaceNumber;
    ep_in = in; ep_out = out; ep_in_mps = mps ? mps : 64;
    memset(&xs, 0, sizeof(xs));
    kind = k;

    if (k == PAD_GIP) {
        static const uint8_t start[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };    // "enable" Xbox One/Series
        send_out(start, sizeof(start));
    } else if (k == PAD_XINPUT) {
        static const uint8_t led[] = { 0x01, 0x03, 0x06 };                   // ring: player 1
        send_out(led, sizeof(led));
    }
    xin->device_handle = dev;
    xin->bEndpointAddress = ep_in;
    xin->callback = in_cb;
    xin->num_bytes = ep_in_mps;
    usb_host_transfer_submit(xin);
    static const char *kn[] = { "", "XINPUT", "XBOX ONE", "HID" };
    snprintf(name, sizeof(name), "USB %s", kn[k]);
    ESP_LOGI(TAG, "gamepad connected: %s (iface %d, ep 0x%02x, mps %d)", kn[k], iface_num, ep_in, ep_in_mps);
    return true;
}

static void open_device(uint8_t addr)
{
    if (usb_host_device_open(client, addr, &dev) != ESP_OK) return;
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    ESP_LOGI(TAG, "device VID %04x PID %04x class %02x", dd->idVendor, dd->idProduct, dd->bDeviceClass);
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        if (intf->bAlternateSetting != 0) continue;
        if (try_interface(cfg, intf, off)) return;
    }
    ESP_LOGW(TAG, "no gamepad interface found — switch the gamepad mode (XInput/DInput)");
}

static void close_device(void)
{
    kind = PAD_NONE;
    if (dev) {
        usb_host_endpoint_halt(dev, ep_in);
        usb_host_endpoint_flush(dev, ep_in);
        usb_host_interface_release(client, dev, iface_num);
        usb_host_device_close(client, dev);
        dev = NULL;
    }
    ESP_LOGI(TAG, "gamepad disconnected");
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) dev_gone = true;
}

static void host_lib_task(void *arg)
{
    usb_host_config_t cfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    if (usb_host_install(&cfg) != ESP_OK) { ESP_LOGE(TAG, "usb_host_install failed"); vTaskDelete(NULL); }
    xTaskNotifyGive((TaskHandle_t)arg);
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);       // wait for the host to be installed
    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    if (usb_host_client_register(&cc, &client) != ESP_OK) { ESP_LOGE(TAG, "USB client register failed"); vTaskDelete(NULL); return; }
    usb_host_transfer_alloc(64, 0, &xin);
    usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 1024, 0, &xctrl);
    usb_host_transfer_alloc(64, 0, &xout);
    ESP_LOGI(TAG, "USB host ready — connect a gamepad to port USB (GPIO19/20)");
    while (1) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        if (dev_gone) { dev_gone = false; close_device(); }
        if (new_addr) {
            uint8_t a = new_addr;
            new_addr = 0;
            if (!dev) open_device(a);
        }
    }
}

void usb_gamepad_start(void)
{
    ctrl_done = xSemaphoreCreateBinary();
    TaskHandle_t ct;
    xTaskCreatePinnedToCore(client_task, "usb_cli", 4096, NULL, 3, &ct, 0);
    xTaskCreatePinnedToCore(host_lib_task, "usb_lib", 4096, ct, 4, NULL, 0);
}

bool usb_gamepad_connected(void) { return kind != PAD_NONE; }
const char *usb_gamepad_name(void) { return name; }

bool usb_gamepad_read(game_input_t *o)
{
    int k = kind;
    if (k == PAD_NONE) return false;
    if (k == PAD_HID) {
        hid_pad_state_t s;
        taskENTER_CRITICAL(&mux); s = hs; taskEXIT_CRITICAL(&mux);
        pad_from_hid(&s, hid_right_ok, o);
    } else {
        xbox_state_t s;
        taskENTER_CRITICAL(&mux); s = xs; taskEXIT_CRITICAL(&mux);
        pad_from_xbox(&s, o);
    }
    return true;
}
