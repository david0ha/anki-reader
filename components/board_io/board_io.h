/*
 * board_io.h — the board's clock and power sensing.
 *
 * One shared I2C master bus (SDA=13, SCL=14) drives the PCF85063A RTC (0x51);
 * the battery voltage is read on ADC1 channel 3 (GPIO4) through the board's
 * 1:3 divider.
 *
 * The RTC is kept in UTC: seed the system clock from it at boot (so the screen
 * shows the right time even before SNTP, and across brief power loss), and write
 * the SNTP-synced time back into it once online. Display code applies the local
 * timezone via the C library (TZ + localtime_r).
 *
 * Every getter is defensive: if a device is missing or NAKs, it returns
 * false / 0 instead of blocking the caller, so a depopulated sensor never
 * wedges the render loop.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the I2C bus, the PCF85063A and the battery ADC. Safe to call once at
 * boot; logs and continues if either device is absent. */
void board_io_init(void);

/* Read the RTC as a UTC struct tm. Returns false if the oscillator-stop flag is
 * set or the year looks unset (clock never written) — i.e. the time is not
 * trustworthy and the caller should fall back to SNTP. */
bool board_io_rtc_get(struct tm *out_utc);

/* Write a UTC struct tm into the RTC (clears the oscillator-stop flag). */
bool board_io_rtc_set(const struct tm *utc);

/* Battery terminal voltage in volts (after the 1:3 divider). <= 0 on error. */
float board_io_battery_voltage(void);

/* Battery charge estimate, 0..100, mapped from ~3.0V (empty) to ~4.12V (full). */
int board_io_battery_percent(void);

#ifdef __cplusplus
}
#endif
