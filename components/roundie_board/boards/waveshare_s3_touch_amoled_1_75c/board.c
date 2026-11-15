#include "roundie_board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "board_logic.h"
#include "board_pins.h"

#define LCD_WIDTH ROUNDIE_BOARD_DISPLAY_WIDTH
#define LCD_HEIGHT ROUNDIE_BOARD_DISPLAY_HEIGHT
#define LCD_TRANSFER_BYTES (LCD_WIDTH * ROUNDIE_BOARD_DISPLAY_STRIP_LINES * 2)
#define I2C_TIMEOUT_MS 100

static const char *TAG = "roundie_board";

/* CO5300 vendor sequence from the Waveshare ESP32-S3-Touch-AMOLED-1.75C BSP
 * 3.0.0 (Apache-2.0, Copyright Waveshare). One change: brightness (0x51) starts
 * at 0 instead of 0xFF, so the panel stays dark until the firmware sets it;
 * the board applies the stored brightness after every initialization. */
static const co5300_lcd_init_cmd_t lcd_init_commands[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static void log_cleanup_error(const char *resource, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Cleanup %s: %s", resource, esp_err_to_name(error));
    }
}

struct roundie_board {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t pmic;
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_touch_handle_t touch;
    volatile bool touch_irq_pending;
    board_touch_filter_t touch_filter;
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t panel;
    bool spi_started;
    uint8_t brightness_percent;
    bool boot_button_ready;
    bool wake_armed;
    roundie_board_draw_done_t draw_done;
    void *draw_done_context;
    const audio_codec_ctrl_if_t *codec_control;
};

static esp_err_t read_register(i2c_master_dev_handle_t device, uint8_t address,
                               uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(device, &address, 1, data, length,
                                       I2C_TIMEOUT_MS);
}

static esp_err_t start_i2c(roundie_board_t *board, roundie_board_status_t *status)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &board->i2c_bus);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "I2C master start: %s", esp_err_to_name(error));
        status->touch = error;
        status->gauge = error;
        return error;
    }

    status->touch = i2c_master_probe(board->i2c_bus, BOARD_TOUCH_ADDRESS, I2C_TIMEOUT_MS);
    status->gauge = i2c_master_probe(board->i2c_bus, BOARD_PMIC_ADDRESS, I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "I2C probe CST9217 0x5A: %s", esp_err_to_name(status->touch));
    ESP_LOGI(TAG, "I2C probe AXP2101 0x34: %s", esp_err_to_name(status->gauge));

    const i2c_device_config_t pmic_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_PMIC_ADDRESS,
        .scl_speed_hz = BOARD_I2C_DEVICE_HZ,
    };
    esp_err_t pmic_add = i2c_master_bus_add_device(board->i2c_bus, &pmic_config,
                                                    &board->pmic);
    if (pmic_add != ESP_OK) {
        ESP_LOGE(TAG, "PMIC handle: %s", esp_err_to_name(pmic_add));
        status->gauge = pmic_add;
    }
    return ESP_OK;
}

static void touch_interrupt(esp_lcd_touch_handle_t touch)
{
    roundie_board_t *board = touch->config.user_data;
    board->touch_irq_pending = true;
}

static esp_err_t start_touch(roundie_board_t *board)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_config.scl_speed_hz = BOARD_I2C_DEVICE_HZ;
    esp_err_t error = esp_lcd_new_panel_io_i2c(board->i2c_bus, &io_config,
                                               &board->touch_io);
    if (error != ESP_OK) {
        return error;
    }

    /* Mirrored on both axes as in the Waveshare BSP; orientation on this
     * board is not yet confirmed by a touch test. */
    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = BOARD_TOUCH_RESET,
        .int_gpio_num = BOARD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .mirror_x = 1,
            .mirror_y = 1,
        },
        .interrupt_callback = NULL,
        .user_data = board,
    };
    error = esp_lcd_touch_new_i2c_cst9217(board->touch_io, &touch_config,
                                          &board->touch);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = esp_lcd_touch_register_interrupt_callback(board->touch,
                                                       touch_interrupt);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CST9217 IRQ registration: %s", esp_err_to_name(error));
        goto cleanup;
    }
    /* The main board has no pull-up on TP_INT (schematic S2: J3 pin 30 to
     * GPIO11) and the driver configures none. Whether the controller drives
     * the line push-pull is unknown, so the internal pull-up keeps an idle
     * line from floating into interrupts or light-sleep wakes. */
    error = gpio_pullup_en(BOARD_TOUCH_INT);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CST9217 INT pull-up: %s", esp_err_to_name(error));
        goto cleanup;
    }
    return ESP_OK;

cleanup:
    if (board->touch != NULL) {
        log_cleanup_error("touch after failed start",
                          esp_lcd_touch_del(board->touch));
        board->touch = NULL;
    }
    log_cleanup_error("touch IO after failed start",
                      esp_lcd_panel_io_del(board->touch_io));
    board->touch_io = NULL;
    return error;
}

/* Reset, vendor sequence (which ends with display on), stored brightness. */
static esp_err_t initialize_panel(roundie_board_t *board)
{
    esp_err_t error = esp_lcd_panel_reset(board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_init(board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_co5300_set_brightness(board->panel, board->brightness_percent);
    if (error != ESP_OK) {
        return error;
    }
    return esp_lcd_panel_disp_on_off(board->panel, true);
}

static esp_err_t start_lcd(roundie_board_t *board)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .data0_io_num = BOARD_LCD_DATA0,
        .data1_io_num = BOARD_LCD_DATA1,
        .data2_io_num = BOARD_LCD_DATA2,
        .data3_io_num = BOARD_LCD_DATA3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_TRANSFER_BYTES + 64,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    esp_err_t error = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        return error;
    }
    board->spi_started = true;

    /* Two queued draws at most (roundie_board.h). */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = 1,
    };
    error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                     &io_config, &board->lcd_io);
    if (error != ESP_OK) {
        return error;
    }

    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_commands,
        .init_cmds_size = sizeof(lcd_init_commands) / sizeof(lcd_init_commands[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    error = esp_lcd_new_panel_co5300(board->lcd_io, &panel_config, &board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_set_gap(board->panel, BOARD_LCD_COLUMN_GAP, 0);
    if (error != ESP_OK) {
        return error;
    }
    return initialize_panel(board);
}

static esp_err_t start_board(roundie_board_t **out_board,
                              roundie_board_status_t *status, bool recovery)
{
    if (out_board == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_board = NULL;
    memset(status, 0, sizeof(*status));
    status->touch = ESP_ERR_NOT_FOUND;
    status->rtc = ESP_ERR_NOT_FOUND; /* no RTC chip on this board */
    status->gauge = ESP_ERR_NOT_FOUND;

    roundie_board_t *board = calloc(1, sizeof(*board));
    if (board == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Recovery starts no I2C: the panel relies on the AXP2101 power-on rails. */
    if (!recovery) {
        esp_err_t i2c_error = start_i2c(board, status);
        if (i2c_error != ESP_OK) {
            roundie_board_stop(board);
            return i2c_error;
        }
        status->touch = start_touch(board);
        ESP_LOGI(TAG, "CST9217 driver: %s", esp_err_to_name(status->touch));
        /* Input only, never driven: GPIO0 stays the ROM Download strap. */
        const gpio_config_t button_config = {
            .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t button_error = gpio_config(&button_config);
        board->boot_button_ready = button_error == ESP_OK;
        if (button_error != ESP_OK) {
            ESP_LOGE(TAG, "BOOT button input: %s", esp_err_to_name(button_error));
        }
    }

    esp_err_t error = start_lcd(board);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD start failed: %s", esp_err_to_name(error));
        roundie_board_stop(board);
        return error;
    }
    *out_board = board;
    return ESP_OK;
}

esp_err_t roundie_board_start(roundie_board_t **out_board,
                              roundie_board_status_t *status)
{
    return start_board(out_board, status, false);
}

esp_err_t roundie_board_start_recovery(roundie_board_t **out_board,
                                      roundie_board_status_t *status)
{
    return start_board(out_board, status, true);
}

void roundie_board_stop(roundie_board_t *board)
{
    if (board == NULL) {
        return;
    }
    if (board->panel != NULL) {
        /* Also releases the reset GPIO. */
        log_cleanup_error("LCD panel", esp_lcd_panel_del(board->panel));
    }
    if (board->lcd_io != NULL) {
        log_cleanup_error("LCD IO", esp_lcd_panel_io_del(board->lcd_io));
    }
    if (board->spi_started) {
        log_cleanup_error("LCD SPI bus", spi_bus_free(BOARD_LCD_SPI_HOST));
    }
    if (board->touch != NULL) {
        log_cleanup_error("touch", esp_lcd_touch_del(board->touch));
    }
    if (board->touch_io != NULL) {
        log_cleanup_error("touch IO", esp_lcd_panel_io_del(board->touch_io));
    }
    if (board->pmic != NULL) {
        log_cleanup_error("PMIC I2C device", i2c_master_bus_rm_device(board->pmic));
    }
    if (board->i2c_bus != NULL) {
        log_cleanup_error("I2C bus", i2c_del_master_bus(board->i2c_bus));
    }
    free(board);
}

/* SPI color-transfer completion, ISR context. */
static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *event,
                              void *user_context)
{
    (void)io;
    (void)event;
    roundie_board_t *board = user_context;
    return board->draw_done(board->draw_done_context);
}

esp_err_t roundie_board_display_set_draw_done(roundie_board_t *board,
                                              roundie_board_draw_done_t callback,
                                              void *context)
{
    if (board == NULL || board->lcd_io == NULL || callback == NULL ||
        board->draw_done != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    board->draw_done = callback;
    board->draw_done_context = context;
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lcd_transfer_done,
    };
    esp_err_t error = esp_lcd_panel_io_register_event_callbacks(board->lcd_io,
                                                                &callbacks, board);
    if (error != ESP_OK) {
        board->draw_done = NULL;
        board->draw_done_context = NULL;
    }
    return error;
}

esp_err_t roundie_board_display_draw(roundie_board_t *board, int x1, int y1,
                                     int x2, int y2, const void *pixels)
{
    return esp_lcd_panel_draw_bitmap(board->panel, x1, y1, x2, y2, pixels);
}

esp_err_t roundie_board_touch_read(roundie_board_t *board, bool *pressed,
                                   uint16_t *x, uint16_t *y)
{
    if (board == NULL || pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (board->touch == NULL) {
        *pressed = false;
        return ESP_ERR_NOT_FOUND;
    }
    if (!board->touch_irq_pending && !board->touch_filter.pressed) {
        *pressed = false;
        return ESP_OK;
    }
    /* While a touch is held, every call reads, without waiting for INT. */
    board->touch_irq_pending = false;
    board_touch_report_t report = BOARD_TOUCH_NONE;
    esp_lcd_touch_point_data_t point = {0};
    esp_err_t error = esp_lcd_touch_read_data(board->touch);
    if (error == ESP_ERR_INVALID_RESPONSE) {
        /* No acknowledge byte; the driver kept no new data. Assumption: a
         * report between updates, not a bus fault, so it must not trigger the
         * caller's retry backoff (board_logic.h). */
        report = BOARD_TOUCH_NO_ACK;
    } else if (error != ESP_OK) {
        board_touch_filter_reset(&board->touch_filter);
        *pressed = false;
        return error;
    } else {
        uint8_t point_count = 0;
        error = esp_lcd_touch_get_data(board->touch, &point, &point_count, 1);
        if (error != ESP_OK) {
            board_touch_filter_reset(&board->touch_filter);
            *pressed = false;
            return error;
        }
        if (point_count > 0) {
            report = BOARD_TOUCH_POINT;
        }
    }
    *pressed = board_touch_filter_update(&board->touch_filter, report, point.x, point.y,
                                         LCD_WIDTH, LCD_HEIGHT, x, y);
    return ESP_OK;
}

/* AMOLED: no backlight; brightness is the panel's 0x51 command, sent in the
 * GUI task between draws. Kept so a panel re-initialization restores it. */
esp_err_t roundie_board_backlight_set(roundie_board_t *board, uint8_t percent)
{
    if (board == NULL || board->panel == NULL || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = esp_lcd_panel_co5300_set_brightness(board->panel, percent);
    if (error == ESP_OK) {
        board->brightness_percent = percent;
    }
    return error;
}

/* Screen-off is brightness 0 plus DISPOFF; the panel stays powered and keeps
 * its frame memory, also through light sleep. A reset and the vendor sequence
 * would instead show undefined memory until the repaint reaches it: a green band
 * at the bottom, painted last, was seen on wake. So the panel is only switched
 * on again and the kept frame is shown at once. Device test 2026-11-14 (owner
 * observation): no band after screen-off, and the screen came back after touch
 * and BOOT wakes from light sleep (docs/hardware-audit-1.75c.md). */
esp_err_t roundie_board_display_restore(roundie_board_t *board, bool *repaint)
{
    if (board == NULL || board->panel == NULL || repaint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *repaint = false;
    return esp_lcd_panel_disp_on_off(board->panel, true);
}

esp_err_t roundie_board_display_power(roundie_board_t *board, bool on)
{
    if (board == NULL || board->panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_lcd_panel_disp_on_off(board->panel, on);
}

bool roundie_board_boot_pressed(roundie_board_t *board)
{
    if (board == NULL || !board->boot_button_ready) {
        return false;
    }
    return gpio_get_level(BOARD_BOOT_BUTTON) == 0;
}

bool roundie_board_wake_source_active(roundie_board_t *board)
{
    if (board == NULL) {
        return false;
    }
    bool touch_low = board->touch != NULL && gpio_get_level(BOARD_TOUCH_INT) == 0;
    return touch_low || roundie_board_boot_pressed(board);
}

esp_err_t roundie_board_wake_arm(roundie_board_t *board)
{
    if (board == NULL || board->wake_armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (board->touch == NULL && !board->boot_button_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t error = ESP_OK;
    board->wake_armed = true;
    if (board->touch != NULL) {
        /* Level wake replaces the edge type; keep the CPU interrupt off so a
         * held touch cannot retrigger it until roundie_board_wake_disarm(). */
        error = gpio_intr_disable(BOARD_TOUCH_INT);
        if (error == ESP_OK) {
            error = gpio_wakeup_enable(BOARD_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
        }
    }
    if (error == ESP_OK && board->boot_button_ready) {
        error = gpio_wakeup_enable(BOARD_BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
    }
    if (error == ESP_OK) {
        error = esp_sleep_enable_gpio_wakeup();
    }
    if (error != ESP_OK) {
        roundie_board_wake_disarm(board);
    }
    return error;
}

void roundie_board_wake_disarm(roundie_board_t *board)
{
    if (board == NULL || !board->wake_armed) {
        return;
    }
    log_cleanup_error("GPIO wake source",
                      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO));
    if (board->boot_button_ready) {
        log_cleanup_error("BOOT wake", gpio_wakeup_disable(BOARD_BOOT_BUTTON));
        log_cleanup_error("BOOT interrupt type",
                          gpio_set_intr_type(BOARD_BOOT_BUTTON, GPIO_INTR_DISABLE));
    }
    if (board->touch != NULL) {
        /* Sample before restoring: low means the controller is reporting a
         * touch. The waking edge happened while the interrupt was off. */
        bool touch_low = gpio_get_level(BOARD_TOUCH_INT) == 0;
        log_cleanup_error("touch wake", gpio_wakeup_disable(BOARD_TOUCH_INT));
        log_cleanup_error("touch interrupt type",
                          gpio_set_intr_type(BOARD_TOUCH_INT, GPIO_INTR_NEGEDGE));
        log_cleanup_error("touch interrupt", gpio_intr_enable(BOARD_TOUCH_INT));
        if (touch_low) {
            board->touch_irq_pending = true;
        }
    }
    board->wake_armed = false;
}

/* Identity probe, then I2S, then the ES8311 through esp_codec_dev, as on the
 * 1.85B with this board's pins. Started once; nothing here is torn down. */
esp_err_t roundie_board_speaker_start(roundie_board_t *board, uint32_t sample_rate,
                                      roundie_board_speaker_t *speaker)
{
    if (speaker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *speaker = (roundie_board_speaker_t){.failed_step = "i2c bus"};
    if (board == NULL || board->i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (board->codec_control != NULL) {
        speaker->failed_step = "already started";
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t probe = i2c_master_probe(board->i2c_bus, BOARD_CODEC_ADDRESS, I2C_TIMEOUT_MS);
    speaker->codec_ack = probe == ESP_OK;
    if (!speaker->codec_ack) {
        speaker->failed_step = "codec not found";
        return probe;
    }
    const i2c_device_config_t identity_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CODEC_ADDRESS,
        .scl_speed_hz = BOARD_I2C_DEVICE_HZ,
    };
    i2c_master_dev_handle_t identity;
    if (i2c_master_bus_add_device(board->i2c_bus, &identity_config, &identity) == ESP_OK) {
        for (uint8_t index = 0; index < 3; index++) {
            uint8_t reg = (uint8_t)(0xFD + index);
            i2c_master_transmit_receive(identity, &reg, 1, &speaker->codec_id[index], 1,
                                        I2C_TIMEOUT_MS);
        }
        i2c_master_bus_rm_device(identity);
    }

    i2s_chan_handle_t transmit = NULL;
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true; /* silence, not a repeated buffer, if the feed stops */
    esp_err_t error = i2s_new_channel(&channel_config, &transmit, NULL);
    if (error != ESP_OK) {
        speaker->failed_step = "i2s channel";
        return error;
    }
    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    error = i2s_channel_init_std_mode(transmit, &standard_config);
    if (error != ESP_OK) {
        i2s_del_channel(transmit);
        speaker->failed_step = "i2s init";
        return error;
    }

    audio_codec_i2c_cfg_t i2c_config = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = board->i2c_bus};
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c_config);
    board->codec_control = control;
    audio_codec_i2s_cfg_t i2s_config = {.port = BOARD_I2S_PORT, .tx_handle = transmit, .rx_handle = NULL};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_config);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    if (control == NULL || data == NULL || gpio == NULL) {
        i2s_del_channel(transmit);
        speaker->failed_step = "codec interfaces";
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin = BOARD_SPEAKER_PA,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0F, .codec_dac_voltage = 3.3F},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&codec_config);
    if (codec == NULL) {
        i2s_del_channel(transmit);
        speaker->failed_step = "es8311";
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t device_config = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    speaker->device = esp_codec_dev_new(&device_config);
    if (speaker->device == NULL) {
        i2s_del_channel(transmit);
        speaker->failed_step = "codec device";
        return ESP_FAIL;
    }
    speaker->failed_step = "ok";
    ESP_LOGI(TAG, "Speaker: ES8311 id %02x %02x %02x, amplifier on GPIO%d", speaker->codec_id[0],
             speaker->codec_id[1], speaker->codec_id[2], (int)BOARD_SPEAKER_PA);
    return ESP_OK;
}

esp_err_t roundie_board_speaker_read_register(roundie_board_t *board, uint8_t reg, uint8_t *value)
{
    if (board == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const audio_codec_ctrl_if_t *control = board->codec_control;
    if (control == NULL || control->read_reg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return control->read_reg(control, reg, 1, value, 1) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void roundie_board_rtc_read(roundie_board_t *board, roundie_rtc_sample_t *sample)
{
    (void)board;
    if (sample == NULL) {
        return;
    }
    memset(sample, 0, sizeof(*sample));
    sample->state = ROUNDIE_SENSOR_NOT_FOUND;
    sample->error = ESP_ERR_NOT_FOUND;
    sample->observed_us = esp_timer_get_time();
}

esp_err_t roundie_board_rtc_set_trusted(roundie_board_t *board,
                                         const roundie_rtc_time_t *time)
{
    (void)board;
    (void)time;
    return ESP_ERR_NOT_SUPPORTED;
}

static roundie_sensor_state_t pmic_error(roundie_board_t *board, esp_err_t error)
{
    if (error == ESP_ERR_NOT_FOUND) {
        return ROUNDIE_SENSOR_NOT_FOUND;
    }
    if (board->i2c_bus != NULL &&
        i2c_master_probe(board->i2c_bus, BOARD_PMIC_ADDRESS, I2C_TIMEOUT_MS) ==
            ESP_ERR_NOT_FOUND) {
        return ROUNDIE_SENSOR_NOT_FOUND;
    }
    return ROUNDIE_SENSOR_COMM_ERROR;
}

/* AXP2101, read only: nothing is written to the PMIC. Register meanings are a
 * hypothesis taken from the XPowersLib register map, not yet checked on this
 * board (docs/hardware-audit-1.75c.md):
 *   0x00 bit 3       battery connected
 *   0x01 bits 6..5   01 charging, 10 discharging
 *   0x34..0x35       battery voltage in mV, 13 bits; reads 0 while the
 *                    PMIC's battery voltage ADC is disabled
 * The PMIC's own fuel gauge is not used, so the state is at best
 * ROUNDIE_SENSOR_UNCALIBRATED with no state of charge, current or temperature. */
void roundie_board_battery_read(roundie_board_t *board,
                                 roundie_battery_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    memset(sample, 0, sizeof(*sample));
    sample->state = ROUNDIE_SENSOR_NOT_FOUND;
    sample->error = ESP_ERR_NOT_FOUND;
    sample->observed_us = esp_timer_get_time();
    if (board == NULL || board->pmic == NULL) {
        return;
    }
    uint8_t status[2];
    esp_err_t error = read_register(board->pmic, 0x00, status, sizeof(status));
    if (error != ESP_OK) {
        sample->error = error;
        sample->state = pmic_error(board, error);
        return;
    }
    sample->battery_flags = (uint16_t)((status[0] << 8) | status[1]);
    sample->battery_present = board_pmic_battery_present(status);
    if (!sample->battery_present) {
        sample->state = ROUNDIE_SENSOR_BATTERY_ABSENT;
        sample->error = ESP_OK;
        return;
    }
    sample->discharging = board_pmic_discharging(status);
    sample->state = ROUNDIE_SENSOR_UNCALIBRATED;
    sample->error = ESP_OK;

    uint8_t voltage[2];
    error = read_register(board->pmic, 0x34, voltage, sizeof(voltage));
    if (error != ESP_OK) {
        sample->error = error;
        sample->state = pmic_error(board, error);
        return;
    }
    uint16_t millivolts = board_pmic_battery_mv(voltage);
    sample->voltage_mv = millivolts;
    sample->voltage_valid = board_pmic_voltage_plausible(millivolts);
    sample->observed_us = esp_timer_get_time();
}
