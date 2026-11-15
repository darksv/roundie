#pragma once

#include "driver/gpio.h"

/* Waveshare ESP32-S3-Touch-AMOLED-1.75C wiring, from the Waveshare schematic and
 * BSP (docs/hardware-audit-1.75c.md). The only place in the firmware that names
 * this board's GPIOs, buses and I2C addresses. */

/* Shared I2C master: touch, PMIC and the audio codec. */
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_15
#define BOARD_I2C_SCL GPIO_NUM_14
#define BOARD_I2C_DEVICE_HZ 100000

/* CST9217: INT is driven low on touch. */
#define BOARD_TOUCH_ADDRESS 0x5A
#define BOARD_TOUCH_RESET GPIO_NUM_2
#define BOARD_TOUCH_INT GPIO_NUM_11

/* AXP2101 power management IC; read only. */
#define BOARD_PMIC_ADDRESS 0x34

/* BOOT: active low, the ROM Download strap; input only. PWR goes to the
 * AXP2101, not to a GPIO. */
#define BOARD_BOOT_BUTTON GPIO_NUM_0

/* CO5300 on QSPI. TE (GPIO13) is not used. No backlight: brightness is a
 * panel command. */
#define BOARD_LCD_SPI_HOST SPI2_HOST
#define BOARD_LCD_SCLK GPIO_NUM_38
#define BOARD_LCD_DATA0 GPIO_NUM_4
#define BOARD_LCD_DATA1 GPIO_NUM_5
#define BOARD_LCD_DATA2 GPIO_NUM_6
#define BOARD_LCD_DATA3 GPIO_NUM_7
#define BOARD_LCD_CS GPIO_NUM_12
#define BOARD_LCD_RESET GPIO_NUM_1
/* Waveshare BSP value; the panel window starts at column 6. */
#define BOARD_LCD_COLUMN_GAP 6
/* Waveshare BSP clock; not measured on this board. */
#define BOARD_LCD_PCLK_HZ 40000000

/* Speaker: ES8311 codec on the shared I2C bus, I2S transmit only, amplifier
 * enabled by the codec driver. GPIO10 (codec ADC data) is unused. */
#define BOARD_CODEC_ADDRESS 0x18 /* 7-bit */
#define BOARD_I2S_PORT I2S_NUM_0
#define BOARD_I2S_MCLK GPIO_NUM_16
#define BOARD_I2S_BCLK GPIO_NUM_9
#define BOARD_I2S_WS GPIO_NUM_45
#define BOARD_I2S_DOUT GPIO_NUM_8
#define BOARD_I2S_DIN GPIO_NUM_10 /* ES7210 data out; capture verified on the board */
#define BOARD_SPEAKER_PA GPIO_NUM_46
