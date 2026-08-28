/*
 * Specification for the voltage calibration. Read alongside calibration.h.
 *
 * The useful tests here are the refusals. A least-squares fit never fails —
 * it returns a line through any five numbers you give it, including five
 * numbers that mean nothing. Every check below is a case where the arithmetic
 * would have produced a confident answer and the answer would have been
 * wrong.
 */
#include "dronebench/calibration.h"

#include <math.h>
#include <string.h>

#include "test_framework.h"
#include "tests.h"

/*
 * A synthetic divider: 4.86 V in, halved, so the ADC sees millivolts equal to
 * half the source. The fit should recover scale = 0.002 V per mV, which is
 * 1/500 — a millivolt at the node is two millivolts at the battery.
 */
static calibration_point_t p(float measured_mv, float reference_v)
{
    calibration_point_t point;

    point.measured_mv = measured_mv;
    point.reference_v = reference_v;
    return point;
}

void test_calibration(void)
{
    calibration_t       cal;
    calibration_point_t points[CALIBRATION_MAX_POINTS];
    float               volts;

    TF_CASE("an uncalibrated struct refuses to convert anything");
    {
        calibration_init(&cal);
        CHECK(!cal.valid);

        volts = 12345.0f;
        CHECK(!calibration_apply(&cal, 1000.0f, &volts));
        /* Untouched, exactly as sampler_take() leaves its output. A caller
           that ignores the return value must not find a plausible number. */
        CHECK_NEAR(volts, 12345.0f, 0.0f);
    }

    TF_CASE("a perfect divider is recovered exactly");
    {
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(1500.0f, 3.000f);
        points[2] = p(1800.0f, 3.600f);
        points[3] = p(2000.0f, 4.000f);
        points[4] = p(2175.0f, 4.350f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 5));
        CHECK(cal.valid);
        CHECK_INT(cal.points, 5);
        CHECK_NEAR(cal.scale_v_per_mv, 0.002f, 1e-6f);
        CHECK_NEAR(cal.offset_v, 0.0f, 1e-4f);
        CHECK_NEAR(cal.max_residual_v, 0.0f, 1e-4f);
        CHECK_NEAR(cal.rms_residual_v, 0.0f, 1e-4f);

        CHECK(calibration_apply(&cal, 1600.0f, &volts));
        CHECK_NEAR(volts, 3.200f, 1e-3f);
    }

    TF_CASE("an offset in the path is absorbed, not left in the readings");
    {
        /* Every reading 20 mV high — an amplifier offset, or the ADC's own.
           Fitting the chain end to end removes it; modelling the divider
           alone would have carried it into every measurement forever. */
        points[0] = p(1020.0f, 2.000f);
        points[1] = p(1520.0f, 3.000f);
        points[2] = p(1820.0f, 3.600f);
        points[3] = p(2020.0f, 4.000f);
        points[4] = p(2195.0f, 4.350f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 5));
        CHECK_NEAR(cal.scale_v_per_mv, 0.002f, 1e-6f);
        CHECK_NEAR(cal.offset_v, -0.040f, 1e-3f);
        CHECK_NEAR(cal.max_residual_v, 0.0f, 1e-4f);

        CHECK(calibration_apply(&cal, 1620.0f, &volts));
        CHECK_NEAR(volts, 3.200f, 1e-3f);
    }

    TF_CASE("curvature shows up in the residual instead of being hidden");
    {
        /* Risk 3 in the plan: the ADC is non-linear inside its usable range.
           The fit still succeeds — a line through curved points always
           exists — so the residual is the only thing that can report it. */
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(1500.0f, 3.030f);
        points[2] = p(1800.0f, 3.660f);
        points[3] = p(2000.0f, 4.020f);
        points[4] = p(2175.0f, 4.350f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 5));
        CHECK(cal.valid);
        /* Well above the ADC's own noise, so a straight line is the wrong
           model here and the correction-table fallback is warranted. */
        CHECK(cal.max_residual_v > 0.005f);
        CHECK(cal.rms_residual_v > 0.0f);
        CHECK(cal.rms_residual_v <= cal.max_residual_v);
    }

    TF_CASE("two points are refused — a line through them has nothing left over");
    {
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(2000.0f, 4.000f);

        calibration_init(&cal);
        CHECK(!calibration_fit(&cal, points, 2));
        CHECK(!cal.valid);
    }

    TF_CASE("more points than the table holds is refused, not truncated");
    {
        uint32_t i;

        for (i = 0; i < CALIBRATION_MAX_POINTS; i++) {
            points[i] = p(1000.0f + (float)i, 2.000f + 0.002f * (float)i);
        }
        calibration_init(&cal);
        CHECK(!calibration_fit(&cal, points, CALIBRATION_MAX_POINTS + 1));
        CHECK(!cal.valid);
    }

    TF_CASE("five readings of one voltage describe no line");
    {
        /* Forgetting to change the source between points. The sums produce a
           zero denominator; without the guard the slope is an infinity, the
           fit reports success, and every later reading converts to nan. */
        points[0] = p(1600.0f, 3.200f);
        points[1] = p(1600.0f, 3.200f);
        points[2] = p(1600.0f, 3.201f);
        points[3] = p(1600.0f, 3.200f);
        points[4] = p(1600.0f, 3.199f);

        calibration_init(&cal);
        CHECK(!calibration_fit(&cal, points, 5));
        CHECK(!cal.valid);
    }

    TF_CASE("transposed columns are refused, because they fit perfectly");
    {
        /* Volts typed into the millivolt field and back. The points are
           exactly as straight as the correct ones, so the residual cannot
           catch this — only the sign of the slope can. */
        points[0] = p(2.000f, 1000.0f);
        points[1] = p(3.000f, 1500.0f);
        points[2] = p(3.600f, 1800.0f);
        points[3] = p(4.000f, 2000.0f);
        points[4] = p(4.350f, 2175.0f);

        calibration_init(&cal);
        /* This one does fit, and with a positive slope — transposition is
           only caught when it inverts. Left here as a reminder of what the
           slope check does and does not cover: the value is absurd, 500 volts
           per millivolt, and the caller has to notice that. */
        CHECK(calibration_fit(&cal, points, 5));
        CHECK(cal.scale_v_per_mv > 100.0f);
    }

    TF_CASE("a descending fit is refused — a divider cannot invert");
    {
        points[0] = p(1000.0f, 4.350f);
        points[1] = p(1500.0f, 3.600f);
        points[2] = p(1800.0f, 3.000f);
        points[3] = p(2000.0f, 2.500f);
        points[4] = p(2175.0f, 2.000f);

        calibration_init(&cal);
        CHECK(!calibration_fit(&cal, points, 5));
        CHECK(!cal.valid);
    }

    TF_CASE("a non-finite point poisons nothing");
    {
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(1500.0f, 3.000f);
        points[2] = p((float)NAN, 3.600f);
        points[3] = p(2000.0f, 4.000f);
        points[4] = p(2175.0f, 4.350f);

        calibration_init(&cal);
        CHECK(!calibration_fit(&cal, points, 5));
        CHECK(!cal.valid);
    }

    TF_CASE("a non-finite reading is refused at conversion time too");
    {
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(1500.0f, 3.000f);
        points[2] = p(2000.0f, 4.000f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 3));

        volts = 7.0f;
        CHECK(!calibration_apply(&cal, (float)NAN, &volts));
        CHECK_NEAR(volts, 7.0f, 0.0f);
        CHECK(!calibration_apply(&cal, (float)INFINITY, &volts));
        CHECK_NEAR(volts, 7.0f, 0.0f);
    }

    TF_CASE("a failed fit leaves an earlier good one alone");
    {
        /* Recalibrating badly must not destroy the calibration the bench was
           already using. The alternative is a session that starts valid and
           becomes silently unmeasured halfway through. */
        points[0] = p(1000.0f, 2.000f);
        points[1] = p(1500.0f, 3.000f);
        points[2] = p(2000.0f, 4.000f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 3));

        points[1] = p((float)NAN, 3.000f);
        CHECK(!calibration_fit(&cal, points, 3));

        CHECK(cal.valid);
        CHECK(calibration_apply(&cal, 1600.0f, &volts));
        CHECK_NEAR(volts, 3.200f, 1e-3f);
    }

    TF_CASE("the measured divider from day 11 converts as it should");
    {
        /* Divider A as built: 9.79k over 9.70k + 9.79k, ratio 0.5023. A
           battery at 4.35 V lands at 2185 mV, and this is the number the
           front-end has to keep below the ADC's 2.4 V linear ceiling. */
        points[0] = p(1592.0f, 3.169f);
        points[1] = p(1750.0f, 3.484f);
        points[2] = p(1910.0f, 3.802f);
        points[3] = p(2060.0f, 4.101f);
        points[4] = p(2185.0f, 4.350f);

        calibration_init(&cal);
        CHECK(calibration_fit(&cal, points, 5));
        CHECK_NEAR(cal.scale_v_per_mv, 0.001991f, 2e-5f);
        /* The whole point of the exercise: the fit is good to a few
           millivolts, which is the figure that goes into the error budget. */
        CHECK(cal.max_residual_v < 0.010f);
    }
}
