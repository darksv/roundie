#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "roundie_rtc_time.h"

/* Board support boundary (docs/board-support.md). Firmware outside
 * components/roundie_board uses only this header: no GPIO numbers, bus ports,
 * device addresses or display-controller APIs. One implementation is compiled,
 * chosen by CONFIG_ROUNDIE_BOARD_*; its roundie_board_config.h defines:
 *   ROUNDIE_BOARD_NAME                  board name for logs
 *   ROUNDIE_BOARD_CAPABILITIES          ROUNDIE_BOARD_CAP_* bits below
 *   ROUNDIE_BOARD_DISPLAY_WIDTH/HEIGHT  panel pixels; larger than the UI canvas
 *                                       means the canvas is shown upscaled
 *   ROUNDIE_BOARD_DISPLAY_STRIP_LINES   full-width rows per draw, the bus transfer limit
 *   ROUNDIE_BOARD_DISPLAY_SWAP_RGB565   1 when the panel takes RGB565 MSB first
 *   ROUNDIE_BOARD_DISPLAY_ALIGN         1, or 2 when draw windows must start on an
 *                                       even and end on an odd column and row
 *   ROUNDIE_BOARD_FLASH_BYTES           fitted Flash size, checked by the OTA self-test
 *   ROUNDIE_BOARD_PSRAM_BYTES           fitted PSRAM size, checked by the OTA self-test
 * Every function below exists on every board; a missing device is reported at
 * run time (ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ROUNDIE_SENSOR_*). */

/* Hardware the firmware consults before using it. Add a bit only together with
 * the code that reads it. */
#define ROUNDIE_BOARD_CAP_TOUCH (1U << 0)         /* touch input for LVGL */
#define ROUNDIE_BOARD_CAP_SPEAKER (1U << 1)       /* roundie_board_speaker_start() */
#define ROUNDIE_BOARD_CAP_ROUND_DISPLAY (1U << 2) /* circular glass: the round UI needs it */
#define ROUNDIE_BOARD_CAP_RTC (1U << 3)           /* battery-backed RTC behind roundie_board_rtc_*() */
#define ROUNDIE_BOARD_CAP_MICROPHONE (1U << 4)    /* roundie_board_speaker_t.microphone */
#define ROUNDIE_BOARD_CAP_IMU (1U << 5)           /* roundie_board_imu_read() */

#include "roundie_board_config.h"

typedef struct roundie_board roundie_board_t;

typedef enum {
    ROUNDIE_SENSOR_VALID,
    ROUNDIE_SENSOR_NOT_FOUND,
    ROUNDIE_SENSOR_COMM_ERROR,
    ROUNDIE_SENSOR_INVALID,
    ROUNDIE_SENSOR_UNAVAILABLE,
    ROUNDIE_SENSOR_BATTERY_ABSENT,
    ROUNDIE_SENSOR_UNCALIBRATED,
} roundie_sensor_state_t;

typedef struct {
    roundie_sensor_state_t state;
    esp_err_t error;
    int64_t observed_us; /* esp_timer monotonic timestamp */
    bool oscillator_stopped;
    roundie_rtc_time_t time;
} roundie_rtc_sample_t;

typedef struct {
    roundie_sensor_state_t state;
    esp_err_t error;
    int64_t observed_us;
    bool battery_present;
    bool initialized;
    bool voltage_valid;
    bool soc_valid;
    bool current_valid;
    bool temperature_valid;
    bool remaining_valid;
    bool full_valid;
    uint16_t voltage_mv;
    uint16_t soc_percent;
    int16_t current_ma; /* positive = charging, negative = discharging */
    int32_t temperature_deci_c;
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t battery_flags;
    uint16_t operation_flags;
    bool discharging;
    bool sleeping;
} roundie_battery_sample_t;

typedef struct {
    esp_err_t touch;
    esp_err_t rtc;
    esp_err_t gauge;
    uint8_t lcd_variant;
} roundie_board_status_t;

/* The board owns all handles. A failed LCD or bus start returns an error and
 * releases resources. Optional I2C device failures remain in status. */
esp_err_t roundie_board_start(roundie_board_t **out_board,
                              roundie_board_status_t *status);
/* Recovery display only: same LCD ownership/cleanup, no I2C, touch, RTC or
 * gauge initialization. USB does not depend on success. */
esp_err_t roundie_board_start_recovery(roundie_board_t **out_board,
                                      roundie_board_status_t *status);
void roundie_board_stop(roundie_board_t *board);

/* Speaker output, for the audio task (docs/audio.md). The board wires the
 * codec, its data bus and the amplifier and hands over an esp_codec_dev output
 * device; the caller opens, writes, sets volume and closes it. 16-bit stereo
 * frames. Started once, after roundie_board_start(); not in the recovery
 * display start. failed_step names the step that failed, or "ok".
 * On a board with ROUNDIE_BOARD_CAP_MICROPHONE the same start also hands over
 * an esp_codec_dev input device that shares the speaker's data bus and clocks
 * (same sample rate, 16-bit stereo); it is NULL elsewhere or when the
 * microphone failed to start, which does not fail the speaker. */
typedef struct {
    esp_codec_dev_handle_t device;
    esp_codec_dev_handle_t microphone;
    bool codec_ack;         /* the codec answered on the control bus */
    uint8_t codec_id[3];    /* board-defined identity bytes, for diagnostics */
    const char *failed_step;
} roundie_board_speaker_t;
esp_err_t roundie_board_speaker_start(roundie_board_t *board, uint32_t sample_rate,
                                      roundie_board_speaker_t *speaker);
/* Development: one codec control register, independent of playback. */
esp_err_t roundie_board_speaker_read_register(roundie_board_t *board, uint8_t reg, uint8_t *value);

/* Display, GUI task only. The board owns the panel, its bus and its controller.
 * ISR context: the queued draw has finished with its pixel buffer. Returns true
 * when the callback woke a higher-priority task. */
typedef bool (*roundie_board_draw_done_t)(void *context);
/* Once, before the first draw. */
esp_err_t roundie_board_display_set_draw_done(roundie_board_t *board,
                                              roundie_board_draw_done_t callback,
                                              void *context);
/* Queues RGB565 pixels for columns x1..x2-1 and rows y1..y2-1, in the panel's
 * byte order (ROUNDIE_BOARD_DISPLAY_SWAP_RGB565), at most
 * ROUNDIE_BOARD_DISPLAY_WIDTH x ROUNDIE_BOARD_DISPLAY_STRIP_LINES pixels, from
 * internal DMA-capable memory. The buffer belongs to the transfer until the
 * draw-done callback, which runs once per draw. Up to two draws may be queued. */
esp_err_t roundie_board_display_draw(roundie_board_t *board, int x1, int y1,
                                     int x2, int y2, const void *pixels);

/* Only call from the GUI task. ESP_ERR_NOT_FOUND without working touch. */
esp_err_t roundie_board_touch_read(roundie_board_t *board, bool *pressed,
                                   uint16_t *x, uint16_t *y);
esp_err_t roundie_board_backlight_set(roundie_board_t *board, uint8_t percent);
/* GUI task only: panel display off/on; panel memory is kept where the
 * controller allows it. Backlight is separate. */
esp_err_t roundie_board_display_power(roundie_board_t *board, bool on);
/* GUI task only, no draw in flight: brings the panel back after it was off
 * (roundie_board_display_power(false) or light sleep) and switches it on. Sets
 * *repaint when panel memory was lost and the caller must redraw everything. */
esp_err_t roundie_board_display_restore(roundie_board_t *board, bool *repaint);
/* Normal start only. The 1.85B BOOT button is GPIO0, the ROM Download strap:
 * an input, never driven. False on boards without a button. */
bool roundie_board_boot_pressed(roundie_board_t *board);

/* Light-sleep wake on the board's user inputs (1.85B: touch INT and BOOT, both
 * low level). Only while the GUI task is parked and does not read touch.
 * Disarm restores touch interrupts and requests one touch read if a touch is
 * still reported. Arm returns ESP_ERR_NOT_FOUND when no wake input exists. */
bool roundie_board_wake_source_active(roundie_board_t *board);
esp_err_t roundie_board_wake_arm(roundie_board_t *board);
void roundie_board_wake_disarm(roundie_board_t *board);

/* Sensor calls use the board's shared master and bounded 100 ms transactions.
 * The sensor worker serializes RTC reads and sets after startup. Set verifies
 * immediate readback only; its caller must confirm progression before wall sync.
 * A failed probe is retried on later samples; no handle is permanently absent.
 * Without ROUNDIE_BOARD_CAP_RTC, read reports ROUNDIE_SENSOR_NOT_FOUND and set
 * returns ESP_ERR_NOT_SUPPORTED. */
void roundie_board_rtc_read(roundie_board_t *board, roundie_rtc_sample_t *sample);
esp_err_t roundie_board_rtc_set_trusted(roundie_board_t *board,
                                        const roundie_rtc_time_t *time);
void roundie_board_battery_read(roundie_board_t *board,
                                roundie_battery_sample_t *sample);

typedef struct {
    roundie_sensor_state_t state;
    esp_err_t error;
    float accel_mg[3]; /* x, y, z in the sensor's own axes */
    float gyro_dps[3];
    float temperature_c;
} roundie_imu_sample_t;

/* Inertial sensor, same bus rules as the sensors above. Without
 * ROUNDIE_BOARD_CAP_IMU, or when the sensor did not start, the state is
 * ROUNDIE_SENSOR_UNAVAILABLE. */
void roundie_board_imu_read(roundie_board_t *board, roundie_imu_sample_t *sample);
