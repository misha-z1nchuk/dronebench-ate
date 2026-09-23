/*
 * Specification for the ACS724 conversion. Read alongside current_sensor.h.
 *
 * The numbers are day 11's: VCC 4.80 V at the module, OUT 2.40 V with no
 * current, dividers 0.5008 (OUT) and 0.3323 (VCC) measured in circuit.
 */
#include "dronebench/current_sensor.h"

#include <math.h>

#include "test_framework.h"
#include "tests.h"

#define OUT_DIV 0.5008f
#define VCC_DIV 0.3323f

/* Pin volts to node millivolts, through a divider. */
static float node_mv(float pin_v, float divider)
{
    return pin_v * divider * 1000.0f;
}

/* What an ideal ACS724-50AB outputs at this supply and current. */
static float acs_out_v(float vcc_v, float amps)
{
    return vcc_v * (0.5f + 0.008f * amps);
}

static void make_sensor(current_sensor_t *s)
{
    current_sensor_init(s, OUT_DIV, VCC_DIV,
                        CURRENT_SENSOR_ACS724_50AB_AMPS_PER_RATIO);
}

void test_current_sensor(void)
{
    current_sensor_t s;
    float            r;
    float            amps;

    TF_CASE("an unzeroed sensor refuses to report current");
    {
        make_sensor(&s);
        amps = 777.0f;
        CHECK(!current_sensor_amps(&s, node_mv(2.40f, OUT_DIV),
                                   node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 777.0f, 0.0f);
    }

    TF_CASE("day 11's no-current reading is a ratio of one half");
    {
        make_sensor(&s);
        CHECK(current_sensor_ratio(&s, node_mv(2.40f, OUT_DIV),
                                   node_mv(4.80f, VCC_DIV), &r));
        CHECK_NEAR(r, 0.5f, 1e-5f);
        CHECK(current_sensor_set_zero(&s, r));
        CHECK(current_sensor_amps(&s, node_mv(2.40f, OUT_DIV),
                                  node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 0.0f, 1e-3f);
    }

    TF_CASE("the zero follows VCC — the whole reason for the third channel");
    {
        /*
         * Zeroed at 4.80 V, then USB rises to 5.10 V with still no current.
         * OUT follows to 2.55 V. A zero stored as 2.40 V would read that as
         * 0.15 V / 38.4 mV/A = 3.9 A of current that is not there.
         */
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5f));
        CHECK(current_sensor_amps(&s, node_mv(acs_out_v(5.10f, 0.0f), OUT_DIV),
                                  node_mv(5.10f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 0.0f, 1e-3f);
    }

    TF_CASE("so does the sensitivity: 1 A reads 1 A at any supply");
    {
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5f));

        CHECK(current_sensor_amps(&s, node_mv(acs_out_v(4.80f, 1.0f), OUT_DIV),
                                  node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 1.0f, 1e-3f);

        CHECK(current_sensor_amps(&s, node_mv(acs_out_v(5.20f, 1.0f), OUT_DIV),
                                  node_mv(5.20f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 1.0f, 1e-3f);
    }

    TF_CASE("bidirectional: current the wrong way round is negative, not lost");
    {
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5f));
        CHECK(current_sensor_amps(&s, node_mv(acs_out_v(4.80f, -2.0f), OUT_DIV),
                                  node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, -2.0f, 1e-3f);
    }

    TF_CASE("a wrong divider costs gain, never offset, once zeroed through it");
    {
        /*
         * The real OUT divider is 2 % higher than the one configured. The
         * zero, measured through the same wrong number, absorbs the offset:
         * no current still reads zero. What is left is a 2 % gain error.
         */
        const float real_out_div = OUT_DIV * 1.02f;

        make_sensor(&s);
        CHECK(current_sensor_ratio(&s, node_mv(2.40f, real_out_div),
                                   node_mv(4.80f, VCC_DIV), &r));
        CHECK(current_sensor_set_zero(&s, r));

        CHECK(current_sensor_amps(&s, node_mv(2.40f, real_out_div),
                                  node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 0.0f, 1e-3f);

        CHECK(current_sensor_amps(&s, node_mv(acs_out_v(4.80f, 5.0f), real_out_div),
                                  node_mv(4.80f, VCC_DIV), &amps));
        CHECK_NEAR(amps, 5.0f * 1.02f, 2e-3f);
    }

    TF_CASE("a VCC wire that is off is refused, not divided by");
    {
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5f));
        amps = 777.0f;
        CHECK(!current_sensor_amps(&s, node_mv(2.40f, OUT_DIV), 0.0f, &amps));
        CHECK(!current_sensor_amps(&s, node_mv(2.40f, OUT_DIV), 3.0f, &amps));
        CHECK_NEAR(amps, 777.0f, 0.0f);
    }

    TF_CASE("VCC outside the sensor's supply range is refused");
    {
        make_sensor(&s);
        CHECK(!current_sensor_ratio(&s, node_mv(2.10f, OUT_DIV),
                                    node_mv(4.20f, VCC_DIV), &r));
        CHECK(!current_sensor_ratio(&s, node_mv(2.90f, OUT_DIV),
                                    node_mv(5.80f, VCC_DIV), &r));
    }

    TF_CASE("an output at the clip is 'at least this much', so it is refused");
    {
        make_sensor(&s);
        CHECK(!current_sensor_ratio(&s, node_mv(4.80f * 0.95f, OUT_DIV),
                                    node_mv(4.80f, VCC_DIV), &r));
        CHECK(!current_sensor_ratio(&s, node_mv(4.80f * 0.05f, OUT_DIV),
                                    node_mv(4.80f, VCC_DIV), &r));
    }

    TF_CASE("a zero taken under load is refused and the old one kept");
    {
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5010f));
        /* 10 A flowing while zeroing: r = 0.58. */
        CHECK(!current_sensor_set_zero(&s, 0.58f));
        CHECK(!current_sensor_set_zero(&s, NAN));
        CHECK_NEAR(s.zero_ratio, 0.5010f, 0.0f);
        CHECK(s.zeroed);
    }

    TF_CASE("non-finite readings are refused");
    {
        make_sensor(&s);
        CHECK(current_sensor_set_zero(&s, 0.5f));
        CHECK(!current_sensor_amps(&s, NAN, node_mv(4.80f, VCC_DIV), &amps));
        CHECK(!current_sensor_amps(&s, node_mv(2.40f, OUT_DIV), INFINITY, &amps));
    }
}
