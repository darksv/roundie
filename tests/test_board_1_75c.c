#include "board_logic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define WIDTH 466
#define HEIGHT 466

static bool update(board_touch_filter_t *filter, board_touch_report_t report,
                   uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    return board_touch_filter_update(filter, report, raw_x, raw_y, WIDTH, HEIGHT, x, y);
}

static void check_touch_filter(void)
{
    board_touch_filter_t filter = {0};
    uint16_t x = 7;
    uint16_t y = 9;

    /* Idle: no report and an unacknowledged report never start a touch, and
     * the outputs are left alone. */
    assert(!update(&filter, BOARD_TOUCH_NONE, 0, 0, &x, &y));
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 100, 100, &x, &y));
    assert(x == 7 && y == 9);

    /* A point is reported as is. */
    assert(update(&filter, BOARD_TOUCH_POINT, 100, 200, &x, &y));
    assert(x == 100 && y == 200);

    /* Unacknowledged reads hold the last point up to the limit, then release. */
    for (unsigned i = 0; i < BOARD_TOUCH_NO_ACK_LIMIT; i++) {
        x = 0;
        y = 0;
        assert(update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));
        assert(x == 100 && y == 200);
    }
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));
    assert(!filter.pressed);
    /* Released: further unacknowledged reads do not bring it back. */
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));

    /* Each acknowledged point restarts the allowance: a long press with
     * occasional unacknowledged reads stays one press. */
    for (unsigned round = 0; round < 50; round++) {
        uint16_t px = (uint16_t)(10U + round);
        assert(update(&filter, BOARD_TOUCH_POINT, px, 20, &x, &y));
        assert(x == px && y == 20);
        for (unsigned i = 0; i < BOARD_TOUCH_NO_ACK_LIMIT; i++) {
            assert(update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));
            assert(x == px && y == 20);
        }
    }

    /* An acknowledged report without a point releases at once. */
    assert(!update(&filter, BOARD_TOUCH_NONE, 0, 0, &x, &y));
    assert(filter.unacknowledged == 0);

    /* A bus error resets the filter. */
    assert(update(&filter, BOARD_TOUCH_POINT, 1, 1, &x, &y));
    board_touch_filter_reset(&filter);
    assert(!filter.pressed && filter.unacknowledged == 0);
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));

    /* Edges and corners. The driver mirrors as x_max - x with x_max = 466,
     * so a raw 0 arrives as 466: clamped to the last panel pixel. */
    const uint16_t edges[][4] = {
        {0, 0, 0, 0},
        {465, 0, 465, 0},
        {0, 465, 0, 465},
        {465, 465, 465, 465},
        {466, 466, 465, 465},
        {466, 0, 465, 0},
        {0, 466, 0, 465},
        {UINT16_MAX, UINT16_MAX, 465, 465},
        {233, 233, 233, 233},
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        assert(update(&filter, BOARD_TOUCH_POINT, edges[i][0], edges[i][1], &x, &y));
        assert(x == edges[i][2] && y == edges[i][3]);
        /* A held clamped point stays clamped. */
        assert(update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));
        assert(x == edges[i][2] && y == edges[i][3]);
    }
}

static void check_pmic(void)
{
    /* 0x00 bit 3: battery connected. */
    assert(board_pmic_battery_present((const uint8_t[]){0x08, 0x00}));
    assert(board_pmic_battery_present((const uint8_t[]){0xFF, 0x00}));
    assert(!board_pmic_battery_present((const uint8_t[]){0xF7, 0xFF}));
    assert(!board_pmic_battery_present((const uint8_t[]){0x00, 0xFF}));

    /* 0x01 bits 6..5: 01 charging, 10 discharging, 00 standby. */
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x00}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x20}));
    assert(board_pmic_discharging((const uint8_t[]){0x08, 0x40}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x60}));
    /* Bits outside 6..5 do not change the direction. */
    assert(board_pmic_discharging((const uint8_t[]){0x08, 0xDF}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x9F}));

    /* 0x34..0x35: 13-bit millivolts, upper three bits ignored. */
    assert(board_pmic_battery_mv((const uint8_t[]){0x00, 0x00}) == 0);
    assert(board_pmic_battery_mv((const uint8_t[]){0x0F, 0xA0}) == 4000);
    assert(board_pmic_battery_mv((const uint8_t[]){0xEF, 0xA0}) == 4000);
    assert(board_pmic_battery_mv((const uint8_t[]){0x1F, 0xFF}) == 8191);
    assert(board_pmic_battery_mv((const uint8_t[]){0xFF, 0xFF}) == 8191);

    assert(!board_pmic_voltage_plausible(0)); /* ADC disabled */
    assert(board_pmic_voltage_plausible(1));
    assert(board_pmic_voltage_plausible(3700));
    assert(board_pmic_voltage_plausible(6000));
    assert(!board_pmic_voltage_plausible(6001));
    assert(!board_pmic_voltage_plausible(8191));

    assert(board_pmic_percent_valid(0));
    assert(board_pmic_percent_valid(100));
    assert(!board_pmic_percent_valid(101));
    assert(!board_pmic_percent_valid(0xFF));

    /* 0x92 bits 4..0: 0.5 V + 100 mV per step, 28 = 3.3 V, higher codes reserved. */
    assert(board_pmic_aldo1_mv(0x00) == 500);
    assert(board_pmic_aldo1_mv(0x1C) == 3300);
    assert(board_pmic_aldo1_mv(0xFC) == 3300);
    assert(board_pmic_aldo1_mv(0x1D) == 0);
    assert(board_pmic_aldo1_mv(0x1F) == 0);
}

int main(void)
{
    check_touch_filter();
    check_pmic();
    puts("board 1.75C logic: OK");
    return 0;
}
