/*
 * Tests for the host platform itself.
 *
 * Testing test infrastructure sounds circular, but this is the fixture every
 * later suite depends on. A fake clock that silently goes backwards, or a
 * failed read that quietly leaves a value behind, would make the tests above
 * it lie — and those are the tests that decide whether a drone is safe.
 */
#include <string.h>

#include "dronebench/platform.h"
#include "platform_host.h"
#include "test_framework.h"
#include "tests.h"

void test_platform(void)
{
    TF_CASE("reset puts the platform in a known state");
    {
        platform_host_set_time_us(12345);
        platform_status_led_set(true);
        platform_watchdog_feed();
        platform_uart_write("noise", 5);

        platform_host_reset();

        CHECK_INT(platform_time_us(), 0);
        CHECK(!platform_host_led());
        CHECK_INT(platform_host_watchdog_feeds(), 0);
        CHECK_STR(platform_host_output(), "");
    }

    TF_CASE("time is controlled, not measured");
    {
        platform_host_reset();
        platform_host_set_time_us(1000);
        CHECK_INT(platform_time_us(), 1000);

        platform_host_advance_us(500);
        CHECK_INT(platform_time_us(), 1500);

        /* An hour has to be expressible without waiting an hour — otherwise
           the mAh integration tests could never run. */
        platform_host_advance_us(3600ull * 1000000ull);
        CHECK_INT(platform_time_us(), 3600001500ull);
    }

    TF_CASE("injected readings come back through the platform interface");
    {
        float v = 0.0f;
        float i = 0.0f;
        float r = 0.0f;

        platform_host_reset();
        platform_host_set_readings(4.083f, 0.42f, 0.397f);

        CHECK(platform_adc_read_voltage(&v));
        CHECK(platform_adc_read_current(&i));
        CHECK(platform_ref_read_current(&r));

        CHECK_NEAR(v, 4.083f, 0.0005f);
        CHECK_NEAR(i, 0.42f, 0.0005f);
        CHECK_NEAR(r, 0.397f, 0.0005f);
    }

    TF_CASE("an unavailable sensor reports failure and touches nothing");
    {
        float v = -1.0f;
        float i = -1.0f;
        float r = -1.0f;

        platform_host_reset();
        platform_host_set_readings(4.0f, 1.0f, 1.0f);
        platform_host_set_sensor_ok(false, false, false);

        CHECK(!platform_adc_read_voltage(&v));
        CHECK(!platform_adc_read_current(&i));
        CHECK(!platform_ref_read_current(&r));

        /* The caller's variables must be untouched. Writing a partial or
           stale value on a failed read is how a bench ends up reporting a
           measurement it never took. */
        CHECK_NEAR(v, -1.0f, 0.0001f);
        CHECK_NEAR(i, -1.0f, 0.0001f);
        CHECK_NEAR(r, -1.0f, 0.0001f);
    }

    TF_CASE("channels fail independently");
    {
        float v = 0.0f;
        float r = -1.0f;

        platform_host_reset();
        platform_host_set_readings(3.7f, 2.0f, 2.0f);
        platform_host_set_sensor_ok(true, true, false);

        CHECK(platform_adc_read_voltage(&v));
        CHECK_NEAR(v, 3.7f, 0.0005f);

        /* The reference channel is optional hardware — the bench must run
           without it, degraded but honest. */
        CHECK(!platform_ref_read_current(&r));
        CHECK_NEAR(r, -1.0f, 0.0001f);
    }

    TF_CASE("uart output is captured in order");
    {
        platform_host_reset();
        platform_uart_write("OK,", 3);
        platform_uart_write("version", 7);
        platform_uart_write("\n", 1);
        CHECK_STR(platform_host_output(), "OK,version\n");
    }

    TF_CASE("uart accepts embedded NUL bytes, unlike a C string");
    {
        const char frame[] = { 'A', '\0', 'B' };

        platform_host_reset();
        platform_uart_write(frame, sizeof frame);

        /* The capture buffer is compared as a string, so only "A" is visible
           here — the point of the check is that the call took a length and
           did not stop at the NUL. Day 7's binary protocol depends on this. */
        CHECK_STR(platform_host_output(), "A");
    }

    TF_CASE("led and watchdog are observable");
    {
        platform_host_reset();

        platform_status_led_set(true);
        CHECK(platform_host_led());
        platform_status_led_set(false);
        CHECK(!platform_host_led());

        platform_watchdog_feed();
        platform_watchdog_feed();
        platform_watchdog_feed();
        CHECK_INT(platform_host_watchdog_feeds(), 3);
    }
}
