#include <inttypes.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_newlib.h"
#include "esp_psram.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "roundie_board.h"
#include "time_protocol.h"
#include "clock_diagnostic.h"
#include "clock_retention.h"
#include "display_scale.h"
#include "storage_diagnostics.h"
#include "ota_diagnostics.h"
#include "recovery_diagnostics.h"
#include "app_runtime.h"
#include "elf_diagnostics.h"
#include "package_diagnostics.h"
#include "gui_diagnostics.h"
#include "roundie_launcher.h"
#include "roundie_app_ui.h"
#include "power_manager.h"
#include "roundie_storage_mode.h"
#include "storage_mode_screen.h"
#include "storage_import.h"
#include "audio_diagnostics.h"
#include "roundie_audio.h"
#include "local_time.h"
#if CONFIG_ROUNDIE_DEV_TIME_SYNC || CONFIG_ROUNDIE_RECOVERY_USB
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

_Static_assert(sizeof(time_t) == 8, "RTC requires ESP-IDF 64-bit time_t");

#define DISPLAY_WIDTH ROUNDIE_BOARD_DISPLAY_WIDTH
#define DISPLAY_HEIGHT ROUNDIE_BOARD_DISPLAY_HEIGHT
#define DISPLAY_STRIP_LINES ROUNDIE_BOARD_DISPLAY_STRIP_LINES
/* The UI canvas is part of the application contract. A larger panel shows it
 * upscaled (display_scale.h, docs/display-rendering.md); a panel of the same
 * size keeps the direct path. */
#define CANVAS_WIDTH ROUNDIE_UI_WIDTH
#define CANVAS_HEIGHT ROUNDIE_UI_HEIGHT
#define DISPLAY_SCALED (DISPLAY_WIDTH != CANVAS_WIDTH || DISPLAY_HEIGHT != CANVAS_HEIGHT)
_Static_assert(DISPLAY_WIDTH >= CANVAS_WIDTH && DISPLAY_HEIGHT >= CANVAS_HEIGHT,
               "the board's panel must not be smaller than the UI canvas");
#if DISPLAY_SCALED
_Static_assert(ROUNDIE_BOARD_DISPLAY_ALIGN == 1 || ROUNDIE_BOARD_DISPLAY_ALIGN == 2,
               "the scaler aligns panel windows to 1 or 2 pixels");
_Static_assert(DISPLAY_WIDTH % ROUNDIE_BOARD_DISPLAY_ALIGN == 0 &&
               DISPLAY_HEIGHT % ROUNDIE_BOARD_DISPLAY_ALIGN == 0,
               "the panel edges must be aligned");
#else
_Static_assert(ROUNDIE_BOARD_DISPLAY_ALIGN == 1, "the direct path sends LVGL areas unrounded");
#endif
_Static_assert((ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_ROUND_DISPLAY) != 0,
               "the launcher and round UI need a round panel");
#define GUI_DELAY_MAX_MS 10U
#define HOME_PACKAGE "org.roundie.watch" /* see docs/system-navigation.md */

static const char *TAG = "roundie";

typedef struct {
    roundie_board_t *board;
    lv_display_t *display;
    lv_obj_t *touch_label;
    lv_obj_t *brightness_label;
    lv_obj_t *i2c_label;
    lv_obj_t *rtc_label;
    lv_obj_t *battery_label;
    lv_obj_t *battery_detail_label;
    lv_obj_t *ota_label;
    TaskHandle_t gui_task;
    bool transfer_pending;
    bool display_fault;
    unsigned transfer_errors;
    unsigned completed_transfers;
    int64_t transfer_started_us;
    portMUX_TYPE dma_lock;
    int64_t dma_done_us;
    uint32_t dma_completions;
    roundie_gui_work_t work;
    int64_t render_started_us;
    int64_t refresh_started_us;
    uint64_t iteration_id;
    uint64_t transfer_iteration_id;
    uint64_t transfer_id;
    bool was_pressed;
    uint16_t last_x;
    uint16_t last_y;
    bool touch_error_logged;
    int64_t touch_retry_after_us;
    uint8_t brightness_percent;
    bool touch_available;
#if DISPLAY_SCALED
    /* GUI task only. */
    roundie_scale_carry_t carry;
    uint16_t *scale_output[2]; /* panel chunks; chunk n uses scale_output[n % 2] */
    uint16_t *scale_carry_pixels;
    uint32_t collected_completions;
    uint64_t collected_busy_us;
    /* Under dma_lock: written by the GUI task before a draw, read by the ISR. */
    int64_t chunk_submitted_us[2];
    /* Under dma_lock: written by the ISR. */
    uint64_t dma_busy_us;
    uint32_t dma_busy_max_us;
#endif
} gui_state_t;

typedef struct {
    portMUX_TYPE lock;
    roundie_board_t *board;
    roundie_rtc_sample_t rtc;
    roundie_battery_sample_t battery;
    bool wall_valid;
    bool sync_failed; /* A failed explicit SET requires another successful SET. */
    bool board_ready;
} sensor_state_t;

static sensor_state_t sensors = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* Startup owns this until board publication, then only system_worker writes. */
static uint32_t wall_sync_count;
static int64_t wall_sync_at_us;

/* Wall time is valid only after a trusted sync: from the external RTC, or on a
 * board without one from the explicit roundie_time SET, which a software reset
 * keeps (clock_retention.h). Validity never depends on the RTC being present. */
#define BOARD_HAS_RTC ((ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_RTC) != 0)

/* Local time source: wall time is valid only after a trusted sync. */
static bool wall_clock_valid(uint32_t *sync_count)
{
    portENTER_CRITICAL(&sensors.lock);
    bool valid = sensors.wall_valid && sensors.board_ready;
    portEXIT_CRITICAL(&sensors.lock);
    *sync_count = wall_sync_count;
    return valid;
}

static const char *sensor_state_name(roundie_sensor_state_t state)
{
    switch (state) {
    case ROUNDIE_SENSOR_VALID: return "valid";
    case ROUNDIE_SENSOR_NOT_FOUND: return "not found";
    case ROUNDIE_SENSOR_COMM_ERROR: return "I2C error";
    case ROUNDIE_SENSOR_INVALID: return "invalid data";
    case ROUNDIE_SENSOR_UNAVAILABLE: return "unavailable";
    case ROUNDIE_SENSOR_BATTERY_ABSENT: return "battery absent";
    case ROUNDIE_SENSOR_UNCALIBRATED: return "gauge unverified";
    }
    return "unknown";
}

static const char *bus_state_name(roundie_sensor_state_t state)
{
    if (state == ROUNDIE_SENSOR_NOT_FOUND) {
        return "--";
    }
    if (state == ROUNDIE_SENSOR_COMM_ERROR) {
        return "ERR";
    }
    return "OK";
}

static bool set_wall_clock(int64_t seconds)
{
    struct timeval now = {
        .tv_sec = (time_t)seconds,
        .tv_usec = 0,
    };
    esp_err_t error = settimeofday(&now, NULL) == 0 ? ESP_OK : ESP_FAIL;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "System wall clock set failed: %s", esp_err_to_name(error));
        return false;
    }
    wall_sync_count++;
    wall_sync_at_us = esp_timer_get_time();
    return true;
}

static bool sync_wall_clock(const roundie_rtc_sample_t *sample)
{
    int64_t seconds;
    if (sample->state != ROUNDIE_SENSOR_VALID || sample->oscillator_stopped ||
        !roundie_rtc_unix_seconds(&sample->time, &seconds)) {
        return false;
    }
    if (!set_wall_clock(seconds)) {
        return false;
    }
    ESP_LOGI(TAG, "System wall clock synchronized from valid RTC (UTC)");
    return true;
}

#if !BOARD_HAS_RTC
/* Kept by a software reset in RTC slow memory; arbitrary after power-on. */
static RTC_NOINIT_ATTR roundie_clock_retention_t retained_clock;

static int64_t wall_now_us(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

/* Align ESP-IDF's RTC-domain boot time with the running clock, so that time
 * read back after a reset matches the time before it. system_worker only. */
static void align_retained_wall_clock(void)
{
    esp_sync_timekeeping_timers();
}

/* After a trusted SET. ESP-IDF itself keeps the time; this marks it trusted. */
static void retain_wall_clock(void)
{
    align_retained_wall_clock();
    roundie_clock_retention_record(&retained_clock, wall_now_us(), (int64_t)esp_rtc_get_time_us());
}

static bool reset_keeps_rtc_domain(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        /* Power-on, brownout, power glitch, external pin and unknown resets
         * may have lost the RTC domain: time stays invalid until a sync. */
        return false;
    }
}

/* Startup only. Never restores a stored time: it accepts the system time that
 * ESP-IDF kept in the RTC domain, or invalidates it. */
static bool accept_retained_wall_clock(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool accepted = reset_keeps_rtc_domain(reason) &&
        roundie_clock_retention_accept(&retained_clock, wall_now_us(),
                                       (int64_t)esp_rtc_get_time_us());
    if (!accepted) {
        roundie_clock_retention_clear(&retained_clock);
        ESP_LOGW(TAG, "System wall clock unsynchronized after reset reason %d; no external RTC",
                 (int)reason);
        return false;
    }
    retain_wall_clock();
    wall_sync_count++;
    wall_sync_at_us = esp_timer_get_time();
    ESP_LOGI(TAG, "System wall clock kept across reset reason %d (UTC)", (int)reason);
    return true;
}
#endif

#if CONFIG_ROUNDIE_DEV_TIME_SYNC
static void clock_diagnostic(sensor_state_t *shared, const char *id)
{
    portENTER_CRITICAL(&shared->lock);
    roundie_board_t *board = shared->board_ready ? shared->board : NULL;
    bool wall_valid = shared->wall_valid;
    /* Startup may still be writing these before board publication. Never
     * read its 64-bit state until the publication lock transfers ownership. */
    uint32_t sync_count = shared->board_ready ? wall_sync_count : 0;
    int64_t sync_at_us = shared->board_ready ? wall_sync_at_us : 0;
    portEXIT_CRITICAL(&shared->lock);
    struct timeval before = {0};
    struct timeval after = {0};
    roundie_clock_sample_t sample = {.wall_valid = wall_valid};
    roundie_rtc_sample_t rtc;
    sample.mono_before_us = esp_timer_get_time();
    int before_status = gettimeofday(&before, NULL);
    roundie_board_rtc_read(board, &rtc);
    int after_status = gettimeofday(&after, NULL);
    sample.mono_after_us = esp_timer_get_time();
    sample.wall_before_us = (int64_t)before.tv_sec * 1000000 + before.tv_usec;
    sample.wall_after_us = (int64_t)after.tv_sec * 1000000 + after.tv_usec;
    sample.rtc_seconds = -1;
    bool decoded = roundie_rtc_unix_seconds(&rtc.time, &sample.rtc_seconds);
    sample.rtc_valid = decoded && rtc.state == ROUNDIE_SENSOR_VALID;
    sample.oscillator_stopped = rtc.oscillator_stopped;
    sample.read_ok = rtc.error == ESP_OK && before_status == 0 && after_status == 0;
    roundie_clock_offset_t offset = roundie_clock_offset(&sample);
    int os = (rtc.state == ROUNDIE_SENSOR_VALID || rtc.state == ROUNDIE_SENSOR_INVALID) ?
             rtc.oscillator_stopped : -1;
    char response[512];
    int length = snprintf(response, sizeof(response),
        "\n!RT1 %s DIAG state=%d os=%d rtc=%" PRId64 " err=%d valid=%d"
        " wb=%" PRId64 " wa=%" PRId64 " mb=%" PRId64 " ma=%" PRId64
        " usable=%d lo=%" PRId64 " hi=%" PRId64 " sync=%" PRIu32 " at=%" PRId64 "\n",
        id, (int)rtc.state, os, sample.rtc_seconds, (int)rtc.error, wall_valid,
        sample.wall_before_us, sample.wall_after_us, sample.mono_before_us,
        sample.mono_after_us, offset.usable, offset.lower_us, offset.upper_us,
        sync_count, sync_at_us);
    if (length > 0 && (size_t)length < sizeof(response)) {
        if (usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100)) != length) {
            ESP_LOGW(TAG, "Clock diagnostic transport write failed");
        }
    }
}

static void time_command(sensor_state_t *shared,
                          const roundie_time_frame_t *frame)
{
    roundie_time_command_t command;
    const char *result = roundie_time_parse(frame, &command);
    if (strcmp(result, "OK") == 0 && command.diagnostic) {
        /* Strictly read-only: bypass GET's legacy recovery synchronization,
         * cached status publication, SET, NVS and restart paths. */
        clock_diagnostic(shared, command.id);
        return;
    }
    if (strcmp(result, "OK") == 0 && command.zone) {
        /* TZ [rule]: set when a rule is given, then report the active rule. */
        if (command.zone_rule[0] != '\0' && roundie_local_time_set_zone(command.zone_rule) != ESP_OK) {
            result = "ZONE";
        }
        char active[ROUNDIE_TIME_ZONE_CAPACITY];
        roundie_local_time_zone(active);
        char reply[96];
        int length = snprintf(reply, sizeof(reply), "\n!RT1 %s %s zone=%s\n", command.id, result, active);
        if (length > 0 && (size_t)length < sizeof(reply)) {
            usb_serial_jtag_write_bytes(reply, (size_t)length, pdMS_TO_TICKS(100));
        }
        return;
    }
    if (command.restart && !roundie_ota_restart_allowed()) {
        result = "OTA_BUSY";
    }
    roundie_rtc_sample_t rtc;
    esp_err_t error = ESP_OK;
    portENTER_CRITICAL(&shared->lock);
    bool wall_valid = shared->wall_valid;
    roundie_board_t *board = shared->board_ready ? shared->board : NULL;
    portEXIT_CRITICAL(&shared->lock);
#if !BOARD_HAS_RTC
    if (strcmp(result, "OK") == 0 && command.set) {
        wall_valid = false;
        shared->sync_failed = true;
        portENTER_CRITICAL(&shared->lock);
        shared->wall_valid = false;
        portEXIT_CRITICAL(&shared->lock);
        roundie_clock_retention_clear(&retained_clock);
        wall_valid = set_wall_clock(command.seconds);
        if (!wall_valid) {
            result = "CLOCK";
            error = ESP_FAIL;
        } else {
            retain_wall_clock();
            shared->sync_failed = false;
            ESP_LOGI(TAG, "System wall clock set by explicit time sync (UTC)");
        }
    }
#endif
    if (BOARD_HAS_RTC && strcmp(result, "OK") == 0 && command.set) {
        wall_valid = false;
        shared->sync_failed = true;
        portENTER_CRITICAL(&shared->lock);
        shared->wall_valid = false;
        portEXIT_CRITICAL(&shared->lock);
        roundie_rtc_time_t calendar;
        roundie_rtc_from_unix(command.seconds, &calendar);
        error = roundie_board_rtc_set_trusted(board, &calendar);
        if (error != ESP_OK) {
            result = "RTC_SET";
        } else {
            roundie_rtc_sample_t first;
            roundie_board_rtc_read(board, &first);
            if (first.state != ROUNDIE_SENSOR_VALID) {
                result = "RTC_READ";
                error = first.error;
            } else {
                int64_t first_seconds;
                int64_t next_seconds;
                roundie_rtc_unix_seconds(&first.time, &first_seconds);
                /* Yield only the sensor worker. GUI and touch continue. */
                vTaskDelay(pdMS_TO_TICKS(1200));
                roundie_board_rtc_read(board, &rtc);
                if (rtc.state != ROUNDIE_SENSOR_VALID) {
                    result = rtc.oscillator_stopped ? "RTC_INVALID" : "RTC_READ";
                    error = rtc.error;
                } else if (!roundie_rtc_unix_seconds(&rtc.time, &next_seconds) ||
                           !roundie_rtc_progress_matches(first_seconds, next_seconds,
                                                       rtc.observed_us - first.observed_us)) {
                    result = "RTC_VERIFY";
                    error = ESP_ERR_INVALID_RESPONSE;
                } else {
                    wall_valid = sync_wall_clock(&rtc);
                    if (!wall_valid) {
                        result = "CLOCK";
                        error = ESP_FAIL;
                    } else {
                        shared->sync_failed = false;
                    }
                }
            }
        }
    }
    /* Fresh status, also on failure. Never substitute a cached valid sample. */
    roundie_board_rtc_read(board, &rtc);
    if (BOARD_HAS_RTC && rtc.state != ROUNDIE_SENSOR_VALID) {
        wall_valid = false;
        if (strcmp(result, "OK") == 0) {
            result = rtc.state == ROUNDIE_SENSOR_INVALID ? "RTC_INVALID" : "RTC_READ";
            error = rtc.error;
        }
    } else if (BOARD_HAS_RTC && strcmp(result, "OK") == 0 && !wall_valid && !shared->sync_failed) {
        wall_valid = sync_wall_clock(&rtc);
        if (!wall_valid) {
            result = "CLOCK";
            error = ESP_FAIL;
        }
    }
    if (command.set && strcmp(result, "OK") != 0) {
        shared->sync_failed = true;
        wall_valid = false;
#if !BOARD_HAS_RTC
        roundie_clock_retention_clear(&retained_clock);
#endif
    }
    portENTER_CRITICAL(&shared->lock);
    shared->rtc = rtc;
    shared->wall_valid = wall_valid;
    portEXIT_CRITICAL(&shared->lock);
    int64_t rtc_seconds = -1;
    if (rtc.state == ROUNDIE_SENSOR_VALID) {
        roundie_rtc_unix_seconds(&rtc.time, &rtc_seconds);
    }
    int os = (rtc.state == ROUNDIE_SENSOR_VALID || rtc.state == ROUNDIE_SENSOR_INVALID) ?
             rtc.oscillator_stopped : -1;
    char response[192];
    int length = snprintf(response, sizeof(response),
                          "\n!RT1 %s %s state=%d os=%d rtc=%" PRId64
                          " wall=%" PRId64 " err=%d\n",
                          command.id, result, (int)rtc.state, os, rtc_seconds,
                          wall_valid ? (int64_t)time(NULL) : INT64_C(-1), (int)error);
    if (length > 0 && (size_t)length < sizeof(response)) {
        /* Enqueue one contiguous record under the driver's TX mutex. Logs use
         * this same driver, so they cannot be inserted inside the response.
         * Bulk TX also avoids one USB packet per byte in the pinned VFS. */
        if (usb_serial_jtag_write_bytes(response, (size_t)length,
                                         pdMS_TO_TICKS(100)) != length) {
            ESP_LOGW(TAG, "Time response transport write failed");
        }
    }
    if (command.restart && strcmp(result, "OK") == 0) {
        /* Dedicated retention test, not a general command interpreter. */
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(100));
        roundie_boot_planned_restart();
        esp_restart();
    }
}
#endif

#if CONFIG_ROUNDIE_DEV_TIME_SYNC || CONFIG_ROUNDIE_RECOVERY_USB
static void poll_time_commands(int serial_fd, sensor_state_t *shared,
                                roundie_time_frame_t *frame, int64_t *started_us)
{
    int64_t now = esp_timer_get_time();
    if (frame->used > 0 && now - *started_us > 2000000) {
        frame->discard = true; /* Never execute a suffix of an expired frame. */
    }
    char bytes[256];
    ssize_t count = read(serial_fd, bytes, sizeof(bytes));
    for (ssize_t i = 0; i < count; i++) {
        if (frame->used == 0) {
            *started_us = now;
        }
        if (roundie_time_feed(frame, bytes[i])) {
            if (frame->used > 0 || frame->discard) {
#if CONFIG_ROUNDIE_RECOVERY_USB
                if (strncmp(frame->line, "!RS1 ", 5) == 0) {
                    roundie_recovery_command(frame->line, frame->discard);
                } else
#endif
#if CONFIG_ROUNDIE_DEV_POWER
                if (strncmp(frame->line, "!RW1 ", 5) == 0) {
                    roundie_power_command(frame->line, frame->discard);
                } else
#endif
#if CONFIG_ROUNDIE_DEV_OTA
                if (strncmp(frame->line, "!RO1 ", 5) == 0) {
                    roundie_ota_command(frame->line, frame->discard);
                } else
#endif
#if CONFIG_ROUNDIE_DEV_AUDIO
                if (strncmp(frame->line, "!RA1 ", 5) == 0) {
                    roundie_audio_command(frame->line, frame->discard);
                } else
#endif
#if CONFIG_ROUNDIE_DEV_ELF
                if (strncmp(frame->line, "!RP1 ", 5) == 0) {
                    roundie_package_command(frame->line, frame->discard);
                } else
                if (strncmp(frame->line, "!RE1 ", 5) == 0) {
                    roundie_elf_command(frame->line, frame->discard);
                } else
#endif
                {
#if CONFIG_ROUNDIE_DEV_TIME_SYNC
                    time_command(shared, frame);
#else
                    (void)shared;
#endif
                }
            }
            *frame = (roundie_time_frame_t){0};
        }
    }
}
#endif

#define BOARD_HAS_IMU ((ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_IMU) != 0)

#if BOARD_HAS_IMU
/* Bring-up diagnostic: one IMU line at the first valid sample and whenever an
 * acceleration axis has moved by IMU_LOG_STEP_MG since the last line, so tilting
 * the board shows up in the log without a periodic flood. */
#define IMU_LOG_STEP_MG 250

static void log_imu_if_moved(const roundie_imu_sample_t *imu)
{
    static bool logged_once;
    static bool logged_error;
    static float last_mg[3];
    if (imu->state != ROUNDIE_SENSOR_VALID) {
        if (!logged_error && imu->state != ROUNDIE_SENSOR_UNAVAILABLE) {
            ESP_LOGW(TAG, "IMU read: %s", esp_err_to_name(imu->error));
            logged_error = true;
        }
        return;
    }
    logged_error = false;
    bool moved = !logged_once;
    for (int axis = 0; axis < 3; axis++) {
        float delta = imu->accel_mg[axis] - last_mg[axis];
        if (delta >= IMU_LOG_STEP_MG || delta <= -IMU_LOG_STEP_MG) {
            moved = true;
        }
    }
    if (!moved) {
        return;
    }
    ESP_LOGI(TAG, "IMU accel x=%d y=%d z=%d mg, gyro x=%d y=%d z=%d (0.1 dps), temp %d (0.1 C)",
             (int)imu->accel_mg[0], (int)imu->accel_mg[1], (int)imu->accel_mg[2],
             (int)(imu->gyro_dps[0] * 10.0F), (int)(imu->gyro_dps[1] * 10.0F),
             (int)(imu->gyro_dps[2] * 10.0F), (int)(imu->temperature_c * 10.0F));
    for (int axis = 0; axis < 3; axis++) {
        last_mg[axis] = imu->accel_mg[axis];
    }
    logged_once = true;
}
#endif

static void sensor_worker(void *argument)
{
    sensor_state_t *shared = argument;
    roundie_sensor_state_t last_rtc = shared->rtc.state;
    roundie_sensor_state_t last_battery = shared->battery.state;
    bool first_battery = true;
    uint16_t last_voltage = 0;
    uint16_t last_soc = 0;
#if CONFIG_ROUNDIE_DEV_TIME_SYNC || CONFIG_ROUNDIE_RECOVERY_USB
    /* Secondary VFS RX requires its standard interrupt driver. One RX owner,
     * no extra task or per-command transport allocation. */
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_config.tx_buffer_size = 512; /* One complete uncertainty diagnostic. */
#if CONFIG_ROUNDIE_DEV_OTA || CONFIG_ROUNDIE_RECOVERY_USB
    /* The pinned ISR drops bytes if its RX ring is full, and TX cannot enqueue
     * a record larger than its ring. Hold one whole RO1 request/response;
     * the host uses stop-and-wait, never pipelines while Flash work runs. */
    usb_config.rx_buffer_size = 2048;
    usb_config.tx_buffer_size = 2048;
#endif
    /* In storage mode the internal PHY belongs to USB-OTG: no USB Serial/JTAG
     * driver, no RT1/RO1/RS1/RP1 service. A restart brings both back. */
    const bool storage_mode = roundie_storage_mode_active();
    esp_err_t usb_error = storage_mode ? ESP_OK : usb_serial_jtag_driver_install(&usb_config);
    int serial_fd = -1;
    if (storage_mode) {
        ESP_LOGI(TAG, "USB Serial/JTAG services off in storage mode");
    } else if (usb_error == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
        serial_fd = open("/dev/secondary", O_RDWR | O_NONBLOCK);
    } else {
        ESP_LOGE(TAG, "Development RTC USB driver: %s", esp_err_to_name(usb_error));
    }
    roundie_time_frame_t frame = {0};
    int64_t started_us = 0;
    ESP_LOGI(TAG, "Development RTC protocol RT1 %s",
             serial_fd >= 0 ? "enabled on USB Serial/JTAG" : "unavailable");
    if (serial_fd < 0 && !storage_mode) {
        roundie_boot_enter_safe(ROUNDIE_SAFE_CORE);
        roundie_ota_gui_health(false);
        ESP_LOGE(TAG, "USB recovery unavailable; no new-image confirmation, ROM recovery required");
    }
#endif
    /* This worker alone serves USB recovery, OTA and packages. A stall must
     * reset into BootGuard, not silently remove those paths. */
    esp_err_t watchdog_error = esp_task_wdt_add(NULL);
    if (watchdog_error != ESP_OK) {
        ESP_LOGE(TAG, "System worker watchdog: %s", esp_err_to_name(watchdog_error));
    }
#if CONFIG_ROUNDIE_APP_RUNTIME
    /* Home is the watch face (docs/system-navigation.md). The one normal trial
     * after Safe Mode starts on the list, not in a package that may have failed. */
    roundie_boot_status_t boot_status;
    roundie_boot_status(&boot_status);
    roundie_launcher_set_home(HOME_PACKAGE, !boot_status.trial);
#endif
    int64_t next_sample_us = esp_timer_get_time() + 5000000;
    for (;;) {
        if (watchdog_error == ESP_OK) {
            esp_task_wdt_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        roundie_ota_poll(esp_timer_get_time());
        roundie_ota_status_t ota_health;
        roundie_ota_snapshot(&ota_health);
        if (ota_health.boot_checked && !ota_health.boot_passed && !roundie_recovery_active()) {
            roundie_boot_enter_safe(ROUNDIE_SAFE_GUI);
        }
        roundie_recovery_poll(esp_timer_get_time());
#if CONFIG_ROUNDIE_APP_RUNTIME
        roundie_app_runtime_poll(esp_timer_get_time());
        roundie_storage_import_poll(esp_timer_get_time());
        roundie_launcher_poll(roundie_app_runtime_allowed(), roundie_app_runtime_upload_busy());
#endif
#if CONFIG_ROUNDIE_POWER
        /* May sleep this worker; it owns every Flash writer, so none is
         * in progress. Returns after wake with the watchdog restarted. */
        roundie_power_worker_poll(esp_timer_get_time());
#endif
#if CONFIG_ROUNDIE_DEV_TIME_SYNC || CONFIG_ROUNDIE_RECOVERY_USB
        if (serial_fd >= 0) {
            poll_time_commands(serial_fd, shared, &frame, &started_us);
        }
#endif
        if (esp_timer_get_time() < next_sample_us) {
            continue;
        }
        next_sample_us = esp_timer_get_time() + 5000000;
        portENTER_CRITICAL(&shared->lock);
        bool ready = shared->board_ready;
        portEXIT_CRITICAL(&shared->lock);
        if (!ready || roundie_recovery_active()) {
            continue;
        }
        roundie_rtc_sample_t rtc;
        roundie_battery_sample_t battery;
        roundie_board_rtc_read(shared->board, &rtc);
        roundie_board_battery_read(shared->board, &battery);
        if (rtc.state == ROUNDIE_SENSOR_VALID && !shared->wall_valid &&
            !shared->sync_failed) {
            bool synchronized = sync_wall_clock(&rtc);
            portENTER_CRITICAL(&shared->lock);
            shared->wall_valid = synchronized;
            portEXIT_CRITICAL(&shared->lock);
        } else if (BOARD_HAS_RTC && rtc.state != ROUNDIE_SENSOR_VALID && shared->wall_valid) {
            portENTER_CRITICAL(&shared->lock);
            shared->wall_valid = false;
            portEXIT_CRITICAL(&shared->lock);
            ESP_LOGW(TAG, "System wall clock marked unsynchronized after RTC failure");
        }
#if !BOARD_HAS_RTC
        if (shared->wall_valid) {
            align_retained_wall_clock();
        }
#endif
        if (rtc.state != last_rtc) {
            ESP_LOGI(TAG, "RTC %s, OS=%u, error=%s",
                     sensor_state_name(rtc.state), rtc.oscillator_stopped,
                     esp_err_to_name(rtc.error));
            last_rtc = rtc.state;
        }
        if (battery.state != last_battery) {
            ESP_LOGI(TAG, "Gauge %s, present=%u, initialized=%u, error=%s",
                     sensor_state_name(battery.state), battery.battery_present,
                     battery.initialized, esp_err_to_name(battery.error));
            last_battery = battery.state;
        }
        if (battery.voltage_valid &&
            (first_battery || abs((int)battery.voltage_mv - last_voltage) >= 50 ||
             (battery.soc_valid && abs((int)battery.soc_percent - last_soc) >= 5))) {
            if (battery.soc_valid) {
                ESP_LOGI(TAG, "Battery %u mV, gauge SoC %u%% (accuracy unverified), flags=0x%04x, op=0x%04x",
                         battery.voltage_mv, battery.soc_percent,
                         battery.battery_flags, battery.operation_flags);
            } else {
                ESP_LOGI(TAG, "Battery %u mV, gauge SoC unavailable, flags=0x%04x, op=0x%04x",
                         battery.voltage_mv, battery.battery_flags,
                         battery.operation_flags);
            }
            last_voltage = battery.voltage_mv;
            last_soc = battery.soc_percent;
            first_battery = false;
        }
        portENTER_CRITICAL(&shared->lock);
        shared->rtc = rtc;
        shared->battery = battery;
        portEXIT_CRITICAL(&shared->lock);
#if BOARD_HAS_IMU
        roundie_imu_sample_t imu;
        roundie_board_imu_read(shared->board, &imu);
        log_imu_if_moved(&imu);
#endif
    }
}

static void update_sensor_labels(gui_state_t *state)
{
    roundie_ota_status_t ota;
    roundie_ota_snapshot(&ota);
    if (ota.initialized) {
        lv_label_set_text_fmt(state->ota_label, "%s %s %s (%d)",
                              ota.running.label, roundie_ota_image_state_name(ota.running.state),
                              roundie_ota_phase_name(ota.phase), (int)ota.last_error);
    } else {
        lv_label_set_text(state->ota_label, "OTA unavailable in this layout");
    }
    roundie_rtc_sample_t rtc;
    roundie_battery_sample_t battery;
    bool wall_valid;
    portENTER_CRITICAL(&sensors.lock);
    rtc = sensors.rtc;
    battery = sensors.battery;
    wall_valid = sensors.wall_valid;
    portEXIT_CRITICAL(&sensors.lock);

    lv_label_set_text_fmt(state->i2c_label, "I2C  Touch %s  RTC %s  Gauge %s",
                          state->touch_available ? "OK" : "--",
                          bus_state_name(rtc.state), bus_state_name(battery.state));

    if ((rtc.state == ROUNDIE_SENSOR_VALID || !BOARD_HAS_RTC) && wall_valid) {
        time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        lv_label_set_text_fmt(state->rtc_label,
                              "Synced UTC %04d-%02d-%02d %02d:%02d:%02d",
                              utc.tm_year + 1900, utc.tm_mon + 1,
                              utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else if (rtc.state == ROUNDIE_SENSOR_INVALID && rtc.oscillator_stopped) {
        lv_label_set_text(state->rtc_label, "RTC invalid: oscillator stopped");
    } else {
        lv_label_set_text_fmt(state->rtc_label, "Time unsynced: %s",
                              sensor_state_name(rtc.state));
    }
    if (battery.voltage_valid) {
        if (battery.soc_valid) {
            lv_label_set_text_fmt(state->battery_label, "Gauge SoC %u%%  %u mV",
                                  battery.soc_percent, battery.voltage_mv);
        } else {
            lv_label_set_text_fmt(state->battery_label,
                                  "Battery %u mV  SoC unverified",
                                  battery.voltage_mv);
        }
    } else {
        lv_label_set_text_fmt(state->battery_label, "Battery: %s",
                              sensor_state_name(battery.state));
    }
    if (battery.battery_present && battery.error == ESP_OK) {
        lv_label_set_text_fmt(state->battery_detail_label, "%s%s  %s",
                              battery.discharging ? "Discharging" : "Charge/idle",
                              battery.sleeping ? " (sleep)" : "",
                              battery.initialized ? "estimate" : "gauge unverified");
    } else {
        lv_label_set_text(state->battery_detail_label, "");
    }
}

static uint32_t lv_tick_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if !DISPLAY_SCALED
static bool lcd_transfer_done(void *user_context)
{
    gui_state_t *state = user_context;
    int64_t done_us = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&state->dma_lock);
    state->dma_done_us = done_us;
    state->dma_completions++;
    portEXIT_CRITICAL_ISR(&state->dma_lock);
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(state->gui_task, &higher_priority_woken);
    /* IDF v6.1's SPI panel IO discards the color callback's bool return, and
     * the board passes it through.
     * Request the deferred ISR-exit switch here, otherwise a blocked GUI may
     * wait until the next 100 Hz tick for every strip. DMA already completed;
     * the existing GUI wait callback still controls buffer reuse. */
    portYIELD_FROM_ISR(higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

static void wait_for_display(lv_display_t *display)
{
    gui_state_t *state = lv_display_get_user_data(display);
    int64_t started = esp_timer_get_time();
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0) {
        portENTER_CRITICAL(&state->dma_lock);
        int64_t done_us = state->dma_done_us;
        portEXIT_CRITICAL(&state->dma_lock);
        uint32_t dma_us = (uint32_t)(done_us - state->transfer_started_us);
        uint32_t wait_us = (uint32_t)(esp_timer_get_time() - started);
        state->work.done++;
        state->work.last_done_id = state->transfer_id;
        state->work.carry_done += state->transfer_iteration_id != state->iteration_id;
        state->work.dma_us += dma_us;
        state->work.wait_us += wait_us;
        if (dma_us > state->work.dma_max_us) {
            state->work.dma_max_us = dma_us;
        }
        if (wait_us > state->work.wait_max_us) {
            state->work.wait_max_us = wait_us;
        }
        state->transfer_pending = false;
        state->completed_transfers++;
        return;
    }
    state->transfer_errors++;
    ESP_LOGE(TAG, "LCD transfer completion timeout (%u)",
             state->transfer_errors);
    /* LVGL would otherwise reuse a buffer that SPI DMA may still own. */
    esp_restart();
    abort();
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
    gui_state_t *state = lv_display_get_user_data(display);
    if (state->display_fault) {
        lv_display_flush_ready(display);
        return;
    }
    if (state->transfer_pending) {
        state->transfer_errors++;
        ESP_LOGE(TAG, "LCD flush requested while previous transfer is pending");
        esp_restart();
        abort();
    }
    state->transfer_pending = true;
    state->transfer_started_us = esp_timer_get_time();
    state->transfer_iteration_id = state->iteration_id;
    state->transfer_id++;
    state->work.last_submit_id = state->transfer_id;
    state->work.flushes++;
    state->work.pixels += (uint32_t)lv_area_get_size(area);
#if ROUNDIE_BOARD_DISPLAY_SWAP_RGB565
    /* LVGL stores RGB565 in CPU byte order; the panel expects MSB first. */
    lv_draw_sw_rgb565_swap(pixels, lv_area_get_size(area));
#endif
    int64_t driver_started_us = esp_timer_get_time();
    uint32_t swap_us = (uint32_t)(driver_started_us - state->transfer_started_us);
    state->work.swap_us += swap_us;
    if (swap_us > state->work.submit_swap_max_us) {
        state->work.submit_swap_max_us = swap_us;
    }
    esp_err_t error = roundie_board_display_draw(state->board,
                                                 area->x1, area->y1,
                                                 area->x2 + 1, area->y2 + 1,
                                                 pixels);
    int64_t submitted_us = esp_timer_get_time();
    uint32_t driver_us = (uint32_t)(submitted_us - driver_started_us);
    state->work.driver_us += driver_us;
    if (driver_us > state->work.submit_driver_max_us) {
        state->work.submit_driver_max_us = driver_us;
    }
    uint32_t submit_us = (uint32_t)(submitted_us - state->transfer_started_us);
    state->work.submit_us += submit_us;
    if (submit_us > state->work.submit_max_us) {
        state->work.submit_max_us = submit_us;
    }
    if (error != ESP_OK) {
        state->transfer_pending = false;
        state->display_fault = true;
        state->transfer_errors++;
        ESP_LOGE(TAG, "LCD transfer enqueue failed (%u): %s",
                 state->transfer_errors, esp_err_to_name(error));
        lv_display_flush_ready(display);
    }
}
#else
static const roundie_scale_geometry_t scale_geometry = {
    .logical_width = CANVAS_WIDTH,
    .logical_height = CANVAS_HEIGHT,
    .physical_width = DISPLAY_WIDTH,
    .physical_height = DISPLAY_HEIGHT,
    .align = ROUNDIE_BOARD_DISPLAY_ALIGN,
};
static uint16_t scale_columns[DISPLAY_WIDTH];

static bool lcd_chunk_done(void *user_context)
{
    gui_state_t *state = user_context;
    int64_t done_us = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&state->dma_lock);
    /* Chunks complete in submission order. A chunk queued behind another one
     * starts on the bus when the previous one is done. */
    int64_t started_us = state->chunk_submitted_us[(state->dma_completions + 1U) % 2U];
    if (started_us < state->dma_done_us) {
        started_us = state->dma_done_us;
    }
    uint32_t busy_us = (uint32_t)(done_us - started_us);
    state->dma_busy_us += busy_us;
    if (busy_us > state->dma_busy_max_us) {
        state->dma_busy_max_us = busy_us;
    }
    state->dma_done_us = done_us;
    state->dma_completions++;
    portEXIT_CRITICAL_ISR(&state->dma_lock);
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(state->gui_task, &higher_priority_woken);
    /* Same deferred ISR-exit switch as the direct path. */
    portYIELD_FROM_ISR(higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

/* GUI task: moves finished-chunk counters into this iteration's work. */
static void collect_chunks(gui_state_t *state)
{
    portENTER_CRITICAL(&state->dma_lock);
    uint32_t completions = state->dma_completions;
    uint64_t busy_us = state->dma_busy_us;
    uint32_t busy_max_us = state->dma_busy_max_us;
    state->dma_busy_max_us = 0;
    portEXIT_CRITICAL(&state->dma_lock);
    uint32_t finished = completions - state->collected_completions;
    state->collected_completions = completions;
    state->work.done += finished;
    state->work.last_done_id = completions;
    state->work.dma_us += busy_us - state->collected_busy_us;
    state->collected_busy_us = busy_us;
    if (busy_max_us > state->work.dma_max_us) {
        state->work.dma_max_us = busy_max_us;
    }
    state->completed_transfers += finished;
}

/* Blocks the GUI task until chunk number target has finished. */
static void wait_for_chunks(gui_state_t *state, uint32_t target)
{
    int64_t started_us = esp_timer_get_time();
    while (true) {
        portENTER_CRITICAL(&state->dma_lock);
        uint32_t completions = state->dma_completions;
        portEXIT_CRITICAL(&state->dma_lock);
        if ((int32_t)(completions - target) >= 0) {
            break;
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
            state->transfer_errors++;
            ESP_LOGE(TAG, "LCD transfer completion timeout (%u)", state->transfer_errors);
            /* The scaler would otherwise reuse a buffer that DMA may still own. */
            esp_restart();
            abort();
        }
    }
    uint32_t wait_us = (uint32_t)(esp_timer_get_time() - started_us);
    state->work.wait_us += wait_us;
    if (wait_us > state->work.wait_max_us) {
        state->work.wait_max_us = wait_us;
    }
    collect_chunks(state);
}

/* After the last strip of a refresh, LVGL calls this before rendering again. */
static void wait_for_scaled_display(lv_display_t *display)
{
    gui_state_t *state = lv_display_get_user_data(display);
    wait_for_chunks(state, (uint32_t)state->transfer_id);
}

/* Scales each LVGL strip once into a free panel chunk and queues it. The
 * strip buffer is free on return, so LVGL renders the next strip while the
 * chunk transfers; only the last strip of a refresh keeps flushing set. */
static void flush_scaled(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    gui_state_t *state = lv_display_get_user_data(display);
    if (state->display_fault) {
        state->carry.active = false;
        lv_display_flush_ready(display);
        return;
    }
    int64_t started_us = esp_timer_get_time();
    state->work.flushes++;
    state->work.pixels += (uint32_t)lv_area_get_size(area);
    roundie_scale_plan_t plan;
    if (!roundie_scale_plan(&scale_geometry, &state->carry, area->x1, area->y1,
                            area->x2, area->y2, &plan)) {
        state->carry.active = false;
        state->transfer_errors++;
        ESP_LOGE(TAG, "Scaled flush (%" PRId32 ",%" PRId32 ")-(%" PRId32 ",%" PRId32
                 ") breaks panel alignment (%u)", area->x1, area->y1, area->x2, area->y2,
                 state->transfer_errors);
        lv_display_flush_ready(display);
        return;
    }
    bool send = plan.send_y2 >= plan.send_y1;
    uint32_t chunk = (uint32_t)state->transfer_id + 1U;
    uint16_t *output = NULL;
    if (send) {
        /* The buffer was last used by chunk - 2. */
        wait_for_chunks(state, chunk - 2U);
        output = state->scale_output[chunk % 2U];
    }
    int64_t scale_started_us = esp_timer_get_time();
    roundie_scale_strip(&scale_geometry, &plan, scale_columns, (const uint16_t *)pixels,
                        output, state->scale_carry_pixels, ROUNDIE_BOARD_DISPLAY_SWAP_RGB565 != 0);
    roundie_scale_update_carry(&plan, &state->carry);
    int64_t scaled_us = esp_timer_get_time();
    uint32_t scale_us = (uint32_t)(scaled_us - scale_started_us);
    state->work.scale_us += scale_us;
    if (send) {
        uint32_t width = (uint32_t)(plan.x2 - plan.x1 + 1);
        uint32_t rows = (uint32_t)(plan.send_y2 - plan.send_y1 + 1);
        portENTER_CRITICAL(&state->dma_lock);
        state->chunk_submitted_us[chunk % 2U] = scaled_us;
        portEXIT_CRITICAL(&state->dma_lock);
        esp_err_t error = roundie_board_display_draw(state->board, plan.x1, plan.send_y1,
                                                     plan.x2 + 1, plan.send_y2 + 1, output);
        int64_t submitted_us = esp_timer_get_time();
        uint32_t driver_us = (uint32_t)(submitted_us - scaled_us);
        state->work.driver_us += driver_us;
        if (driver_us > state->work.submit_driver_max_us) {
            state->work.submit_driver_max_us = driver_us;
        }
        if (error != ESP_OK) {
            state->carry.active = false;
            state->display_fault = true;
            state->transfer_errors++;
            ESP_LOGE(TAG, "LCD transfer enqueue failed (%u): %s",
                     state->transfer_errors, esp_err_to_name(error));
            lv_display_flush_ready(display);
            return;
        }
        state->transfer_id = chunk;
        state->work.last_submit_id = chunk;
        state->work.chunks++;
        state->work.scaled_pixels += (uint64_t)width * rows;
    }
    uint32_t submit_us = (uint32_t)(esp_timer_get_time() - started_us);
    state->work.submit_us += submit_us;
    if (submit_us > state->work.submit_max_us) {
        state->work.submit_max_us = submit_us;
    }
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
    }
}
#endif

static void display_event(lv_event_t *event)
{
    gui_state_t *state = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_INVALIDATE_AREA) {
#if DISPLAY_SCALED
        /* Rounder: every area's panel rectangle starts and ends aligned. */
        lv_area_t *rounded = lv_event_get_invalidated_area(event);
        roundie_scale_round(&scale_geometry, &rounded->x1, &rounded->y1, &rounded->x2, &rounded->y2);
#endif
        const lv_area_t *area = lv_event_get_param(event);
        uint32_t pixels = (uint32_t)lv_area_get_size(area);
        state->work.invalidations++;
        state->work.invalid_pixels += pixels;
        state->work.full_invalidations += pixels == CANVAS_WIDTH * CANVAS_HEIGHT;
    } else if (code == LV_EVENT_REFR_START) {
        state->refresh_started_us = esp_timer_get_time();
    } else if (code == LV_EVENT_RENDER_START) {
        state->render_started_us = esp_timer_get_time();
        state->work.layout_us += (uint64_t)(state->render_started_us - state->refresh_started_us);
    } else if (code == LV_EVENT_RENDER_READY) {
        state->work.render_us += (uint64_t)(esp_timer_get_time() - state->render_started_us);
    }
}

static void read_touch(lv_indev_t *device, lv_indev_data_t *data)
{
    gui_state_t *state = lv_indev_get_user_data(device);
    bool pressed = false;
    uint16_t x = state->last_x;
    uint16_t y = state->last_y;
    if (esp_timer_get_time() < state->touch_retry_after_us) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = x;
        data->point.y = y;
        return;
    }
    esp_err_t error = roundie_board_touch_read(state->board, &pressed, &x, &y);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        if (!state->touch_error_logged) {
            ESP_LOGE(TAG, "Touch read failed: %s", esp_err_to_name(error));
            state->touch_error_logged = true;
        }
        state->touch_retry_after_us = esp_timer_get_time() + 5000000;
    } else if (error == ESP_OK) {
        state->touch_error_logged = false;
    }
#if DISPLAY_SCALED
    /* The board reports panel coordinates; LVGL works on the canvas. */
    if (pressed) {
        x = (uint16_t)roundie_scale_touch(x, CANVAS_WIDTH, DISPLAY_WIDTH);
        y = (uint16_t)roundie_scale_touch(y, CANVAS_HEIGHT, DISPLAY_HEIGHT);
    }
#endif
#if CONFIG_ROUNDIE_POWER
    /* Touches that turn the screen on, or arrive while it is off, never
     * reach applications or the launcher. */
    if (!roundie_power_gui_touch(esp_timer_get_time(), pressed)) {
        pressed = false;
    }
#endif
    if (pressed) {
        state->last_x = x;
        state->last_y = y;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = state->last_x;
    data->point.y = state->last_y;
    if (pressed != state->was_pressed) {
        ESP_LOGI(TAG, "Touch %s at (%u,%u)", pressed ? "press" : "release",
                 state->last_x, state->last_y);
        state->was_pressed = pressed;
        if (pressed) {
            lv_label_set_text_fmt(state->touch_label, "Touch %u, %u", x, y);
        } else {
            lv_label_set_text(state->touch_label, "Touch released");
        }
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 270);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void close_color_field(lv_event_t *event)
{
    lv_obj_delete(lv_event_get_target_obj(event));
}

/* 30, 60, 100, 30 ... percent; GUI task. */
static void step_backlight(gui_state_t *state)
{
    uint8_t next_percent = 30;
    if (state->brightness_percent < 60) {
        next_percent = 60;
    } else if (state->brightness_percent < 100) {
        next_percent = 100;
    }
    esp_err_t error = roundie_board_backlight_set(state->board, next_percent);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Backlight change: %s", esp_err_to_name(error));
        return;
    }
    state->brightness_percent = next_percent;
    lv_label_set_text_fmt(state->brightness_label, "Backlight %u%%",
                          next_percent);
}

static void cycle_backlight(lv_event_t *event)
{
    step_backlight(lv_event_get_user_data(event));
}

#if CONFIG_ROUNDIE_APP_RUNTIME && CONFIG_ROUNDIE_POWER
/* Quick settings on the home application (docs/quick-settings.md). */
static gui_state_t *quick_state;

static bool quick_battery(uint8_t *percent, bool *charging)
{
    portENTER_CRITICAL(&sensors.lock);
    roundie_battery_sample_t battery = sensors.battery;
    portEXIT_CRITICAL(&sensors.lock);
    if (battery.state != ROUNDIE_SENSOR_VALID || !battery.battery_present || !battery.soc_valid ||
        battery.soc_percent > 100) {
        return false;
    }
    *percent = (uint8_t)battery.soc_percent;
    *charging = battery.current_valid && battery.current_ma > 0;
    return true;
}

static uint8_t quick_brightness(bool cycle)
{
    if (cycle) {
        step_backlight(quick_state);
    }
    return quick_state->brightness_percent;
}

static const roundie_launcher_quick_t quick_settings = {
    quick_battery, quick_brightness, roundie_power_gui_screen_off, roundie_power_gui_sleep,
};
#endif

static void show_color_field(lv_event_t *event)
{
    const uint32_t *color = lv_event_get_user_data(event);
    lv_obj_t *screen = lv_obj_get_parent(lv_event_get_target_obj(event));
    lv_obj_t *field = lv_obj_create(screen);
    lv_obj_set_size(field, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_set_pos(field, 0, 0);
    lv_obj_set_style_radius(field, 0, 0);
    lv_obj_set_style_border_width(field, 0, 0);
    lv_obj_set_style_pad_all(field, 0, 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(*color), 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(field, close_color_field, LV_EVENT_CLICKED, NULL);
}

static void make_color_swatches(lv_obj_t *parent)
{
    static const uint32_t colors[] = {0xff3030, 0x30dd66, 0x368dff, 0xffffff};
    for (size_t index = 0; index < 4; index++) {
        lv_obj_t *swatch = lv_obj_create(parent);
        lv_obj_set_size(swatch, 45, 12);
        lv_obj_set_style_radius(swatch, 2, 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(colors[index]), 0);
        lv_obj_align(swatch, LV_ALIGN_TOP_MID,
                     (int)index * 49 - 74, 296);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(swatch, show_color_field, LV_EVENT_CLICKED,
                            (void *)&colors[index]);
    }
}

static void make_screen(gui_state_t *state,
                        const roundie_board_status_t *status,
                        uint32_t flash_bytes, size_t psram_bytes)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b2b), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    if (roundie_recovery_active()) {
        roundie_boot_status_t boot;
        roundie_boot_status(&boot);
        roundie_ota_status_t ota;
        roundie_ota_snapshot(&ota);
        make_label(screen, "ROUNDIE", 48, lv_color_hex(0xffffff));
        make_label(screen, "SAFE MODE", 86, lv_color_hex(0xffcc66));
        make_label(screen, "Recovery mode active", 130, lv_color_hex(0xffffff));
        lv_obj_t *reason = make_label(screen, "", 165, lv_color_hex(0xc3d2e2));
        lv_label_set_text_fmt(reason, "Reason: %s\nFirmware: %s\nOTA: %s / %s",
            roundie_boot_reason_name(boot.reason), esp_app_get_description()->version,
            ota.running.label, roundie_ota_image_state_name(ota.running.state));
        make_label(screen, "Connect USB for diagnostics", 260, lv_color_hex(0xffffff));
        return;
    }

    lv_obj_t *title = make_label(screen, "ROUNDIE", 24, lv_color_hex(0xffffff));
    lv_label_set_text_fmt(title, "ROUNDIE / %s", esp_app_get_description()->version);
    state->ota_label = make_label(screen, "OTA startup", 54, lv_color_hex(0x9eb8d0));
    lv_obj_set_style_text_font(state->ota_label, LV_FONT_DEFAULT, 0);

    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    lv_obj_t *memory = make_label(screen, "", 87, lv_color_hex(0xe1ecf6));
    lv_label_set_text_fmt(memory,
                          "Flash %" PRIu32 " MB   PSRAM %zu MB\n"
                          "SRAM %zu / %zu KiB\n"
                          "PSRAM %zu / %zu KiB",
                          flash_bytes / (1024U * 1024U),
                          psram_bytes / (1024U * 1024U),
                          internal_free / 1024U, internal_largest / 1024U,
                          psram_free / 1024U, psram_largest / 1024U);

    state->i2c_label = make_label(screen, "", 153, lv_color_hex(0xb6dce6));
    lv_label_set_text_fmt(state->i2c_label, "I2C  Touch %s  RTC %s  Gauge %s",
                          status->touch == ESP_OK ? "OK" : "ERR",
                          status->rtc == ESP_OK ? "OK" : "ERR",
                          status->gauge == ESP_OK ? "OK" : "ERR");
    state->rtc_label = make_label(screen, "Time unsynced", 178,
                                  lv_color_hex(0xc3d2e2));
    state->battery_label = make_label(screen, "Battery: reading", 204,
                                      lv_color_hex(0xc3d2e2));
    state->battery_detail_label = make_label(screen, "", 226,
                                             lv_color_hex(0x9eb8d0));
    state->touch_label = make_label(screen, "Touch to test", 246,
                                    lv_color_hex(0xffffff));
    lv_obj_t *brightness_button = lv_button_create(screen);
    lv_obj_set_size(brightness_button, 145, 30);
    lv_obj_align(brightness_button, LV_ALIGN_TOP_MID, 0, 265);
    lv_obj_add_event_cb(brightness_button, cycle_backlight, LV_EVENT_CLICKED,
                        state);
    state->brightness_label = lv_label_create(brightness_button);
    lv_label_set_text(state->brightness_label, "Backlight 60%");
    lv_obj_center(state->brightness_label);
    make_color_swatches(screen);

    lv_obj_t *spinner = lv_spinner_create(screen);
    lv_obj_set_size(spinner, 25, 25);
    lv_obj_align(spinner, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void headless_recovery(void)
{
    roundie_boot_enter_safe(ROUNDIE_SAFE_GUI);
    roundie_ota_gui_health(false);
    ESP_LOGW(TAG, "Headless Safe Mode: existing USB worker remains available");
    /* Keep app_main's stack alive: registered LCD callbacks may refer to it.
     * No display buffer is reused or freed while DMA might own it. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    roundie_boot_begin(roundie_boot_reset_class(), description->app_elf_sha256);
    ESP_LOGI(TAG, "Firmware %s %s, built %s %s, IDF %s",
             description->project_name, description->version,
             description->date, description->time, description->idf_ver);
    char elf_hash[65] = {0};
    esp_app_get_elf_sha256(elf_hash, sizeof(elf_hash));
    ESP_LOGI(TAG, "Firmware ELF SHA-256 prefix: %s", elf_hash);
    roundie_ota_diagnostics_init();
    esp_err_t nvs_error = nvs_flash_init();
    ESP_LOGI(TAG, "NVS initialization: %s; no erase fallback", esp_err_to_name(nvs_error));
    roundie_boot_persistence_init(nvs_error == ESP_OK);
    roundie_local_time_init(nvs_error == ESP_OK, wall_clock_valid);
    roundie_elf_set_clock_source(roundie_local_time_now);
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    ESP_LOGI(TAG, "BootGuard reset=%lu retained=%u failures=%lu safe=%u reason=%s trial=%u fault_remaining=%lu",
        (unsigned long)boot.reset, boot.retained_valid, (unsigned long)boot.failures, boot.safe,
        roundie_boot_reason_name(boot.reason), boot.trial, (unsigned long)boot.fault_remaining);
    if (boot.fault_app[0] != '\0') {
        ESP_LOGW(TAG, "Previous session failed with package %s loaded", boot.fault_app);
    }
    /* One-shot request, read and cleared here so any failure returns to normal
     * mode. Only a planned software restart outside Safe Mode starts it. */
    const bool storage_mode = roundie_storage_mode_begin(
        boot.reset == ROUNDIE_RESET_SOFTWARE && !boot.safe, description->app_elf_sha256);
    if (storage_mode) {
        ESP_LOGW(TAG, "USB storage mode: the USB PHY belongs to the storage drive; applications are inhibited");
    }
    roundie_ota_status_t startup_ota;
    roundie_ota_snapshot(&startup_ota);
    if (roundie_boot_fault_step(startup_ota.running.state == ESP_OTA_IMG_VALID)) {
        roundie_boot_status(&boot);
        ESP_LOGW(TAG, "BOUNDED BOOT TEST: VALID incomplete startup; remaining=%lu; software restart",
            (unsigned long)boot.fault_remaining);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        abort();
    }
    if (!roundie_recovery_active() && !roundie_storage_diagnostics()) {
        roundie_boot_enter_safe(ROUNDIE_SAFE_STORAGE);
    }
    roundie_boot_stage(ROUNDIE_BOOT_CORE);
    /* The sole serial/sensor owner starts independently of board and GUI.
     * Until board_ready is published it only services USB/OTA/recovery. */
    TaskHandle_t sensor_task = NULL;
    /* RP1 adds nested validation/hash/VFS calls. Budget 12 KiB, then observe
     * the worker high-water mark through RP1 instead of assuming host stacks
     * establish the Xtensa call-chain margin. */
    if (xTaskCreate(sensor_worker, "system_worker", 12288, &sensors, 4, &sensor_task) != pdPASS) {
        roundie_boot_enter_safe(ROUNDIE_SAFE_CORE);
        ESP_LOGE(TAG, "System worker creation failed; USB recovery unavailable, ROM recovery required");
        roundie_ota_startup_failed();
        return;
    }
    if (roundie_recovery_skip_gui()) {
        headless_recovery();
    }
    roundie_boot_stage(ROUNDIE_BOOT_GUI);

    uint32_t flash_bytes = 0;
    esp_err_t error = esp_flash_get_size(NULL, &flash_bytes);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Flash size query: %s", esp_err_to_name(error));
    }
    size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "Detected flash %" PRIu32 " bytes, PSRAM %zu bytes",
             flash_bytes, psram_bytes);
    ESP_LOGI(TAG, "Internal free %zu, largest %zu; PSRAM free %zu, largest %zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    roundie_board_t *board = NULL;
    roundie_board_status_t status;
    error = roundie_recovery_active() ? roundie_board_start_recovery(&board, &status) :
        roundie_board_start(&board, &status);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Bring-up stopped: %s", esp_err_to_name(error));
        headless_recovery();
    }

#if CONFIG_ROUNDIE_AUDIO
    /* Speaker output: normal mode only; a failure leaves audio off and changes nothing else. */
    if ((ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_SPEAKER) != 0 &&
        !roundie_recovery_active() && !storage_mode) {
        roundie_audio_init(board);
    }
#endif
    /* Startup samples remain private until the complete board is published. */
    roundie_rtc_sample_t initial_rtc;
    roundie_battery_sample_t initial_battery;
    roundie_board_rtc_read(board, &initial_rtc);
    roundie_board_battery_read(board, &initial_battery);
    ESP_LOGI(TAG, "RTC startup: %s, OS=%u, error=%s",
             sensor_state_name(initial_rtc.state), initial_rtc.oscillator_stopped,
             esp_err_to_name(initial_rtc.error));
    ESP_LOGI(TAG, "Gauge startup: %s, present=%u, %u mV, SoC valid=%u (%u%%), error=%s",
             sensor_state_name(initial_battery.state), initial_battery.battery_present,
             initial_battery.voltage_mv, initial_battery.soc_valid,
             initial_battery.soc_percent, esp_err_to_name(initial_battery.error));
#if BOARD_HAS_RTC
    bool initial_wall_valid = sync_wall_clock(&initial_rtc);
    if (!initial_wall_valid) {
        ESP_LOGW(TAG, "System wall clock unsynchronized; RTC time not trusted");
    }
#else
    bool initial_wall_valid = accept_retained_wall_clock();
#endif

    lv_init();
    lv_tick_set_cb(lv_tick_now);
    gui_state_t state = {
        .board = board,
        .gui_task = xTaskGetCurrentTaskHandle(),
        .dma_lock = portMUX_INITIALIZER_UNLOCKED,
        .brightness_percent = 60,
        .touch_available = status.touch == ESP_OK,
    };
#if DISPLAY_SCALED
    error = roundie_board_display_set_draw_done(board, lcd_chunk_done, &state);
#else
    error = roundie_board_display_set_draw_done(board, lcd_transfer_done, &state);
#endif
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD callback registration: %s", esp_err_to_name(error));
        headless_recovery();
    }

#if DISPLAY_SCALED
    /* LVGL renders canvas strips into one internal buffer, which is free again
     * when flush_scaled returns; two DMA chunks of DISPLAY_STRIP_LINES panel
     * rows alternate on the bus. The canvas strip is as tall as the worst-case
     * chunk (narrow areas get taller strips) allows. */
    const size_t chunk_pixels = (size_t)DISPLAY_WIDTH * DISPLAY_STRIP_LINES;
    uint32_t render_rows = CANVAS_HEIGHT;
    while (render_rows > 0 &&
           roundie_scale_max_output(&scale_geometry, CANVAS_WIDTH * render_rows) > chunk_pixels) {
        render_rows--;
    }
    if (render_rows < 3) {
        ESP_LOGE(TAG, "%u-line LCD chunks are too small to scale the canvas",
                 (unsigned)DISPLAY_STRIP_LINES);
        headless_recovery();
    }
    const size_t buffer_bytes = (size_t)CANVAS_WIDTH * render_rows * 2;
    void *draw_buffer = heap_caps_malloc(buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *draw_buffer_second = NULL;
    for (unsigned i = 0; i < 2; i++) {
        state.scale_output[i] = heap_caps_malloc(chunk_pixels * 2,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    state.scale_carry_pixels = heap_caps_malloc(DISPLAY_WIDTH * 2,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (draw_buffer == NULL || state.scale_output[0] == NULL || state.scale_output[1] == NULL ||
        state.scale_carry_pixels == NULL) {
        ESP_LOGE(TAG, "No internal memory for scaled LCD buffers");
        headless_recovery();
    }
    roundie_scale_columns(&scale_geometry, scale_columns);
    ESP_LOGI(TAG, "LVGL PARTIAL RGB565 %ux%u canvas, %" PRIu32 " lines (%zu bytes) scaled to "
             "%ux%u panel, 2 x %u-line DMA chunks",
             (unsigned)CANVAS_WIDTH, (unsigned)CANVAS_HEIGHT, render_rows, buffer_bytes,
             (unsigned)DISPLAY_WIDTH, (unsigned)DISPLAY_HEIGHT, (unsigned)DISPLAY_STRIP_LINES);
#else
    const size_t buffer_bytes = DISPLAY_WIDTH * DISPLAY_STRIP_LINES * 2;
    void *draw_buffer = heap_caps_malloc(buffer_bytes,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (draw_buffer == NULL) {
        ESP_LOGE(TAG, "No internal DMA memory for %zu-byte LVGL buffer",
                 buffer_bytes);
        headless_recovery();
    }
    void *draw_buffer_second = NULL;
    unsigned buffer_count = 1;
#if CONFIG_ROUNDIE_LCD_DOUBLE_BUFFER
    draw_buffer_second = heap_caps_malloc(buffer_bytes,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (draw_buffer_second == NULL) {
        heap_caps_free(draw_buffer);
        ESP_LOGE(TAG, "No internal DMA memory for second %zu-byte LVGL buffer",
                 buffer_bytes);
        headless_recovery();
    }
    buffer_count = 2;
#endif
    ESP_LOGI(TAG, "LVGL PARTIAL RGB565: %u x %u lines, %zu bytes each, internal DMA",
             buffer_count, DISPLAY_STRIP_LINES, buffer_bytes);
#endif

    state.display = lv_display_create(CANVAS_WIDTH, CANVAS_HEIGHT);
    if (state.display == NULL) {
        ESP_LOGE(TAG, "LVGL display allocation failed");
        headless_recovery();
    }
    lv_display_set_color_format(state.display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(state.display, &state);
    lv_display_add_event_cb(state.display, display_event, LV_EVENT_ALL, &state);
#if DISPLAY_SCALED
    lv_display_set_flush_cb(state.display, flush_scaled);
    lv_display_set_flush_wait_cb(state.display, wait_for_scaled_display);
#else
    lv_display_set_flush_cb(state.display, flush_display);
    lv_display_set_flush_wait_cb(state.display, wait_for_display);
#endif
    lv_display_set_buffers(state.display, draw_buffer, draw_buffer_second, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    /* A board without touch gets no pointer device; the UI still renders. */
    const bool use_touch = (ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_TOUCH) != 0 &&
                           !roundie_recovery_active();
    lv_indev_t *input = NULL;
    if (use_touch) {
        input = lv_indev_create();
    }
    if (input == NULL && use_touch) {
        ESP_LOGE(TAG, "LVGL input allocation failed");
        headless_recovery();
    }
    if (input != NULL) {
        lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(input, state.display);
        lv_indev_set_user_data(input, &state);
        lv_indev_set_read_cb(input, read_touch);
    }
    make_screen(&state, &status, flash_bytes, psram_bytes);
    roundie_ui_gui_bind();
#if CONFIG_ROUNDIE_APP_RUNTIME
    if (!roundie_recovery_active()) {
        roundie_launcher_gui_bind();
    }
#endif
    bool recovery_screen = roundie_recovery_active();
    if (!recovery_screen) {
        update_sensor_labels(&state);
    }
#if CONFIG_ROUNDIE_POWER
    if (!recovery_screen) {
        roundie_power_gui_bind(board, &state.brightness_percent, esp_timer_get_time());
#if CONFIG_ROUNDIE_APP_RUNTIME
        quick_state = &state;
        roundie_launcher_gui_set_quick_settings(&quick_settings);
#endif
    }
#endif

    if (storage_mode) {
        roundie_storage_screen_start(esp_timer_get_time());
    }
    error = roundie_board_backlight_set(board, 60);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Backlight start: %s", esp_err_to_name(error));
    }
    error = esp_task_wdt_add(NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "GUI watchdog registration: %s", esp_err_to_name(error));
        headless_recovery();
    }
    portENTER_CRITICAL(&sensors.lock);
    sensors.board = board;
    sensors.rtc = initial_rtc;
    sensors.battery = initial_battery;
    sensors.wall_valid = initial_wall_valid;
    sensors.board_ready = true;
    portEXIT_CRITICAL(&sensors.lock);
    bool health_sent = false;
    int64_t self_test_at_us = esp_timer_get_time() + 1000000;
    int64_t next_health_log_us = esp_timer_get_time() + 300000000;
    int64_t next_ui_update_us = esp_timer_get_time() + 1000000;
    int64_t expected_wake_us = 0;
    while (true) {
        int64_t iteration_at = esp_timer_get_time();
        uint32_t wake_us = expected_wake_us > 0 && iteration_at > expected_wake_us ?
            (uint32_t)(iteration_at - expected_wake_us) : 0;
#if CONFIG_ROUNDIE_POWER
        portENTER_CRITICAL(&state.dma_lock);
        bool dma_idle = state.dma_completions == (uint32_t)state.transfer_id;
        portEXIT_CRITICAL(&state.dma_lock);
        if (!roundie_power_gui_poll(iteration_at, dma_idle, roundie_gui_bench_due() || storage_mode)) {
            /* Parked for light sleep: no LVGL or LCD work until resumed. */
            error = esp_task_wdt_reset();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "GUI watchdog feed: %s", esp_err_to_name(error));
                esp_restart();
                abort();
            }
            vTaskDelay(pdMS_TO_TICKS(GUI_DELAY_MAX_MS));
            expected_wake_us = 0;
            continue;
        }
#endif
        memset(&state.work, 0, sizeof(state.work));
        state.iteration_id++;
        if (state.display_fault) {
            roundie_boot_enter_safe(ROUNDIE_SAFE_GUI);
        }
        static roundie_ui_status_t ui_before;
        roundie_ui_status(&ui_before);
        roundie_ui_gui_batch(roundie_gui_batch());
        roundie_ui_gui_pump();
#if CONFIG_ROUNDIE_APP_RUNTIME
        roundie_launcher_gui_pump(roundie_recovery_active());
#endif
        if (storage_mode) {
            roundie_storage_screen_poll(iteration_at);
        }
        roundie_ui_display_progress(state.completed_transfers, state.transfer_errors);
        static roundie_ui_status_t app_ui;
        roundie_ui_status(&app_ui);
        uint32_t pump_us = (uint32_t)(esp_timer_get_time() - iteration_at);
        uint32_t applied = app_ui.generation == ui_before.generation && app_ui.applied >= ui_before.applied ?
            app_ui.applied - ui_before.applied : app_ui.applied;
        if (roundie_recovery_active() && !recovery_screen && app_ui.generation == 0) {
            if (input != NULL) {
                lv_indev_enable(input, false);
            }
            lv_obj_clean(lv_screen_active());
            make_screen(&state, &status, flash_bytes, psram_bytes);
            recovery_screen = true;
        }
        if (!recovery_screen && roundie_gui_bench_due()) {
            lv_obj_invalidate(lv_screen_active());
        }
        int64_t handler_at = esp_timer_get_time();
        uint32_t next_timer_ms = lv_timer_handler();
        uint32_t handler_us = (uint32_t)(esp_timer_get_time() - handler_at);
        if (!health_sent && esp_timer_get_time() >= self_test_at_us) {
            bool memory_ok = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16384 &&
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 8192 &&
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 131072;
            bool mode_allows_confirmation = roundie_recovery_confirmation_allowed(startup_ota.running.state);
            roundie_ota_gui_health(mode_allows_confirmation && sensor_task != NULL && flash_bytes == ROUNDIE_BOARD_FLASH_BYTES &&
                psram_bytes == ROUNDIE_BOARD_PSRAM_BYTES && memory_ok && !state.display_fault &&
                state.transfer_errors == 0 && state.completed_transfers > 0);
            health_sent = true;
        }
        if (!recovery_screen && esp_timer_get_time() >= next_ui_update_us) {
            update_sensor_labels(&state);
            next_ui_update_us = esp_timer_get_time() + 1000000;
        }
        if (esp_timer_get_time() >= next_health_log_us) {
            ESP_LOGI(TAG,
                     "Health SRAM free/largest %zu/%zu, PSRAM free/largest %zu/%zu, LCD errors %u",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                     state.transfer_errors);
            next_health_log_us = esp_timer_get_time() + 300000000;
        }
        error = esp_task_wdt_reset();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "GUI watchdog feed: %s", esp_err_to_name(error));
            esp_restart();
            abort();
        }
#if DISPLAY_SCALED
        /* Chunks still on the bus at the end of a refresh finish later. */
        collect_chunks(&state);
#endif
        portENTER_CRITICAL(&state.dma_lock);
        uint32_t dma_completions = state.dma_completions;
        portEXIT_CRITICAL(&state.dma_lock);
        roundie_gui_record(iteration_at, pump_us, handler_us, wake_us, ui_before.pending,
            app_ui.pending, applied, &state.work, state.transfer_errors, dma_completions);
        /* Always sleep at least one tick so idle tasks keep their watchdog. */
        if (next_timer_ms > GUI_DELAY_MAX_MS) {
            next_timer_ms = GUI_DELAY_MAX_MS;
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(next_timer_ms);
        if (delay_ticks == 0) {
            delay_ticks = 1;
        }
        expected_wake_us = esp_timer_get_time() + (int64_t)delay_ticks * portTICK_PERIOD_MS * 1000;
        vTaskDelay(delay_ticks);
    }
}
