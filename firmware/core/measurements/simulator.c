/*
 * Synthetic measurements. See simulator.h for what is modelled and why.
 *
 * Deliberately free of randomness. A fault that reproduces only sometimes is
 * a fault that cannot be used in a regression test, and a bench whose test
 * suite is flaky teaches its owner to ignore red.
 */
#include "dronebench/simulator.h"

#include <string.h>

/* Timeline, in milliseconds from the start of the session. */
#define SETTLE_MS     1000
#define MOTOR_MS      2000  /* each motor spins for this long ... */
#define BETWEEN_MS     500  /* ... with this much quiet after it */
#define TAIL_MS       1000

#define MOTOR_COUNT      4
#define MOTOR_SLOT_MS   (MOTOR_MS + BETWEEN_MS)

/* Plausible for a 1S Meteor75 Pro. Not measurements — see the header. */
#define PACK_OPEN_V        4.20f
#define PACK_INTERNAL_OHM  0.060f
#define IDLE_A             0.38f
#define MOTOR_A            1.60f
#define MOTOR_HIGH_A       2.60f
#define SHORT_A           22.00f

/* How long a motor takes to reach its steady draw. Real ones do not step. */
#define SPINUP_MS 150

static uint32_t elapsed_ms(const simulator_t *sim, uint64_t now_us)
{
    if (!sim->started || now_us <= sim->start_us) {
        return 0;
    }
    return (uint32_t)((now_us - sim->start_us) / 1000u);
}

/* Which motor is running at t, or 0 for none. */
static int active_motor(uint32_t t_ms, uint32_t *into_slot_ms)
{
    uint32_t t;
    int      index;

    if (t_ms < SETTLE_MS) {
        return 0;
    }

    t = t_ms - SETTLE_MS;
    index = (int)(t / MOTOR_SLOT_MS);

    if (index >= MOTOR_COUNT) {
        return 0;
    }

    *into_slot_ms = t % MOTOR_SLOT_MS;
    if (*into_slot_ms >= MOTOR_MS) {
        return 0; /* the quiet gap after this motor */
    }
    return index + 1;
}

/* Linear ramp to full draw, so the current profile has an edge to measure. */
static float spinup_scale(uint32_t into_slot_ms)
{
    if (into_slot_ms >= SPINUP_MS) {
        return 1.0f;
    }
    return (float)into_slot_ms / (float)SPINUP_MS;
}

static float motor_draw(sim_profile_t profile, int motor)
{
    switch (profile) {
    case SIM_PROFILE_MOTOR4_FAIL:
        /* Nothing at all: no spin-up edge, no steady draw. The diagnostic
           rules have to notice an absence, which is harder than noticing an
           excess. */
        return (motor == 4) ? 0.0f : MOTOR_A;

    case SIM_PROFILE_MOTOR2_HIGH:
        return (motor == 2) ? MOTOR_HIGH_A : MOTOR_A;

    case SIM_PROFILE_NORMAL:
    case SIM_PROFILE_SAG:
    case SIM_PROFILE_SHORT:
        return MOTOR_A;
    }
    return MOTOR_A;
}

void simulator_init(simulator_t *sim, sim_profile_t profile)
{
    *sim = (simulator_t){ .profile = profile };
}

void simulator_read(simulator_t *sim, uint64_t now_us, float *voltage_v,
                    float *current_a)
{
    uint32_t t_ms;
    uint32_t into_slot_ms = 0;
    int      motor;
    float    current;
    float    resistance = PACK_INTERNAL_OHM;

    if (!sim->started) {
        sim->start_us = now_us;
        sim->started = true;
    }
    t_ms = elapsed_ms(sim, now_us);

    if (sim->profile == SIM_PROFILE_SHORT) {
        /* A short does not wait for the test sequence. */
        *current_a = SHORT_A;
        *voltage_v = PACK_OPEN_V - SHORT_A * PACK_INTERNAL_OHM;
        return;
    }

    current = IDLE_A;
    motor = active_motor(t_ms, &into_slot_ms);
    if (motor != 0) {
        current += motor_draw(sim->profile, motor) * spinup_scale(into_slot_ms);
    }

    if (sim->profile == SIM_PROFILE_SAG) {
        /* A tired pack holds its open-circuit voltage and collapses under
           load — which is exactly why sag has to be measured while drawing
           current, not at rest. */
        resistance = PACK_INTERNAL_OHM * 5.0f;
    }

    *current_a = current;
    *voltage_v = PACK_OPEN_V - current * resistance;
}

uint64_t simulator_duration_us(void)
{
    return ((uint64_t)SETTLE_MS + (uint64_t)MOTOR_COUNT * MOTOR_SLOT_MS +
            TAIL_MS) *
           1000u;
}

static const struct {
    const char   *name;
    sim_profile_t profile;
} PROFILES[] = {
    { "normal", SIM_PROFILE_NORMAL },
    { "motor4_fail", SIM_PROFILE_MOTOR4_FAIL },
    { "motor2_high", SIM_PROFILE_MOTOR2_HIGH },
    { "sag", SIM_PROFILE_SAG },
    { "short", SIM_PROFILE_SHORT },
};

bool simulator_profile_from_name(const char *name, sim_profile_t *out)
{
    for (size_t i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++) {
        if (strcmp(name, PROFILES[i].name) == 0) {
            *out = PROFILES[i].profile;
            return true;
        }
    }
    return false;
}

const char *simulator_profile_name(sim_profile_t profile)
{
    for (size_t i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++) {
        if (PROFILES[i].profile == profile) {
            return PROFILES[i].name;
        }
    }
    return "unknown";
}
