// Pinout for ESP32-S3 N16R8 (DevKitC-1). Change it here.
// Free of flash (26–32), octal PSRAM (33–37), USB (19, 20) and strapping pins (0, 3, 45, 46).
#pragma once

// ---------- Display ST7789 320x240 (SPI2, IOMUX pins - up to 80 MHz) ----------
#define LCD_HOST      SPI2_HOST
#define PIN_SCLK      12
#define PIN_MOSI      11
#define PIN_MISO      13
#define PIN_LCD_CS    10
#define PIN_LCD_DC    9
#define PIN_LCD_RST   8
#define PIN_TOUCH_CS  4          // XPT2046 touch on the same bus: keep CS at "1", touch is not used
// 40 MHz - reliable (~30 FPS). Short wires - try 60/80 MHz (up to ~50–60 FPS).
#define LCD_PCLK_HZ   (40 * 1000 * 1000)

// ---------- Analog joystick (3.3 V supply, NOT 5 V!) ----------
#define USE_JOYSTICK  1               // 0 if no joystick is connected (otherwise "drift" from interference is possible)
#define JOY_ADC_X     ADC_CHANNEL_0   // GPIO1
#define JOY_ADC_Y     ADC_CHANNEL_1   // GPIO2
#define PIN_JOY_SW    14              // joystick button (SW) - pause / hold = screenshot
#define JOY_SWAP_XY   0               // swap the axes if the module is rotated
#define JOY_INVERT_X  0
#define JOY_INVERT_Y  1
#define JOY_DEADZONE  0.15f

// ---------- 4 buttons (common wire -> GND) ----------
#define PIN_K1        5    // shoot
#define PIN_K2        6    // enter / exit car
#define PIN_K3        7    // switch weapon
#define PIN_K4        15   // run / handbrake

// ---------- microSD (separate SPI3 bus, so as not to disturb the display) ----------
#define SD_HOST       SPI3_HOST
#define PIN_SD_SCLK   16
#define PIN_SD_MOSI   17
#define PIN_SD_MISO   18
#define PIN_SD_CS     21
#define SD_MOUNT      "/sdcard"

// ---------- Vibration motor (optional) ----------
// Motor 3 V via a transistor (2N2222/BC337 or a vibration motor module), see README
#define USE_VIBRO     1
#define PIN_VIBRO     47
