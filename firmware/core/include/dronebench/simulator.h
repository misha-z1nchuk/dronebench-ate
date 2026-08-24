/*
 * Synthetic measurements.
 *
 * Not scaffolding for the missing ADC: this is how a fault is reproduced
 * without breaking hardware. A bench that classifies a dead motor correctly
 * has to be shown a dead motor, and the alternative to simulating one is
 * destroying one per test run.
 *
 * The profiles come from plan section 8 — `simulate normal`,
 * `simulate motor4_fail` — and stay in the product. They are what days 19 and
 * 20 test the diagnostic rules against, where the right answer is known in
 * advance because it was chosen.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS MODELLED
 * ---------------------------------------------------------------------------
 *
 * A session runs through a fixed sequence: settle, then each of four motors in
 * turn, then quiet again. Current is the sum of the flight controller's idle
 * draw and whatever motor is spinning. Voltage is the pack's open-circuit
 * voltage minus the drop across its internal resistance, so a heavy motor sags
 * the rail — which is the whole reason sag is worth measuring.
 *
 * The numbers are plausible for a 1S Meteor75 Pro but they are not
 * measurements. Nothing here may be used to set a threshold; thresholds come
 * from a real airframe on a real bench, per plan section 10.
 */
#ifndef DRONEBENCH_SIMULATOR_H
#define DRONEBENCH_SIMULATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SIM_PROFILE_NORMAL = 0,
    SIM_PROFILE_MOTOR4_FAIL, /* motor 4 never spins up */
    SIM_PROFILE_MOTOR2_HIGH, /* motor 2 draws far more than its peers */
    SIM_PROFILE_SAG,         /* a tired pack: high internal resistance */
    SIM_PROFILE_SHORT,       /* a dead short the moment power is applied */
} sim_profile_t;

typedef struct {
    sim_profile_t profile;
    uint64_t      start_us;
    bool          started;
} simulator_t;

void simulator_init(simulator_t *sim, sim_profile_t profile);

/*
 * The readings this profile would produce now_us into the session. Both
 * outputs are always written — the simulator models a working sensor; sensor
 * failure is the platform's business, not the profile's.
 */
void simulator_read(simulator_t *sim, uint64_t now_us, float *voltage_v,
                    float *current_a);

/* Total length of the scripted sequence. */
uint64_t simulator_duration_us(void);

/* For CLI argument handling. Returns false for an unknown name. */
bool        simulator_profile_from_name(const char *name, sim_profile_t *out);
const char *simulator_profile_name(sim_profile_t profile);

#endif /* DRONEBENCH_SIMULATOR_H */
