/*
 * Platform interface — the only way the core reaches hardware.
 *
 * The core calls these; each platform implements them:
 *
 *   firmware/platform/host/    fakes, used by the unit tests
 *   firmware/platform/esp32/   ESP-IDF
 *   firmware/platform/stm32/   STM32 HAL
 *
 * Rule: no file under firmware/core/ may include a vendor SDK header. If the
 * core needs something the platform can do, it goes here first. That rule is
 * what makes the same core run on both MCUs and under `make test`.
 *
 * All read functions return false on failure and leave *value untouched, so a
 * caller that ignores the return value gets a stale reading rather than
 * garbage. Callers are still expected to check.
 */
#ifndef DRONEBENCH_PLATFORM_H
#define DRONEBENCH_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Monotonic microseconds since boot. Must never go backwards. */
uint64_t platform_time_us(void);

/* Battery voltage in volts, already scaled for the divider and calibrated. */
bool platform_adc_read_voltage(float *value);

/*
 * The same channel before calibration: millivolts at the ADC pin, after
 * multisampling and whatever conversion the vendor's reference correction
 * applies, and nothing else.
 *
 * This exists so a calibration can be collected at all. calibration_fit()
 * needs pairs of "what the ADC said" and "what the meter said", and the first
 * half of every pair has to come out of the firmware in a form that has not
 * already been through the fit being measured. Reading it back through
 * platform_adc_read_voltage() would be circular.
 *
 * Not for measurement. Nothing outside the calibration command should call
 * it: the number is real but it is not volts at the battery, and it will be
 * off by the divider ratio — a factor of two, which looks entirely plausible
 * on a 1S pack.
 */
bool platform_adc_read_millivolts(float *value);

/* Current in amperes from the analog front-end (ACS724 + ADC). */
bool platform_adc_read_current(float *value);

/* Current in amperes from the independent I2C reference (INA226). */
bool platform_ref_read_current(float *value);

/* Writes size bytes to the telemetry/CLI transport. Blocking. */
void platform_uart_write(const char *data, size_t size);

/* Kicks the hardware watchdog. */
void platform_watchdog_feed(void);

void platform_status_led_set(bool on);

#endif /* DRONEBENCH_PLATFORM_H */
