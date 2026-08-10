/*
 * board_io.h — the board's power sensing.
 *
 * On the EE04 this is only the battery: there is no RTC on the board, and the
 * two pins the previous carrier routed to an I2C header (GPIO5/GPIO6) are a
 * user button and the battery divider's load-switch enable here. The clock
 * therefore comes from SNTP alone — see net_time.c.
 *
 * The battery divider is 1:3 and sits BEHIND a load switch, so it reads noise
 * until its enable is driven HIGH. board_io_init() drives it and leaves it on;
 * the divider's standing draw is a few microamps, which is nothing against a
 * board whose panel is the interesting load.
 *
 * Every getter is defensive: if the ADC is unavailable it returns 0 instead of
 * blocking, so a depopulated part never wedges the render loop.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the battery ADC on `adc_gpio` and drive `enable_gpio` HIGH.
 * Pass enable_gpio < 0 if the divider is hardwired. Safe to call once at boot;
 * logs and continues if the ADC cannot be configured. */
void board_io_init(int adc_gpio, int enable_gpio);

/* Battery terminal voltage in volts (after undoing the 1:3 divider).
 * <= 0 on error or when no battery is fitted. */
float board_io_battery_voltage(void);

/* Battery charge estimate, 0..100, mapped from ~3.0V (empty) to ~4.12V (full).
 * 0 when the reading is not trustworthy — callers should treat 0 as "hide the
 * battery chip", not as "flat". */
int board_io_battery_percent(void);

/* Whether the last reading looked like a real cell rather than an open input.
 * USB-only operation with no battery fitted lands here. */
bool board_io_battery_present(void);

#ifdef __cplusplus
}
#endif
