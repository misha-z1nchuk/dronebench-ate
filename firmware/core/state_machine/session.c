/*
 * TODO(day6): implement against the contract in session.h.
 *
 * firmware/tests/test_session.c is the specification.
 *
 * Suggested order:
 *
 *   1. session_init, session_is_active
 *   2. session_start, session_precheck_passed  — the forward path
 *   3. session_stop, session_sequence_finished — the two ways it ends
 *   4. session_reset
 *   5. findings, and the rule that unsafe is never refused
 *   6. session_state_name
 *
 * Every function that returns bool answers one question: was the request
 * accepted? A refusal increments rejected_requests and changes nothing else.
 */
#include "dronebench/session.h"

void session_init(session_t *session) { *session = (session_t){0}; }

bool session_start(session_t *session) {
  /* Active means a session is under way. Unsafe means the last one ended
     dangerously and nobody has acknowledged it yet — the phase is DONE, so
     without this check the bench would happily start again. */
  if (session_is_active(session) || session->unsafe) {
    session->rejected_requests++;
    return false;
  }

  /* The previous drone's verdict must not follow the next one onto the
     bench. */
  session->warned = false;
  session->failed = false;
  session->stop_reason = STOP_NOT_STOPPED;

  session->phase = PHASE_PRECHECK;
  return true;
}

bool session_precheck_passed(session_t *session) {
  if (session->phase != PHASE_PRECHECK) {
    session->rejected_requests++;
    return false;
  }

  session->phase = PHASE_RUNNING;
  return true;
}

bool session_sequence_finished(session_t *session) {
  if (!session_is_active(session)) {
    session->rejected_requests++;
    return false;
  }

  session->phase = PHASE_DONE;
  session->stop_reason = STOP_SEQUENCE_FINISHED;
  return true;
}

bool session_stop(session_t *session) {
  if (!session_is_active(session)) {
    session->rejected_requests++;
    return false;
  }

  session->phase = PHASE_DONE;
  session->stop_reason = STOP_OPERATOR;
  return true;
}

bool session_raise_warning(session_t *session) {
  if (!session_is_active(session)) {
    session->rejected_requests++;
    return false;
  }

  /* Latches, and the test continues — a deviation is a finding to report,
     not a reason to stop measuring. */
  session->warned = true;
  return true;
}

bool session_raise_failure(session_t *session) {
  if (!session_is_active(session)) {
    session->rejected_requests++;
    return false;
  }

  session->failed = true;
  session->phase = PHASE_DONE;
  session->stop_reason = STOP_FAULT;
  return true;
}

void session_raise_unsafe(session_t *session) {
  /* Never refused and never counted as rejected: a short circuit found by the
     continuity check before anything started is exactly as real as one found
     mid-test. Returns nothing because there is no answer a caller could act
     on — the bench is stopping either way. */
  session->unsafe = true;
  session->phase = PHASE_DONE;
  session->stop_reason = STOP_FAULT;
}

bool session_reset(session_t *session) {
  if (session_is_active(session)) {
    session->rejected_requests++;
    return false;
  }

  /* Deliberately not session_init(): the rejected-request count belongs to
     the bench, not to one session, and clearing it here would erase evidence
     that the operator and the firmware disagreed about what was going on. */
  session->phase = PHASE_IDLE;
  session->warned = false;
  session->failed = false;
  session->unsafe = false;
  session->stop_reason = STOP_NOT_STOPPED;
  return true;
}

bool session_is_active(const session_t *session) {
  return session->phase == PHASE_PRECHECK || session->phase == PHASE_RUNNING;
}

const char *session_state_name(const session_t *session) {
  /* The order of these three lines is the precedence, and it is the order in
     which they matter to someone holding a charged battery. */
  if (session->unsafe) {
    return "UNSAFE";
  }
  if (session->failed) {
    return "FAILED";
  }
  if (session->warned) {
    return "WARNING";
  }

  switch (session->phase) {
  case PHASE_IDLE:
    return "IDLE";
  case PHASE_PRECHECK:
    return "PRECHECK";
  case PHASE_RUNNING:
    return "RUNNING";
  case PHASE_DONE:
    return "COMPLETE";
  }
  return "UNKNOWN";
}
