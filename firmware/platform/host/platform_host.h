/*
 * Host implementation of the platform interface, plus the knobs a test needs
 * to drive it.
 *
 * Time is controlled, not measured. A test that computes consumed mAh over an
 * hour must not take an hour, and a test that depends on wall-clock timing is
 * a test that fails on a loaded CI machine for no reason.
 *
 * Sensor readings are injected. That is what makes the whole measurement
 * pipeline — sampling, integration, thresholds, verdicts — testable before a
 * single resistor has been soldered.
 */
#ifndef PLATFORM_HOST_H
#define PLATFORM_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Back to a known state: time 0, all readings 0 and available, no captured
   output, LED off, watchdog counter cleared. Call at the start of every test. */
void platform_host_reset(void);

void platform_host_set_time_us(uint64_t now_us);
void platform_host_advance_us(uint64_t delta_us);

void platform_host_set_readings(float voltage_v, float current_a,
                                float ref_current_a);

/* Marks channels as unavailable, so the core's failure paths can be exercised.
   An unavailable read returns false and must leave the caller's value alone. */
void platform_host_set_sensor_ok(bool voltage_ok, bool current_ok,
                                 bool ref_ok);

/* Everything platform_uart_write() has emitted since the last reset,
   concatenated. Never NULL. */
const char *platform_host_output(void);

bool     platform_host_led(void);
uint32_t platform_host_watchdog_feeds(void);

#endif /* PLATFORM_HOST_H */
