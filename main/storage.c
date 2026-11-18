// microSD: progress saves and BMP screenshots (for video previews).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "storage.h"
#include "pins.h"

static const char *TAG = "sd";
static bool sd_ok = false;
static sdmmc_card_t *card;

#define SAVE_PATH SD_MOUNT "/GTASAVE.DAT"
#define SAVE_MAGIC 0x47544131u   // "GTA1"

bool storage_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "SPI3 init failed");
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s) — playing without saves", esp_err_to_name(err));
        return false;
    }
    sd_ok = true;
    ESP_LOGI(TAG, "SD mounted, %llu MB", ((unsigned long long)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    return true;
}

bool storage_ok(void) { return sd_ok; }

typedef struct { uint32_t magic; game_save_t s; uint32_t sum; } save_file_t;

static uint32_t checksum(const game_save_t *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*s); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool storage_load(game_save_t *s)
{
    if (!sd_ok) return false;
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f) return false;
    save_file_t sf;
    bool ok = fread(&sf, sizeof(sf), 1, f) == 1 && sf.magic == SAVE_MAGIC && sf.sum == checksum(&sf.s);
    fclose(f);
    if (ok) *s = sf.s;
    return ok;
}

void storage_save(const game_save_t *s)
{
    if (!sd_ok) return;
    save_file_t sf = { SAVE_MAGIC, *s, checksum(s) };
    FILE *f = fopen(SAVE_PATH, "wb");
    if (!f) return;
    fwrite(&sf, sizeof(sf), 1, f);
    fclose(f);
}

// ---------- Screenshot: BMP 16-bit (RGB565, BI_BITFIELDS), rows top to bottom ----------
static FILE *shot;

bool storage_shot_begin(int w, int h, char *name_out, int name_len)
{
    if (!sd_ok) return false;
    char path[40];
    for (int n = 1; n < 10000; n++) {
        snprintf(path, sizeof(path), SD_MOUNT "/SHOT%04d.BMP", n);
        struct stat st;
        if (stat(path, &st) != 0) break;
    }
    shot = fopen(path, "wb");
    if (!shot) return false;
    if (name_out) snprintf(name_out, name_len, "%s", path + strlen(SD_MOUNT) + 1);
    uint32_t data = w * h * 2, off = 14 + 40 + 12;
    uint8_t hdr[66] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsize = off + data;
    memcpy(hdr + 2, &fsize, 4);
    memcpy(hdr + 10, &off, 4);
    uint32_t dib = 40; memcpy(hdr + 14, &dib, 4);
    int32_t W = w, H = -h;                       // negative height = top-down
    memcpy(hdr + 18, &W, 4); memcpy(hdr + 22, &H, 4);
    uint16_t planes = 1, bpp = 16; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    uint32_t comp = 3; memcpy(hdr + 30, &comp, 4);          // BI_BITFIELDS
    memcpy(hdr + 34, &data, 4);
    uint32_t masks[3] = { 0xF800, 0x07E0, 0x001F };
    memcpy(hdr + 54, masks, 12);
    fwrite(hdr, 1, sizeof(hdr), shot);
    return true;
}

void storage_shot_rows(const uint16_t *px, int count)
{
    if (!shot) return;
    static uint16_t tmp[320 * 20];
    while (count > 0) {
        int n = count > (int)(sizeof(tmp) / 2) ? (int)(sizeof(tmp) / 2) : count;
        for (int i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));   // back to little-endian
        fwrite(tmp, 2, n, shot);
        px += n;
        count -= n;
    }
}

void storage_shot_end(void)
{
    if (shot) { fclose(shot); shot = NULL; }
}

// ---------- Graphics pack GTA.PAK: SD -> PSRAM GTA.PAK: SD -> PSRAM ----------
const uint8_t *storage_load_pak(size_t *len)
{
    *len = 0;
    if (!sd_ok) return NULL;
    FILE *f = fopen(SD_MOUNT "/GTA.PAK", "rb");
    if (!f) { ESP_LOGI(TAG, "GTA.PAK not found — procedural graphics"); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "no PSRAM for GTA.PAK (%ld bytes)", n); fclose(f); return NULL; }
    size_t got = 0;
    while (got < (size_t)n) {
        size_t r = fread(buf + got, 1, 32768, f);
        if (r == 0) break;
        got += r;
    }
    fclose(f);
    if (got != (size_t)n) { heap_caps_free(buf); return NULL; }
    ESP_LOGI(TAG, "GTA.PAK loaded to PSRAM: %ld KB", n / 1024);
    *len = n;
    return buf;
}

// ---------- World WORLD.BIN: streamed directly into the game's PSRAM structures ----------
void *storage_world_open(void)
{
    if (!sd_ok) return NULL;
    FILE *f = fopen(SD_MOUNT "/WORLD.BIN", "rb");
    if (!f) { ESP_LOGW(TAG, "WORLD.BIN not found on SD"); return NULL; }
    setvbuf(f, NULL, _IOFBF, 16384);           // buffer on the heap (PSRAM), not in static DRAM
    return f;
}
size_t storage_world_read(void *ctx, void *buf, size_t n)
{
    static int calls = 0;
    if ((++calls & 15) == 0) vTaskDelay(1);        // long load: let the idle task run (watchdog)
    return fread(buf, 1, n, (FILE *)ctx);
}
void storage_world_close(void *ctx) { if (ctx) fclose((FILE *)ctx); }
