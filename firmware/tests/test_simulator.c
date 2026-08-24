/*
 * Specification for the simulated profiles. Read alongside simulator.h.
 *
 * These tests pin down what each fault looks like, which is what makes the
 * diagnostic rules of days 19 and 20 testable: the right answer is known
 * because it was chosen here.
 */
#include "dronebench/simulator.h"

#include "test_framework.h"
#include "tests.h"

#define MS 1000u /* microseconds per millisecond */

/* Reading at t milliseconds into the session. */
static void at(simulator_t *sim, uint32_t t_ms, float *v, float *i)
{
    simulator_read(sim, (uint64_t)t_ms * MS, v, i);
}

/* Peak current while the given motor is at full speed. */
static float motor_peak(sim_profile_t profile, int motor)
{
    simulator_t sim;
    float       v;
    float       i;
    uint32_t    t_ms;

    simulator_init(&sim, profile);
    at(&sim, 0, &v, &i); /* anchor the session start */

    /* 1000 ms settle, then 2500 ms per motor slot; sample well past spin-up. */
    t_ms = 1000u + (uint32_t)(motor - 1) * 2500u + 1500u;
    at(&sim, t_ms, &v, &i);
    return i;
}

void test_simulator(void)
{
    simulator_t sim;
    float       v;
    float       i;

    TF_CASE("profile names round-trip");
    {
        sim_profile_t p;

        CHECK(simulator_profile_from_name("motor4_fail", &p));
        CHECK_INT(p, SIM_PROFILE_MOTOR4_FAIL);
        CHECK_STR(simulator_profile_name(p), "motor4_fail");

        CHECK(!simulator_profile_from_name("nonsense", &p));
    }

    TF_CASE("a settled bench draws only the flight controller's idle current");
    {
        simulator_init(&sim, SIM_PROFILE_NORMAL);
        at(&sim, 0, &v, &i);
        at(&sim, 500, &v, &i);

        CHECK_NEAR(i, 0.38f, 0.01f);
        CHECK(v > 4.1f); /* barely loaded, so barely sagging */
    }

    TF_CASE("each motor in turn adds its draw");
    {
        for (int motor = 1; motor <= 4; motor++) {
            CHECK_NEAR(motor_peak(SIM_PROFILE_NORMAL, motor), 0.38f + 1.60f,
                       0.02f);
        }
    }

    TF_CASE("motor 4 failing shows as an absence, not an excess");
    {
        /* The rule under test on day 19 has to notice that nothing happened,
           which is harder than noticing that too much did. */
        CHECK_NEAR(motor_peak(SIM_PROFILE_MOTOR4_FAIL, 1), 1.98f, 0.02f);
        CHECK_NEAR(motor_peak(SIM_PROFILE_MOTOR4_FAIL, 4), 0.38f, 0.02f);
    }

    TF_CASE("a high motor stands out against its peers");
    {
        float peer = motor_peak(SIM_PROFILE_MOTOR2_HIGH, 1);
        float bad = motor_peak(SIM_PROFILE_MOTOR2_HIGH, 2);

        CHECK(bad > peer * 1.4f);
    }

    TF_CASE("current ramps rather than steps");
    {
        float early;
        float late;

        simulator_init(&sim, SIM_PROFILE_NORMAL);
        at(&sim, 0, &v, &i);

        at(&sim, 1050, &v, &early); /* 50 ms into a 150 ms spin-up */
        at(&sim, 1200, &v, &late);

        /* A real motor has an edge with a shape; a step function would let a
           detector pass that could never work on hardware. */
        CHECK(early > 0.38f);
        CHECK(early < late);
    }

    TF_CASE("voltage sags in proportion to current");
    {
        float idle_v;
        float loaded_v;

        simulator_init(&sim, SIM_PROFILE_NORMAL);
        at(&sim, 0, &v, &i);
        at(&sim, 500, &idle_v, &i);
        at(&sim, 2500, &loaded_v, &i);

        CHECK(loaded_v < idle_v);
    }

    TF_CASE("a tired pack sags far harder under the same load");
    {
        simulator_t healthy;
        float       healthy_v;
        float       tired_v;

        simulator_init(&healthy, SIM_PROFILE_NORMAL);
        at(&healthy, 0, &v, &i);
        at(&healthy, 2500, &healthy_v, &i);

        simulator_init(&sim, SIM_PROFILE_SAG);
        at(&sim, 0, &v, &i);
        at(&sim, 2500, &tired_v, &i);

        /* Same current, different pack. This is why sag has to be measured
           under load — at rest the two are indistinguishable. */
        CHECK(tired_v < healthy_v - 0.15f);
    }

    TF_CASE("a short draws immediately, without waiting for the sequence");
    {
        simulator_init(&sim, SIM_PROFILE_SHORT);
        at(&sim, 0, &v, &i);

        CHECK(i > 20.0f);
        CHECK(v < 3.0f);
    }

    TF_CASE("the session has a finite scripted length");
    {
        /* 1 s settle + 4 x 2.5 s + 1 s tail */
        CHECK_INT(simulator_duration_us(), 12000000);
    }

    TF_CASE("the same profile replays identically");
    {
        simulator_t a;
        simulator_t b;
        float       va;
        float       ia;
        float       vb;
        float       ib;

        /* No randomness anywhere: a fault that reproduces only sometimes
           cannot be used in a regression test. */
        simulator_init(&a, SIM_PROFILE_MOTOR4_FAIL);
        simulator_init(&b, SIM_PROFILE_MOTOR4_FAIL);

        for (uint32_t t = 0; t < 8000; t += 137) {
            at(&a, t, &va, &ia);
            at(&b, t, &vb, &ib);
            CHECK_NEAR(va, vb, 1e-6f);
            CHECK_NEAR(ia, ib, 1e-6f);
        }
    }
}
