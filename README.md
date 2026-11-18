# GTA Prototype for ESP32-S3 N16R8 + ST7789 (320×240)

Top-down GTA1-style game prototype for an ESP32-S3 development board (16 MB Flash, 8 MB PSRAM) with a 320×240 ST7789 display.

Key specifications:
- World: 16384×16384 px (1024×1024 tiles). Two cities with distinct urban layouts, an airport, countryside (fields, forests, villages, farms), highways, a port, islands, lakes, and a river.
- Gameplay: AI traffic, pedestrians, vehicle theft, 4 weapon types, NPC kills, up to 5-star wanted level, police on foot and in patrol cars (arrest: `BUSTED`, death: `WASTED`).
- System and UI: minimap, full-world map on pause, pickups (weapons, medkits), delivery missions, progress saving, and screenshots to microSD.

## World and Mechanics

- **Districts**: downtown (offices), affluent district (villas with pools in the northeast), central park (lake, trees, fountain), residential areas (panel apartment blocks with courtyards), ghetto across the canal (dense housing, gangs), industrial zone, and a plaza with a fountain.
- **Shops and interactions** (step onto the colored mat at the entrance on foot: `B/K2` to buy, `X/K3` for next item):
  - `GUN SHOP`: ammo, Uzi, shotgun, body armor ($150, shown as a blue bar under health).
  - `CHIKKO` (fried chicken): restores health for $10 or $25.
  - `AUTO SHOP`: buys a sedan ($300) or a faster sports car ($1200) spawned at the entrance.
  - `CASINO` (purple building with a neon sign): `SPIN` ($100) and `BIG SPIN` ($500). Winning the `BIG SPIN` jackpot (8% chance) unlocks the `VORTEX GT`, the fastest car in the game (top speed ~260 vs. 180, black with a gold stripe). The win is saved to the SD card, and the car remains parked outside the casino.
- **Gangs**: located in the old city ghetto (green in the west, purple in the east) and the port city ghetto (purple). Neutral to the player by default; hitting or shooting one member causes the entire gang to attack for one minute. Gang shootouts occur periodically outside `CHIKKO`.
- **Muggings**: a mugger runs up to a pedestrian (`!` appears over the victim) and flees; a police officer chases and handcuffs the suspect. Killing the mugger awards $100 with no wanted level.
- **Fires**: a building catches fire and emits smoke; a fire truck responds with sirens and extinguishes the fire with water. Active fires appear on the minimap.

## Environment, Missions, and Effects

- **Time of day and weather**: 5-minute day-night cycle (day, warm sunset, blue night). At night, streetlights cast light pools on the ground, car headlights and building windows light up, and police sirens illuminate the asphalt. Rain triggers randomly (dims lighting, draws raindrops).
- **Graphics and physics**: volumetric buildings, trees with shadows and offset canopies, streetlights, and traffic lights (AI traffic stops on red). Boats are available in the strait, port bay, north shore, and south lake. Supports tire skid marks during drifts and handbrake turns, explosion scorch marks, smoke, bullet sparks, blood, and exhaust.
- **Looping missions**: package delivery -> hit contract (eliminate target in black) -> steal a golden car and deliver it to a garage.
- **Pay'n'Spray** (pink `SPRAY` pad in the industrial zone, marked on the map): driving in while wanted clears the wanted level and repaints the car for $100.
- **Kill Frenzy** (red skull): 30 seconds with an unlimited Uzi and +$50 per kill.
- **Hidden packages**: 10 packages worth $250 each in parks and plazas (counter shown on the pause screen).
- **Pedestrians**: passersby stop to chat and gather near fountains.
- **PC simulator hotkeys**: `F2` (+3 hours to time of day), `F3` (toggle rain), `F4` (cinema mode without HUD), `F5` (replay last 10 seconds).

## Project Structure

```text
main/game.c, game.h          - game logic and renderer (hardware-independent)
main/game_plus.inc           - gameplay extensions (single translation unit with game.c)
main/game_world.inc          - 1024x1024 world generator: cities, districts, roads, airport, port, nature
main/game_shore.inc          - shoreline, sand, and grass edge masks (built on the fly per frame)
tools/worldgen.c             - PC world generator -> sd_card/WORLD.BIN (with load check)
sd_card/WORLD.BIN            - prebuilt world (3.6 MB), copy to SD card root
main/main.c                  - display setup, game loop on Core 1, background SD saving
main/pins.h                  - GPIO pinout and hardware settings
main/input_local.c           - analog joystick + 4 buttons
main/bt_gamepad.c            - BLE gamepad (ESP-IDF esp_hidh, Bluedroid)
main/usb_gamepad.c           - wired USB gamepad: XInput, Xbox One (GIP), HID/DInput
main/pad_map.c               - shared gamepad button mapping
main/hid_gamepad_parse.c     - generic HID gamepad descriptor parser
main/esp_hid_gap.c/.h        - BLE scanner from ESP-IDF example (adapted for gamepads)
main/storage.c               - microSD handling: save data and BMP screenshots
host/sim.c                   - PC build wrapper, scenario runner, and frame capture
host/map_test.c, run_test.sh - world tests (road connectivity, dead ends, objects, boats) + map/district renders
host/drive_test.c            - vehicle handling test
host/chase_test.c            - highway chase test (4 stars)
host/fauna_test.c            - biome fauna test + goose scenario (GOOSED)
main/game_fauna.inc          - wildlife and goose logic
firmware/gta_merged.bin      - prebuilt single-file firmware image (flash at 0x0)
```

## Build and Flash (ESP-IDF 5.5)

Tested on ESP-IDF v5.5.1 (builds with zero errors or warnings). Firmware size: 0.9 MB; static RAM usage: 56%.

1. Copy the project directory to a new location.
2. Delete `sdkconfig`, `build/`, `managed_components/`, and `dependencies.lock` so old settings do not override Bluetooth, PSRAM, and partition configurations. The external `esp_lcd_ili9341` component is no longer needed because the ST7789 driver is built into ESP-IDF.
3. Set the target and run the build:
   ```bash
   idf.py set-target esp32s3
   idf.py build flash monitor
   ```
   In VS Code: `Set Espressif Device Target` -> `esp32s3`, then `Build, Flash and Monitor`.

**Flashing the prebuilt binary without building:**
```bash
esptool.py --chip esp32s3 write_flash 0x0 firmware/gta_merged.bin
```
The prebuilt firmware uses the default pinout defined in `main/pins.h`.

## Performance, Memory, and Architecture

### Rendering and SPI
- Base city textures are generated at boot into an atlas of 84 unique 16×16 tiles at 1 byte per pixel (palette index), totaling 21 KB in internal SRAM. Roof colors are set via palette. The inner background loop performs two reads per pixel without accessing flash or SD.
- Frame output uses double-buffered 30-line strips: one strip transfers over DMA while the next is rendered.
- Hot render functions are placed in IRAM (`IRAM_ATTR`), with the game loop pinned to Core 1 and the Bluetooth stack on Core 0. This keeps BLE from evicting render code from the instruction cache and prevents frame stutters.
- Frame rate is limited primarily by SPI bandwidth (a 320×240×16-bit frame is 1.23 Mbit):
  - 20 MHz: ~16 FPS
  - 40 MHz (default): ~30 FPS
  - 80 MHz: up to ~55 FPS (depends on wiring and display tolerance)
  Set the frequency via `LCD_PCLK_HZ` in `main/pins.h`. Display pins use SPI2 IOMUX, enabling operation at these speeds.
- Actual FPS and frame time are printed to the serial monitor every 2 seconds.

### esp-cachectrl
The `esp-cachectrl` library configures ESP32-S3 hardware cache prefetch for sequential reads from flash or PSRAM (claiming up to ~20% speedup on streaming workloads such as neural network weights).

It is not used in this project: all hot data (textures, map, buffers) and render code live in internal SRAM/IRAM, bypassing the cache. Additionally, the prefetch mechanism is undocumented and carries a risk of data corruption. Integrating it would only make sense if large assets (sprites, audio) were moved to PSRAM or flash and read sequentially.

### World Layout and Memory Allocation
- The 1024×1024 map is generated on a PC (`tools/worldgen.c`, ~4 s) and saved to `WORLD.BIN` (3.6 MB). At boot, the ESP32 reads it from SD in 2-3 s. Generating the map on the MCU is not possible because temporary generator data requires over 8 MB of RAM. If `WORLD.BIN` is missing from the SD card, the screen displays `NO WORLD.BIN` (the PC build generates the world automatically at launch in a few seconds).
- PSRAM holds: `map_v` (2 MB), AI lanes (1 MB), shore bitmaps (0.5 MB), lots, trees, and streetlights. Total usage with `GTA.PAK` is ~7.3 MB out of 8 MB.
- The tile atlas supports up to 1024 textures (528 currently used). The 256 most frequent tiles sit in internal SRAM (covering 99.8% of the map), and the rest in PSRAM.
- Shore masks are not stored permanently; they are built on the fly for the visible area into a 32×32 tile cache (128 KB). Moving along a shoreline recalculates an average of 0.2 masks per frame.
- Police pathfinding runs BFS (up to 90 tiles) inside a 256×256 tile window around the player.
- Buildings, trees, streetlights, and traffic lights are sorted by map row, so each frame only checks the visible screen band.
- The pause screen draws the full world map on the left (7 tiles per pixel) and the player's surroundings on the right (1:2 scale).

## microSD and GTA.PAK Graphics Pack

The microSD card does not expand flash and cannot execute code, but is used to load assets and store save files (`GTASAVE.DAT`) and BMP screenshots.

At startup, `GTA.PAK` (2.6 MB) is loaded from the card into PSRAM (~2 s), switching the renderer to detailed sprites:
- vehicles: 14 models × 32 rotation angles with highlights and shadows (including police, fire truck, `VORTEX GT`, and wrecks);
- pedestrians: 12 types × 16 directions × 4 walking frames;
- environment: 6 tree types, casino slot machine, logo.

**Setup:**
- Format the microSD card as **FAT32**.
- Copy `sd_card/WORLD.BIN` and `sd_card/GTA.PAK` to the **root** of the card. Without `GTA.PAK`, the game runs with procedural graphics; without `WORLD.BIN`, the game cannot start (`NO WORLD.BIN`).
- For the PC build, place both files next to `gta_pc.exe` (already included in `pc/`).

**Rebuilding the sprite pack** (requires Python 3 and Pillow):
```bash
python3 tools/make_pak.py GTA.PAK
```
A preview image is also generated at `GTA.PAK.preview.png`.