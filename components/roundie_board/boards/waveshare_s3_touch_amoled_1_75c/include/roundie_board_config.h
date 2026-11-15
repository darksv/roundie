#pragma once

/* Compile-time description of the Waveshare ESP32-S3-Touch-AMOLED-1.75C, read
 * by firmware outside the board component through roundie_board.h. Every board
 * defines the same macros; docs/board-support.md lists them. */

#define ROUNDIE_BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-1.75C"

/* ROUNDIE_BOARD_CAP_* bits from roundie_board.h. No RTC chip is fitted:
 * docs/hardware-audit-1.75c.md. */
#define ROUNDIE_BOARD_CAPABILITIES \
    (ROUNDIE_BOARD_CAP_TOUCH | ROUNDIE_BOARD_CAP_SPEAKER | ROUNDIE_BOARD_CAP_ROUND_DISPLAY | \
     ROUNDIE_BOARD_CAP_MICROPHONE | ROUNDIE_BOARD_CAP_IMU)

/* CO5300, 466 x 466 RGB565 AMOLED, round glass. The 360 x 360 UI canvas is
 * upscaled (docs/display-rendering.md, "Scaled panels"). */
#define ROUNDIE_BOARD_DISPLAY_WIDTH 466
#define ROUNDIE_BOARD_DISPLAY_HEIGHT 466
/* One draw carries at most 466 x 24 pixels; the QSPI transfer limit. */
#define ROUNDIE_BOARD_DISPLAY_STRIP_LINES 24
/* The panel takes RGB565 most significant byte first; LVGL stores CPU order. */
#define ROUNDIE_BOARD_DISPLAY_SWAP_RGB565 1
/* CO5300 windows start on an even and end on an odd column and row. */
#define ROUNDIE_BOARD_DISPLAY_ALIGN 2

/* Flash the firmware uses: the 16 MiB in the image header. The chip is 32 MiB
 * per the wiki and factory image but 16 MB per the schematic; with a 16 MiB
 * header ESP-IDF boots on either and reports 16 MiB
 * (docs/hardware-audit-1.75c.md). */
#define ROUNDIE_BOARD_FLASH_BYTES 0x1000000U
#define ROUNDIE_BOARD_PSRAM_BYTES 0x800000U
