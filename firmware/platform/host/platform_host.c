#include "platform_host.h"

#include <string.h>

#include "dronebench/platform.h"

#define HOST_OUTPUT_MAX 4096

static struct {
    uint64_t now_us;

    float voltage_v;
    float current_a;
    float ref_current_a;

    bool voltage_ok;
    bool current_ok;
    bool ref_ok;

    char   output[HOST_OUTPUT_MAX];
    size_t output_len;

    bool     led;
    uint32_t watchdog_feeds;
} g_host;

void platform_host_reset(void)
{
    memset(&g_host, 0, sizeof g_host);
    g_host.voltage_ok = true;
    g_host.current_ok = true;
    g_host.ref_ok = true;
}

void platform_host_set_time_us(uint64_t now_us)
{
    g_host.now_us = now_us;
}

void platform_host_advance_us(uint64_t delta_us)
{
    g_host.now_us += delta_us;
}

void platform_host_set_readings(float voltage_v, float current_a,
                                float ref_current_a)
{
    g_host.voltage_v = voltage_v;
    g_host.current_a = current_a;
    g_host.ref_current_a = ref_current_a;
}

void platform_host_set_sensor_ok(bool voltage_ok, bool current_ok, bool ref_ok)
{
    g_host.voltage_ok = voltage_ok;
    g_host.current_ok = current_ok;
    g_host.ref_ok = ref_ok;
}

const char *platform_host_output(void)
{
    return g_host.output;
}

bool platform_host_led(void)
{
    return g_host.led;
}

uint32_t platform_host_watchdog_feeds(void)
{
    return g_host.watchdog_feeds;
}

/* --- platform.h ----------------------------------------------------------- */

uint64_t platform_time_us(void)
{
    return g_host.now_us;
}

bool platform_adc_read_voltage(float *value)
{
    if (!g_host.voltage_ok) {
        return false;
    }
    *value = g_host.voltage_v;
    return true;
}

bool platform_adc_read_current(float *value)
{
    if (!g_host.current_ok) {
        return false;
    }
    *value = g_host.current_a;
    return true;
}

bool platform_ref_read_current(float *value)
{
    if (!g_host.ref_ok) {
        return false;
    }
    *value = g_host.ref_current_a;
    return true;
}

void platform_uart_write(const char *data, size_t size)
{
    /* Silently drops what does not fit rather than growing: the target has a
       fixed buffer too, and a test that can capture unbounded output would
       hide the day the real one overflows. */
    if (g_host.output_len + size + 1 > HOST_OUTPUT_MAX) {
        return;
    }
    memcpy(g_host.output + g_host.output_len, data, size);
    g_host.output_len += size;
    g_host.output[g_host.output_len] = '\0';
}

void platform_watchdog_feed(void)
{
    g_host.watchdog_feeds++;
}

void platform_status_led_set(bool on)
{
    g_host.led = on;
}
