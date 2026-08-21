/*
 * Session state.
 *
 * Two things are tracked separately because they are independent: where the
 * bench is in the test sequence, and what it has found. A drone can be
 * mid-sequence with a warning already recorded; it can fail its precheck
 * before the sequence starts. Folding both into one enum multiplies the state
 * count and still needs the findings kept somewhere, because a session that
 * ends in COMPLETE does not remember that it passed through WARNING.
 *
 *   phase    where in the sequence      IDLE -> PRECHECK -> RUNNING -> DONE
 *   findings what has been observed     latched, never cleared mid-session
 *
 * The names in section 4.4.1 and section 10 of the plan — IDLE, PRECHECK,
 * RUNNING, WARNING, FAILED, UNSAFE, COMPLETE — remain the vocabulary of the
 * CLI and the report. session_state_name() derives them. The operator thinks
 * in states; the code works in phases and flags.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT
 * ---------------------------------------------------------------------------
 *
 * Findings latch. A warning that appeared for 200 ms and cleared is still a
 * finding, and a report that omits it is describing a different drone than the
 * one on the bench.
 *
 * Precedence, highest first: UNSAFE, FAILED, WARNING. This is the order in
 * which they matter to whoever is holding a charged battery.
 *
 * UNSAFE is the only finding that:
 *   - may be raised from any phase, including IDLE and PRECHECK
 *   - forces the phase to DONE immediately
 *   - requires session_reset() before another session may start
 *
 * FAILED ends the session too, but a next drone may be started right away.
 *
 * session_stop() from an active phase ends the session with whatever data was
 * collected, and records that it was stopped rather than finished. Discarding
 * measurements the operator already took is worse than reporting them honestly
 * labelled as partial.
 *
 * Requests that make no sense in the current phase are refused and counted,
 * not silently ignored: a bench that quietly drops a `stop` is a bench whose
 * operator does not know the motor is still spinning.
 */
#ifndef DRONEBENCH_SESSION_H
#define DRONEBENCH_SESSION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PHASE_IDLE = 0,
    PHASE_PRECHECK,
    PHASE_RUNNING,
    PHASE_DONE,
} session_phase_t;

typedef enum {
    STOP_NOT_STOPPED = 0,
    STOP_SEQUENCE_FINISHED, /* the profile ran to its end */
    STOP_OPERATOR,          /* someone typed stop */
    STOP_FAULT,             /* a finding ended it */
} session_stop_reason_t;

typedef struct {
    session_phase_t phase;

    bool warned;
    bool failed;
    bool unsafe;

    session_stop_reason_t stop_reason;

    /* Refused requests. A count that is not zero at the end of a session is
       worth showing: it means the operator and the bench disagreed about what
       state it was in. */
    uint32_t rejected_requests;
} session_t;

void session_init(session_t *session);

/*
 * IDLE -> PRECHECK. Refused if a session is already under way, or while an
 * unacknowledged UNSAFE is outstanding.
 *
 * Clears the previous session's findings. Without that, an operator who put a
 * second airframe on the bench after a FAILED verdict would see it condemned
 * before its test began.
 */
bool session_start(session_t *session);

/* PRECHECK -> RUNNING. False from any other phase. */
bool session_precheck_passed(session_t *session);

/* Ends the session as finished by the test profile. */
bool session_sequence_finished(session_t *session);

/* Ends the session by operator request. False when nothing is running. */
bool session_stop(session_t *session);

/* Findings. Raising one during an inactive phase is refused, except for
   unsafe, which is always accepted — a short circuit does not wait for the
   bench to be ready. */
bool session_raise_warning(session_t *session);
bool session_raise_failure(session_t *session);
void session_raise_unsafe(session_t *session);

/* DONE -> IDLE, and clears the findings. Required after UNSAFE; harmless
   otherwise. False while a session is still active — use stop first. */
bool session_reset(session_t *session);

bool session_is_active(const session_t *session);

/* The plan's vocabulary, derived. Never NULL. */
const char *session_state_name(const session_t *session);

#endif /* DRONEBENCH_SESSION_H */
