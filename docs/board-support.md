# Board support (`roundie_board`)

**Status, 2026-11-15:** two boards implemented, the Waveshare
ESP32-S3-Touch-LCD-1.85B and ESP32-S3-Touch-AMOLED-1.75C. The boundary below is
an architectural decision of this refactor. Host tests and target builds of
both boards pass. The 1.85B device results are recorded at the end. The 1.75C
was tested on one board with an earlier port that shares this port's pins,
PMIC, microphone, IMU, wake and panel-restore code; this port's canvas
scaling, 40 MHz pixel clock and touch filter are not yet run on hardware
([hardware-audit-1.75c.md](hardware-audit-1.75c.md#device-results-2026-11-13-and-2026-11-14)).

## Boards

| | 1.85B (`waveshare_s3_touch_lcd_1_85b`) | 1.75C (`waveshare_s3_touch_amoled_1_75c`) |
|---|---|---|
| Panel | ST77916 360 × 360 LCD, QSPI, LEDC backlight | CO5300 466 × 466 AMOLED, QSPI, brightness by panel command |
| UI canvas | 360 × 360, direct | 360 × 360, upscaled per strip ([details](display-rendering.md#scaled-panels)) |
| Draw limit | 360 × 20 (or 60) pixels, align 1 | 466 × 24 pixels, align 2 |
| Touch | CST816S | CST9217 |
| RTC | PCF85063 (`ROUNDIE_BOARD_CAP_RTC`) | None; system clock and RTC timer |
| Battery | BQ27220 fuel gauge | AXP2101: presence, charge direction, voltage, gauge percent (ADC and detection enabled at start) |
| Microphone | None | ES7210, MIC1 and MIC2 (`ROUNDIE_BOARD_CAP_MICROPHONE`) |
| IMU | None | QMI8658 (`ROUNDIE_BOARD_CAP_IMU`) |
| Speaker | ES8311 | ES8311 |
| Wake | Touch INT, BOOT | Touch INT, BOOT |
| Flash, PSRAM | 16 MB, 8 MB Octal | 32 MB per wiki, 16 MB per schematic; 16 MiB image header, 8 MB Octal |
| Status | Hardware-verified (results below) | Board facts, PMIC, microphone, IMU, wake and panel restore verified with an earlier port; display scaling and touch filter pending |

## Boundary

`components/roundie_board` is the only firmware code that knows the board's
GPIOs, bus ports, I²C addresses and chips. Everything else uses
`include/roundie_board.h`:

| Area | API | Notes |
|---|---|---|
| Bring-up | `roundie_board_start`, `roundie_board_start_recovery`, `roundie_board_stop` | Probe results in `roundie_board_status_t` |
| Display | `roundie_board_display_set_draw_done`, `roundie_board_display_draw`, `roundie_board_display_power`, `roundie_board_display_restore`, `roundie_board_backlight_set` | GUI task only; the done callback runs in ISR context |
| Touch | `roundie_board_touch_read` | Polled by the LVGL input device |
| Speaker | `roundie_board_speaker_start`, `roundie_board_speaker_read_register` | Returns an `esp_codec_dev` output device, and an input device on a board with a microphone; `roundie_audio` owns both |
| RTC, battery, IMU | `roundie_board_rtc_read`, `roundie_board_rtc_set_trusted`, `roundie_board_battery_read`, `roundie_board_imu_read` | A missing chip is a sensor state, not an error path |
| Buttons, wake | `roundie_board_boot_pressed`, `roundie_board_wake_arm`, `roundie_board_wake_source_active`, `roundie_board_wake_disarm` | Light-sleep wake sources are the board's |

Calendar conversion (`rtc_time.c`) is board-independent and shared.

Not behind the boundary, on purpose:

- **The UI canvas** (`ROUNDIE_UI_WIDTH/HEIGHT`, 360 × 360, `roundie_app_ui.h`)
  is part of the application contract. `main.c` asserts at compile time that
  the board's panel matches it.
- **`esp_codec_dev`** is a standard Espressif API and appears in the public
  header as the speaker's device type. Writing a second audio abstraction
  would only wrap it.
- **The ELF loader, OTA, partitions and the launcher** do not depend on the
  board.

## Board configuration

Each board lives in `components/roundie_board/boards/<name>/`:

| File | Content |
|---|---|
| `board.cmake` | `set(board_srcs ...)`, sources relative to the directory |
| `include/roundie_board_config.h` | Public compile-time description (below) |
| `board_pins.h` | Private: GPIOs, ports, addresses. The only place they are named |
| `board.c` and helpers | Implementation of every function in `roundie_board.h` |
| `Kconfig` | Board-only options (sourced only when the board is selected) |
| `sdkconfig.defaults` | Target, Flash size, PSRAM, the board choice |

`roundie_board_config.h` defines:

- `ROUNDIE_BOARD_NAME`
- `ROUNDIE_BOARD_CAPABILITIES`
- `ROUNDIE_BOARD_DISPLAY_WIDTH`, `ROUNDIE_BOARD_DISPLAY_HEIGHT` (panel pixels; a
  panel larger than the 360 × 360 UI canvas shows it upscaled, see
  [display-rendering.md](display-rendering.md#scaled-panels))
- `ROUNDIE_BOARD_DISPLAY_STRIP_LINES` (full-width rows per draw; a draw holds at
  most width × strip lines pixels, the bus transfer limit)
- `ROUNDIE_BOARD_DISPLAY_SWAP_RGB565` (1 when the panel takes RGB565 MSB first)
- `ROUNDIE_BOARD_DISPLAY_ALIGN` (1, or 2 when the controller needs draw windows
  that start on an even and end on an odd column and row)
- `ROUNDIE_BOARD_FLASH_BYTES`, `ROUNDIE_BOARD_PSRAM_BYTES` (fitted memory; the
  OTA self-test refuses to confirm an image on a chip that reports other sizes)

### Capabilities

Compile-time bits in `ROUNDIE_BOARD_CAPABILITIES`. A bit exists only when
firmware code reads it:

| Bit | Read by | Effect when absent |
|---|---|---|
| `ROUNDIE_BOARD_CAP_TOUCH` | `main.c` | No LVGL pointer device |
| `ROUNDIE_BOARD_CAP_SPEAKER` | `main.c` | `roundie_audio_init` is not called; `rep_audio_v1` reports no audio |
| `ROUNDIE_BOARD_CAP_ROUND_DISPLAY` | `main.c` (static assert) | Build fails: the launcher and round UI need a round panel |
| `ROUNDIE_BOARD_CAP_RTC` | `main.c` | The system clock alone keeps time after an explicit sync ([details](rtc-time-sync.md#boards-without-an-external-rtc)) |
| `ROUNDIE_BOARD_CAP_MICROPHONE` | board (`speaker.microphone`) | `microphone` is NULL; the RA1 `MIC` diagnostic returns `ESP_ERR_NOT_SUPPORTED` |
| `ROUNDIE_BOARD_CAP_IMU` | `main.c` | No IMU log in the sensor worker; `roundie_board_imu_read` reports `ROUNDIE_SENSOR_UNAVAILABLE` |

The fuel gauge and wake sources have no bit. Their absence is reported
at run time (`ROUNDIE_SENSOR_*` states, `ESP_ERR_NOT_FOUND`,
`ESP_ERR_NOT_SUPPORTED`), and their callers already handle it.

## Selecting a board

The board is selected at build time in two places, which must agree:

- The CMake cache variable `ROUNDIE_BOARD` (default
  `waveshare_s3_touch_lcd_1_85b`, or `waveshare_s3_touch_amoled_1_75c`)
  prepends the board's `sdkconfig.defaults`.
- The Kconfig choice `ROUNDIE_BOARD` sets `CONFIG_ROUNDIE_BOARD_DIR`, and with
  it the compiled directory.

On a mismatch, `components/roundie_board/CMakeLists.txt` stops the build. Use a
separate build directory and sdkconfig for each board:

```bash
idf.py -B build-<board> -DROUNDIE_BOARD=<board> -DSDKCONFIG=sdkconfig.<board> build
```

## Adding a board

1. **Audit the hardware** (spec §23). Record the schematic, the pin map, bus
   addresses and the recovery path in a hardware audit like
   [hardware-audit.md](hardware-audit.md). Keep GPIO0 or the target's
   equivalent free for ROM Download.
2. **Copy the board directory.** Copy
   `boards/waveshare_s3_touch_lcd_1_85b/` to `boards/<name>/`. Rewrite
   `board_pins.h` and `roundie_board_config.h`.
3. **Implement the display.** Use the matching `esp_lcd` panel driver. Call the
   draw-done callback from the panel IO's color-transfer-done event. Implement
   `display_restore` as whatever the controller needs after display-off and
   light sleep. If a plain DISPON is enough, set `*repaint = false`.
4. **Implement the remaining functions.** Every function in `roundie_board.h`
   must exist. For hardware the board lacks, return `ESP_ERR_NOT_SUPPORTED` or
   the matching sensor state, and clear the capability bit.
5. **Register the board.**
   - Add a `config` to the `ROUNDIE_BOARD` choice in
     `components/roundie_board/Kconfig`, with `depends on IDF_TARGET_...`.
   - Add a `ROUNDIE_BOARD_DIR` default and an `rsource` of the board's
     `Kconfig`.
   - Add any managed driver to `components/roundie_board/idf_component.yml`.
   - Add any IDF component to the `PRIV_REQUIRES` in
     `components/roundie_board/CMakeLists.txt`.
6. **Write the board's `sdkconfig.defaults`.** Set the target, Flash size,
   PSRAM mode and the board choice.
7. **Validate.**
   - Run the host tests.
   - Build both profiles for the new board, and again for the existing board.
   - Run the device scenarios below on each board touched.

None of this should need changes to the launcher, OTA, the ELF loader,
`roundie_audio` or application logic. A change there means the boundary is
missing something. Record it as a decision instead of working around it.

## Limitations of the current boundary

- **Resolution.** The application canvas is 360 × 360 (ABI). A larger panel
  shows it upscaled; a smaller one fails the static assert. Native use of a
  larger panel would need an ABI version, not a board change.
- **CPU architecture.**
  - Applications are native Xtensa ELF objects, built and validated for the
    ESP32-S3 (see [elf-loader.md](elf-loader.md)).
  - Another ESP32-S3 board can run the same packages.
  - A RISC-V target (ESP32-C/P series) would need its own toolchain, loader
    validation and package architecture tag. Its board choice can depend on
    that target, but the app runtime is not validated there.
- **Flash and partitions.** The REP profile's partition table and
  `sdkconfig.rep.defaults` use the first 16 MiB. Every board's image header
  says 16 MiB. The 1.75C may carry a 32 MiB chip; ESP-IDF then uses the header
  size, and the rest is unused
  ([hardware-audit-1.75c.md](hardware-audit-1.75c.md#flash-size-conflicting-sources)).
  A board with less Flash needs its own layout, which is a
  storage decision (spec §25).
- **Display buffering.**
  - The strip buffers are internal DMA memory sized for an SPI or QSPI panel.
  - The `ROUNDIE_LCD_STRIP_LINES` and `ROUNDIE_LCD_DOUBLE_BUFFER` experiment
    options exist only for the 1.85B. Other boards fix their strip height in
    `roundie_board_config.h`.
  - An RGB or MIPI panel would need a different buffering strategy, a measured
    choice in its own right.
- **Requirements.** CMake `REQUIRES` cannot depend on Kconfig. The component
  requirements and `idf_component.yml` therefore list the union of every
  board's drivers, so each build downloads drivers it does not compile.
  Conditional manifest rules (`rules: if:`) have not been tried.
- **Speaker format.** The speaker is a 16-bit stereo I²S output with a
  software-volume path in `roundie_audio`. A board without a codec
  (PDM or I²S DAC only) would still need an `esp_codec_dev` data-only device.

## Hardware regression scenarios

Run these after any change to `roundie_board`, on firmware installed by OTA.
The 1.75C has its own first-run list in
[hardware-audit-1.75c.md](hardware-audit-1.75c.md#pending-device-tests).
Each scenario passes only with the stated evidence.

| # | Area | Procedure | Pass |
|---|---|---|---|
| H1 | Boot, I²C | Boot normally. Read the bring-up log and `tools/roundie_clock.py` | Touch, RTC and gauge ACK. RTC time is valid. Battery state is not `ERROR`. No I²C timeout in the log |
| H2 | Display | Launcher, open and close two apps, scroll a list | No tearing or stale strips. `transfer_errors` 0 in the GUI diagnostics |
| H3 | Touch | Tap buttons near the edge of the round safe area. Long-press | Each tap is registered once, at the correct position |
| H4 | Screen off and on | Wait for the screen-off timeout, then press BOOT | Log "Display re-initialised after screen off: ESP_OK". Full repaint, no blank panel |
| H5 | Light-sleep wake | Let the device enter light sleep. Wake it by touch and by BOOT | Log "Display re-initialised after wake: ESP_OK". Touch works at once. The RTC advanced |
| H6 | Audio init | `tools/roundie_audio.py status` | `initialized`, `step ok`, `codec_ack` 1, ID bytes as before the refactor |
| H7 | Audio tone and registers | `tone`, then `regs` | Tone audible (user). Register dump readable. No I²C error in touch or gauge during the tone |
| H8 | Audio stream | Run the Sound example app, play and stop; unload while playing | Tune plays. Stop is immediate. The unload log shows the stream released |
| H9 | Audio and sleep | Start the stream, wait past the screen timeout | Sleep refused while playing (power-manager log) |
| H10 | OTA | Install the next version with `tools/roundie_ota.py`, reboot | Confirmed after the self-test. State VALID |

H3, H5 (touch wake), H7 (listening) and H8 (listening) need a person at the
device.

## Device results, 2026-11-15 (firmware 0.3.50 and 0.3.51)

**Setup.** The board had been running foreign firmware (`esp32-authenticator`)
in `ota_0`, which otadata selected.
- The bootloader (`f7c33fe8…`) and the REP partition table (`a273fbb4…`) were
  unchanged. LittleFS and its six packages were intact.
- Before any write, the 64 KB prefix, the LittleFS head and the foreign
  `ota_0` image were saved under
  `../roundie-backups/restore-foreign-2026-11-15-1731/`.
- With the user's approval, esptool erased `ota_0` and otadata and wrote
  0.3.50 at 0x10000. The write was hash-verified.
- NVS, LittleFS, `ota_1`, the bootloader and the table were not written.

| # | Result |
|---|---|
| H1 | PASS, partial. The RTC read is valid; there were no touch or I²C errors in the UI and audio counters. The gauge was not read separately |
| H2 | PASS for counters: LCD `errors` 0, `flush_errors` 0. Visual inspection PENDING |
| H3 | PENDING (person) |
| H4 | PASS for the log: RW1 screen-on gave "Display re-initialised after screen off: ESP_OK". Visual repaint PENDING |
| H5 | Timer wake PASS: 10 s light sleep, 9,991 ms, no reset. Touch and BOOT wake (the restore path) PENDING (person) |
| H6 | PASS: `step ok`, `ack 1`, ID `83 11 01`, the same as gate A1 |
| H7 | PASS for the protocol: a 440 Hz tone completed with 0 errors, and the register dump read through the BSP. Audibility PENDING (person) |
| H8 | PASS for the protocol. The Sound app (launcher select and button press, both INJECTED) opened a stream and filled its ring; exiting the app while it played released the stream; 0 errors. A 5 s RA1 stream with a 600 ms stall gave 1 underrun and 0 errors. Audibility PENDING |
| H9 | PASS: sleep was refused (`refusal=app`) during the stream |
| H10 | PASS: 0.3.51 was installed by `roundie_ota.py` into `ota_1` and is VALID after the self-test. 0.3.50 remains in `ota_0` |

