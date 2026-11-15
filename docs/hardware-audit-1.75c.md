# Roundie hardware audit: ESP32-S3-Touch-AMOLED-1.75C

**Date:** 2026-11-15
**Scope:** source and schematic audit for the second board implementation
(`components/roundie_board/boards/waveshare_s3_touch_amoled_1_75c/`).
**Status:** hardware facts partly verified on one board; this port's own
display, touch and scaling code is still unverified on the board.
**Evidence level:** desktop inspection and target builds, plus device tests
on 2026-11-13 and 2026-11-14 with an earlier, separately written port of the
same board (see [Device results](#device-results-2026-11-13-and-2026-11-14)).
The board facts and the PMIC, microphone, IMU, light-sleep and panel-restore
code below were verified there. Anything that depends on code only this
port has (canvas scaling, 40 MHz pixel clock, touch filter) is still PENDING.

Terms as in [hardware-audit.md](hardware-audit.md): **documented fact** is shown
by a source listed at the end, at the stated revision; **hypothesis** is an
unverified working premise; **pending** names a device check and its pass
condition.

## Sources

| Id | Source | Revision / hash |
|---|---|---|
| S1 | Waveshare wiki page "ESP32-S3-Touch-AMOLED-1.75C" (Onboard Resources, Features) | Fetched 2026-11-15 |
| S2 | `ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf` from `waveshareteam/ESP32-S3-Touch-AMOLED-1.75C` | Repo commit `4bfdc80e` (2026-10-20); PDF SHA-256 `7bcc0a3a9dc02893741e1e555b203a17055b5cc9cce3b5c548e160e21683900d` |
| S3 | Waveshare BSP `waveshare/esp32_s3_touch_amoled_1_75c` 3.0.0 (Apache-2.0) | Upstream commit `010c5fb3`; archive SHA-256 `23542dfada54a5f7345026d4f09adbd07f517627f980ac4d6d1f3f75767e655e` |
| S4 | `espressif/esp_lcd_co5300` 2.2.0, `waveshare/esp_lcd_touch_cst9217` 2.0.0 | Pinned in `components/roundie_board/idf_component.yml` |
| S5 | Waveshare factory image `ESP32-S3-Touch-AMOLED-1.75C_FactoryOnly_20260114.bin` (33,488,896 bytes) | SHA-256 `46e65fb708f325f6830d6f14ecbba3d357dbdab6a939e13fdf2206e147c063d2`; same repo as S2 |

## Board facts

| Item | Documented fact | Source |
|---|---|---|
| SoC and memory | ESP32-S3R8 (8 MB stacked PSRAM). Flash: 32 MB per S1 and S5, but S2 names a 16 MB part (see below) | S1, S2, S5 |
| Display | 1.75" 466 × 466 AMOLED, CO5300 on QSPI: SCLK 38, D0–D3 4–7, CS 12, RST 1. TE on 13 is not used. Column gap 6. No backlight; brightness is DCS 0x51 | S3, S4 |
| Touch | CST9217 at I²C 0x5A, INT 11, RST 2. No pull-up on TP_INT on the main board (J3 pin 30 straight to GPIO11) | S2, S3 |
| I²C | One bus, SDA 15, SCL 14, with external 2.2 kΩ pull-ups (R23, R24): touch, ES8311 (0x18), ES7210 (0x40), AXP2101 (0x34), QMI8658 IMU (0x6B) | S2, S3 |
| Audio | ES8311 codec, I²S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable GPIO46. ES7210 microphone ADC on the same clocks, data in on GPIO10 | S1, S2, S3 |
| Power | AXP2101 PMIC, MX1.25 battery connector. The PWR button (Key2) pulls the AXP2101's PWRON low and also drives `SYS_OUT` on GPIO3 through a BSS138 inverter (T1, 10 kΩ pull-up R9), so GPIO3 should read high while the button is held | S1, S2 |
| Buttons | BOOT on GPIO0 (ROM Download strap) | S1, S2 |

The pins in `board_pins.h` were compared with S3's `esp32_s3_touch_amoled_1_75c.h`
and match. GPIO0 stays an input; no PSRAM or Flash pin is used.

S2's pin table puts `QMI_INT1` in the GPIO38 row (the LCD's QSPI clock), but
the net label beside the IMU ties `QMI_INT1` to GPIO21. The table row is
probably shifted. The firmware starts the IMU with the `waveshare/qmi8658`
defaults but never configures or reads GPIO21 or GPIO38 for it; whether those
defaults enable an interrupt output is not checked. Nothing may use QMI8658
INT1 until the wiring is checked on the board.

GPIO3 (`SYS_OUT`, the PWR button) is unused. Reading it is a possible later
feature and needs device test D12 first. GPIO3 is a strapping pin only for the
JTAG source eFuse option, which Roundie does not set.

## No external RTC (PCF85063)

**Conclusion:** the 1.75C has no external RTC chip. `ROUNDIE_BOARD_CAP_RTC` is
clear and the wall clock uses the RTC-timer path
([rtc-time-sync.md](rtc-time-sync.md#boards-without-an-external-rtc)).

Evidence:

- S1's Onboard Resources list names ESP32-S3R8, Flash, QMI8658, ES7210,
  ES8311, AXP2101, the buttons and connectors. It lists no RTC.
- S2 (page 1 is the circuit; pages 2 and 3 are the PCB top and bottom
  silkscreen) has exactly seven ICs, and none is an RTC:
  - U1 ESP32-S3R8, U2 XM25QH128DHIQT Flash, U3 QMI8658 IMU;
  - U6 ES8311 codec, U7 NS4150B amplifier, U8 ES7210 microphone ADC;
  - UP1 AXP2101 PMIC.

  The PCB silkscreen shows the same designators and no other IC footprint.
  The "RTC" strings in S2 are the AXP2101's own backup outputs (`RTCLDO`
  → test point `VCC-RTC`, `VBACKUP` → `VBAT2`) and the ESP32-S3's
  `VDD3P3_RTC` supply pin.
- S3 has no RTC driver, address or dependency.
- **No 32 kHz crystal.** In S2, U1 pins 22 and 23 (XTAL_32K_P/N) go to the
  nets GPIO15 and GPIO16. These carry I²C SDA and I²S MCLK (S3). The only crystal
  is X1, 40 MHz, on XTAL_P/N. The RTC slow clock is the internal RC (`CONFIG_RTC_CLK_SRC_INT_RC`).

One contrary hint: a comment in S3's public header says the I²C bus is shared
by "touch, audio, IMU, RTC, and power-management devices". Nothing else in S3
matches it, so it is probably copied from another Waveshare board.
**Pending:** an I²C scan on the device must show no ACK at 0x51.

## Flash size: conflicting sources

| Source | Says |
|---|---|
| S1 (wiki) | 32 MB NOR Flash |
| S2 (schematic) | U2 = XM25QH128DHIQT, 128 Mbit = 16 MB |
| S5 (factory image) | Image header size code 5 = 32 MB. The partition table runs to 0x1FF0000 (`storage` ends there), which only fits a 32 MB chip |

Firmware that needs 32 MB is shipped for this board, so a 32 MB chip on
production units is the likely case. The schematic's part number is probably
out of date. **Measured 2026-11-13** on one unit: `esptool flash-id` reads
manufacturer `0xC8`, device `0x4019`, 32 MB; an image with a 16 MB header
boots on it.

**Decision (revised 2026-11-15):** build the image with
`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and `ROUNDIE_BOARD_FLASH_BYTES` = 16 MiB on
either chip. The first decision, a 32 MB header, was withdrawn before any
flash.

Why. ESP-IDF v6.1 (`components/spi_flash/esp_flash_spi_init.c`,
`esp_flash_init_default_chip`) compares the detected chip with the header:
- A chip **smaller** than the header fails the Flash probe
  (`ESP_ERR_FLASH_SIZE_NOT_MATCH`), and the application does not start.
- A chip **larger** than the header only logs a warning. The header size is
  then used, and `esp_flash_get_size()` returns it.
- A header above 16 MB also needs the 32-bit address feature check and
  `CONFIG_BOOTLOADER_FLASH_32BIT_ADDR`.

Both partition layouts end at 16 MiB, so a 32 MiB header gained nothing. A 16
MiB header boots on both possible chips. On a 32 MB chip the OTA self-test's
size check still passes, because IDF reports the header size. The upper 16 MiB
stays unused and unreachable.

The pre-flash `flash-id` read-out is still required, to record the actual chip.

**Installation gap.** No installer supports this board yet:
- `tools/flash_rep.py` updates an existing REP layout. It refuses a chip whose
  ID does not say 16 MiB, and a bootloader or partition table other than
  Roundie's.
- `tools/flash_bringup.py` is tied to the 1.85B's backup.

Both refuse; neither writes. A first install on the 1.75C needs a reviewed
installer with a full read-out first. That is part of the first flash, not
of this port.

## AXP2101

**Decision:** the firmware writes only enable bits that its readings and the
codecs need, and never a voltage, charger or power-key setting. A wrong write
can switch off a rail that powers the SoC, Flash or panel. Power-off,
PWR-button handling and charger configuration stay out of scope.

At start the board sets, read-modify-write and only when clear:

- 0x30 bit 0, the battery voltage ADC. Without it 0x34..0x35 read 0.
- 0x68 bit 0, battery detection.
- 0x90 bit 0, ALDO1 (A3V3, the ES8311 and ES7210 analog supply), only when
  0x92 already says 3.3 V (code `0x1C`). Otherwise the rail is left alone
  and a warning is logged.

The register map was first taken from XPowersLib (`src/XPowersAXP2101.hpp`,
`src/REG/AXP2101Constants.h` at commit `17132a58`) and then compared with the
X-Powers AXP2101 datasheet V1.0 (English; the copy hosted by M5Stack, a
third-party mirror, so its revision is not independently confirmed).
`roundie_board_battery_read()` uses:

| Register | Meaning used | Datasheet |
|---|---|---|
| 0x00 bit 3 | Battery connected | Same; bit 5 is VBUS good |
| 0x01 bits 6..5 | 01 charging, 10 discharging | Same; bits 2..0 are the charge state |
| 0x34..0x35 | Battery voltage in mV, read as one burst, 13 bits | 14-bit field, 1 mV per LSB, high byte first; the 8.192 V full scale needs 13 |
| 0xA4 | Fuel-gauge state of charge, percent (> 100 is rejected) | Same; the gauge is enabled by 0x18 bit 3 (default 1) |
| 0x90 bit 0 | ALDO1 enable | The table labels both bit 4 and bit 0 "aldo1 enable"; bits 0..3 are ALDO1..4 in order, so bit 4 is most likely BLDO1. Ambiguous |

Not in the datasheet: the chip-ID register 0x03 (reads `0x4A` on the board,
as XPowersLib expects). The datasheet recommends programming the battery
model (0xA1) for gauge accuracy; that is not done, so the percent is the
PMIC's estimate on its defaults. 0x10 bit 0 is a documented soft power-off
bit; it is not used.

There is no current, temperature or capacity. With a valid voltage and percent
the state is `ROUNDIE_SENSOR_VALID`, otherwise `ROUNDIE_SENSOR_UNCALIBRATED`.

## Implementation decisions and assumptions

- **Display.**
  - Init sequence from S3, except that brightness starts at 0, so the panel
    is never shown at full brightness before the firmware sets it. The
    board stores the last brightness and applies it again after every
    re-initialization.
  - 40 MHz pixel clock as in S3 (assumption, not measured).
  - The 360 × 360 canvas is upscaled per strip
    ([display-rendering.md](display-rendering.md#scaled-panels)). Draws are at
    most 466 × 24 pixels, with two queued.
- **Panel restore.** Unlike the 1.85B, no reset: screen-off is brightness 0
  plus DISPOFF, the panel stays powered and keeps its frame memory, so restore
  is DISPON with `*repaint = false`. A reset and re-initialization showed
  undefined panel memory until the repaint reached it: a green band at the
  bottom, painted last, was seen on wake. Without the reset there was no band
  after screen-off, and the screen came back after touch and BOOT wakes from
  light sleep (device test).
- **Recovery start.** It starts no I²C, so the panel relies on the AXP2101
  power-on defaults (assumption).
- **Touch.**
  - Mirrored on both axes, with `x_max`/`y_max` 466 as in S3 (assumption).
    The driver mirrors as `466 - x`, so a raw 0 arrives as 466. The board
    clamps it to 465, and so does the canvas mapping.
  - The CST9217 driver returns `ESP_ERR_INVALID_RESPONSE` for a report without
    the 0xAB acknowledge byte, and keeps its previous data. S3 reads through
    the LVGL adapter, which then reports the old point again, so a held touch
    stays held.
  - Roundie does the same, but bounded: up to 6 consecutive unacknowledged
    reads (about 200 ms at LVGL's 33 ms read period) keep the last point;
    then the touch is released. An unacknowledged read never starts a touch
    and never triggers the 5 s back-off.
  - Earlier this port released the touch at once, which could make a held
    finger chatter. The limit is an assumption (`board_logic.h`).
  - INT gets the internal pull-up, because the main board has none and the
    controller's output type is unknown.
- **Flash.**
  - `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` on either chip, the same as the 1.85B.
    There is no 32-bit addressing in the bootloader or the application.
  - Both partition layouts end at 16 MiB.
  - The OTA self-test expects `esp_flash_get_size()` to report 16 MiB.
- **Microphone.** The ES7210 runs as an I²S slave on the shared clocks, MCLK
  from the pad, MIC1 and MIC2 as one stereo pair, 30 dB input gain (from vendor
  examples). I²S0 is opened full duplex for it; the board hands the input
  device to the audio component (`ROUNDIE_BOARD_CAP_MICROPHONE`).
- **IMU.** QMI8658 through `waveshare/qmi8658` 2.0.2: ±8 g, ±512 dps,
  1000 Hz, read in mg and dps (`ROUNDIE_BOARD_CAP_IMU`). No calibration and
  no interrupt. The driver adds its own I²C device before it checks the chip,
  so the board removes it after a failed start. Its header defines `M_PI`, so
  `board.c` does not include `<math.h>`.
- **Unused hardware.** The PWR button (also on GPIO3), TE and the IMU
  interrupts are unused.

## Verified so far (host and build only)

- Target builds pass with 0 warnings: 1.75C legacy and REP; 1.85B legacy and
  REP.
- The 1.85B sdkconfig and partition table are identical to the build before
  this port. Its loadable ELF sections are the same size; only debug info
  differs. Its bootloader differs only in the compile time and checksum.
- The 1.75C image and bootloader headers say 16 MB at 80 MHz, without 32-bit
  addressing. The offline REP validator accepts both boards' REP builds.
- Host tests: the touch filter and the AXP2101 decoding (`board_logic.c`),
  the canvas touch mapping, the scaled flush path, and resets and power loss
  without an RTC. A short mutation check broke each new rule in turn, and the
  tests failed every time except once. That one change (counting
  unacknowledged reads while idle) has no visible effect.
- The scaled flush path and its host harness:
  [display-rendering.md](display-rendering.md#scaled-panels).

## Device results (2026-11-13 and 2026-11-14)

**Firmware:** an earlier port of this board in the `darksv/roundie` fork,
written separately from this one, REP profile, version 0.3.49 and later
builds; the owner authorized each flash. What it shared with this port: the
pins, the CO5300 vendor init sequence with column gap 6, the CST9217 driver,
the 16 MB image header, the same light-sleep wake scheme, and the PMIC,
microphone, IMU and panel-restore code now in `board.c`. What it did not
share: it drew applications at the native 466 × 466 with the 360 × 360 layout
centred instead of scaling the canvas, used a 20 MHz pixel clock instead of
40 MHz, and had its own touch release handling. Results for those parts do
not carry over.

Before the first write, the unit's whole 32 MiB Flash was read out and kept
off the repository with its SHA-256. Writes were `esptool write-flash` of the
bootloader, partition table, otadata and application, without erase.

"Owner" marks an observation by the project owner at the board, not a
measurement.

| Area | Result |
|---|---|
| Flash, PSRAM | 32 MB chip (see "Flash size"); the 16 MB header boots. Octal PSRAM, 8 MB, 80 MHz, test passed. OTA self-test confirmed the slot |
| I²C | ACK at 0x5A (CST9217), 0x34 (AXP2101), 0x18 (ES8311, ID `83 11 01`), 0x40 (ES7210), 0x6B (QMI8658, register 0x00 reads `0x05`). No ACK at 0x41..0x43 or 0x6A. 0x51 was not probed |
| Display | Image visible and centred (owner), 20 MHz. Colour and byte order, even alignment and the gap of 6 were not checked in detail |
| Touch | The CST9217 reports a point reflection of the image (left x 7, right x 445, top y 18, bottom y 449); mirroring both axes makes touch match the drawing (owner). This port mirrors both axes as well |
| Screen off/on, light sleep | With the no-reset restore: no green band on wake (owner). A touch and a BOOT press each woke the board from light sleep (GPIO wake cause). Sleep current not measured |
| Battery | Without a cell: absent. With a cell: present, 4064..4196 mV, gauge 81 % at first read (accuracy unverified). Status history across a USB unplug: `0x3833` (VBUS good, charging, constant voltage), then `0x1855` (no VBUS, discharging, not charging), then `0x3833` again, as the datasheet encodes. Not measured: voltage and percent against a meter, charge current |
| ALDO1 | Reads 3300 mV (code `0x1C`) and was enabled; both codecs work on it. The A3V3 rail itself was not measured |
| Audio out | Sound heard through the ES8311 and the amplifier with PA on GPIO46 driven high (owner). Level and quality not measured |
| Microphone | RA1 `MIC <ms>`, 30 dB, 16 kHz stereo. Quiet, 3 s: ch0 peak 487 RMS 89, ch1 peak 498 RMS 94. Owner speaking or clapping near the board, 5 s: peaks 32767 (clipped), RMS 1542 and 1785. No read errors. Every open and close logs two `i2s_channel_disable: the channel has not been enabled yet` errors from the unused transmit channel; not investigated. Which microphone is which channel, and MIC3/MIC4, are not known |
| IMU | Flat, screen up: x -25, y -53, z 908 mg; screen down: z -1081 mg; on the image's bottom edge: x 997 mg; on its left edge: y -1056 mg. So +z points out of the screen, +x to the top of the image, +y to its left (right-handed); z offset about -86 mg. Gyroscope at rest -1..-3 dps on x and y (uncalibrated), die 27..28 °C |
| ROM Download | USB unplugged, board off by a long PWR press, BOOT held while USB was plugged in: `esptool chip-id --before no-reset` reached the ROM loader over the native USB port (same VID/PID as in normal operation). `--after watchdog-reset` then started the application normally. Not checked: this path while USB storage mode owns the port, or after a hung application |
| Storage, apps | The factory layout left unrelated data in `storage`; with separate authorization it was replaced by a host-built LittleFS image, which then mounted. Native ELF examples installed and ran from PSRAM |
| Endurance | Not done |

## Pending device tests

What the results above already show is noted in "Status". Everything else is
still pending on this port's firmware.

**Before the next flash of this port:**

- Explicit approval to flash.
- An installer for this board (see "Installation gap"). The unit tested above
  already holds a REP layout and a backup of its factory Flash.

| # | Area | Pass condition | Status |
|---|---|---|---|
| D1 | Boot | 8 MB PSRAM detected. With a 32 MB chip, the "larger than the header" warning; `esp_flash_get_size()` 16 MiB. The OTA self-test confirms (REP) | PSRAM, header and self-test passed with the earlier port; warning and `esp_flash_get_size()` not recorded |
| D2 | I²C scan | ACK at 0x5A, 0x34, 0x18, 0x40, 0x6B. No ACK at 0x51 (no RTC) | Passed except 0x51, which was not probed |
| D3 | Display | Full canvas shown, centred, no edge columns missing (gap 6). No tearing or stale strips. `transfer_errors` 0 | Pending: scaling and 40 MHz untested |
| D4 | Scaling cost | `scale_us`, `chunks` and DMA busy time recorded in the PERF trace for a full frame and for a small area | Pending |
| D5 | Touch | Taps at the four edges of the safe area land where touched (confirms mirroring). A 3 s press and a slow drag log one press and one release. Count the unacknowledged reads during a hold (temporary log) to check the limit of 6 | Mirroring confirmed with the earlier port; the filter is pending |
| D6 | Brightness | 30/60/100 % steps visible. 0 % on screen-off. Brightness kept after the screen is restored | Pending |
| D7 | Screen off/on, light sleep | Panel restored without a visible band. Wake latency measured. Touch and BOOT wake work | Passed with the earlier port; latency not measured |
| D8 | Battery | With a battery: present = 1, plausible voltage (3.0–4.3 V), charge direction follows USB plug/unplug. Without one: `BATTERY_ABSENT` | Passed with the same PMIC code |
| D9 | Audio | `tools/roundie_audio.py status`: `step ok`, `codec_ack` 1. Tone audible (person). `MIC 3000` RMS rises with sound | Sound and microphone passed with the earlier port |
| D10 | Wall clock | The tests in [rtc-time-sync.md](rtc-time-sync.md#boards-without-an-external-rtc) | Pending |
| D11 | ROM Download | Hold BOOT, reset: the ROM serial loader is reachable over USB | Passed (power-on with BOOT held); USB storage mode pending |
| D12 | Schematic details | GPIO3 high only while PWR is held. GPIO11 idles high with the pull-up. The QMI8658 answers at 0x6B, and GPIO38 shows no activity from it | 0x6B answers; the rest pending |
| D13 | IMU | Axes and gravity as in the results above | Passed with the same IMU code |
