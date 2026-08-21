/*
 * Specification for the session state. Read alongside session.h.
 */
#include "dronebench/session.h"

#include "test_framework.h"
#include "tests.h"

/* Drives a session up to RUNNING, which most tests need as a starting point. */
static void running(session_t *s)
{
    session_init(s);
    session_start(s);
    session_precheck_passed(s);
}

void test_session(void)
{
    session_t s;

    TF_CASE("a fresh session is idle with nothing found");
    {
        session_init(&s);
        CHECK_INT(s.phase, PHASE_IDLE);
        CHECK(!s.warned);
        CHECK(!s.failed);
        CHECK(!s.unsafe);
        CHECK(!session_is_active(&s));
        CHECK_INT(s.rejected_requests, 0);
        CHECK_STR(session_state_name(&s), "IDLE");
    }

    TF_CASE("the forward path runs idle to done");
    {
        session_init(&s);

        CHECK(session_start(&s));
        CHECK_INT(s.phase, PHASE_PRECHECK);
        CHECK(session_is_active(&s));
        CHECK_STR(session_state_name(&s), "PRECHECK");

        CHECK(session_precheck_passed(&s));
        CHECK_INT(s.phase, PHASE_RUNNING);
        CHECK_STR(session_state_name(&s), "RUNNING");

        CHECK(session_sequence_finished(&s));
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK(!session_is_active(&s));
        CHECK_STR(session_state_name(&s), "COMPLETE");
        CHECK_INT(s.stop_reason, STOP_SEQUENCE_FINISHED);
    }

    TF_CASE("starting twice is refused and counted");
    {
        session_init(&s);
        CHECK(session_start(&s));
        CHECK(!session_start(&s));
        CHECK_INT(s.phase, PHASE_PRECHECK); /* unchanged */
        CHECK_INT(s.rejected_requests, 1);
    }

    TF_CASE("precheck cannot be passed before it has started");
    {
        session_init(&s);
        CHECK(!session_precheck_passed(&s));
        CHECK_INT(s.phase, PHASE_IDLE);
        CHECK_INT(s.rejected_requests, 1);
    }

    TF_CASE("stop ends an active session and records who ended it");
    {
        running(&s);
        CHECK(session_stop(&s));
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.stop_reason, STOP_OPERATOR);

        /* Stopped, not failed: the drone was not judged, the test was cut
           short. The report has to be able to tell those apart. */
        CHECK(!s.failed);
        CHECK_STR(session_state_name(&s), "COMPLETE");
    }

    TF_CASE("stop during precheck is accepted too");
    {
        session_init(&s);
        session_start(&s);
        CHECK(session_stop(&s));
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.stop_reason, STOP_OPERATOR);
    }

    TF_CASE("stop with nothing running is refused");
    {
        session_init(&s);
        CHECK(!session_stop(&s));
        CHECK_INT(s.rejected_requests, 1);
    }

    TF_CASE("a warning latches and does not end the session");
    {
        running(&s);
        CHECK(session_raise_warning(&s));
        CHECK(s.warned);
        CHECK_INT(s.phase, PHASE_RUNNING); /* the test continues */
        CHECK(session_is_active(&s));
        CHECK_STR(session_state_name(&s), "WARNING");
    }

    TF_CASE("a warning survives to the end of the session");
    {
        running(&s);
        session_raise_warning(&s);
        session_sequence_finished(&s);

        /* This is the whole reason findings are separate from the phase: a
           flat state machine arriving at COMPLETE would have forgotten. */
        CHECK(s.warned);
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.stop_reason, STOP_SEQUENCE_FINISHED);
        CHECK_STR(session_state_name(&s), "WARNING");
    }

    TF_CASE("a failure ends the session");
    {
        running(&s);
        CHECK(session_raise_failure(&s));
        CHECK(s.failed);
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.stop_reason, STOP_FAULT);
        CHECK_STR(session_state_name(&s), "FAILED");
    }

    TF_CASE("failure outranks a warning in the reported name");
    {
        running(&s);
        session_raise_warning(&s);
        session_raise_failure(&s);
        CHECK(s.warned);
        CHECK(s.failed);
        CHECK_STR(session_state_name(&s), "FAILED");
    }

    TF_CASE("unsafe outranks everything");
    {
        running(&s);
        session_raise_warning(&s);
        session_raise_failure(&s);
        session_raise_unsafe(&s);
        CHECK_STR(session_state_name(&s), "UNSAFE");
    }

    TF_CASE("unsafe is accepted from any phase, including idle");
    {
        /* A short circuit is found by the continuity check, before anything
           has been started. The bench must be able to say so. */
        session_init(&s);
        session_raise_unsafe(&s);
        CHECK(s.unsafe);
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.rejected_requests, 0); /* never refused */
        CHECK_STR(session_state_name(&s), "UNSAFE");
    }

    TF_CASE("unsafe ends the session immediately");
    {
        running(&s);
        session_raise_unsafe(&s);
        CHECK_INT(s.phase, PHASE_DONE);
        CHECK_INT(s.stop_reason, STOP_FAULT);
        CHECK(!session_is_active(&s));
    }

    TF_CASE("a warning raised outside a session is refused");
    {
        session_init(&s);
        CHECK(!session_raise_warning(&s));
        CHECK(!s.warned);
        CHECK_INT(s.rejected_requests, 1);
    }

    TF_CASE("reset returns to idle and clears the findings");
    {
        running(&s);
        session_raise_warning(&s);
        session_raise_failure(&s);

        CHECK(session_reset(&s));
        CHECK_INT(s.phase, PHASE_IDLE);
        CHECK(!s.warned);
        CHECK(!s.failed);
        CHECK_INT(s.stop_reason, STOP_NOT_STOPPED);
        CHECK_STR(session_state_name(&s), "IDLE");
    }

    TF_CASE("reset is refused while a session is active");
    {
        running(&s);
        CHECK(!session_reset(&s));
        CHECK_INT(s.phase, PHASE_RUNNING);
        CHECK_INT(s.rejected_requests, 1);
    }

    TF_CASE("a new session cannot start until unsafe is acknowledged");
    {
        running(&s);
        session_raise_unsafe(&s);

        /* The phase is DONE, so this would otherwise look startable. The
           operator has to acknowledge that a dangerous condition was seen
           before the bench will run again. */
        CHECK(!session_start(&s));
        CHECK_INT(s.rejected_requests, 1);

        CHECK(session_reset(&s));
        CHECK(!s.unsafe);
        CHECK(session_start(&s));
    }

    TF_CASE("a failed session may be followed by another without reset");
    {
        running(&s);
        session_raise_failure(&s);

        /* Unlike unsafe: the drone was judged defective, nothing is
           dangerous, and the next airframe can go straight on the bench.
           start() clears the previous drone's findings — otherwise the new
           one inherits a verdict it never earned. */
        CHECK(session_start(&s));
        CHECK_INT(s.phase, PHASE_PRECHECK);
        CHECK(!s.failed);
        CHECK_INT(s.stop_reason, STOP_NOT_STOPPED);
    }
}
