/*
 * The measurement loop.
 *
 * The shape of this file came from four questions; the answers are marked in
 * the sections below.
 *
 *   1. what does it remember?   -> the statics
 *   2. who reaches in?          -> telemetry_task.h, three functions
 *   3. what runs by itself?     -> telemetry_task()
 *   4. what is shared?          -> s_session only, hence one lock
 */
#include "telemetry_task.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dronebench/metrics.h"
#include "dronebench/platform.h"
#include "dronebench/sampler.h"
#include "dronebench/session.h"
#include "dronebench/telemetry.h"

#define SAMPLE_RATE_HZ 500
#define SAMPLE_PERIOD_MS (1000 / SAMPLE_RATE_HZ)

/* pdMS_TO_TICKS divides by 1000 in integer arithmetic, so a period shorter
   than one tick silently becomes a delay of zero. FreeRTOS catches that at
   run time — on the bench, mid-session, as a reboot loop. This catches it at
   build time instead, which is where a fact known to the compiler belongs. */
_Static_assert(pdMS_TO_TICKS(SAMPLE_PERIOD_MS) > 0,
               "sample period is shorter than one FreeRTOS tick - raise "
               "CONFIG_FREERTOS_HZ or lower SAMPLE_RATE_HZ");

/* An interval above this counts as a gap: a few missed periods, not one. */
#define MAX_GAP_US (10 * 1000)

#define SUMMARY_PERIOD_MS 1000

/* Measured with uxTaskGetStackHighWaterMark on day 10; a starting point. */
#define TASK_STACK 4096
#define TASK_PRIORITY 6

/* --- 1. what this file remembers ----------------------------------------- */

/* All of these are shared. telemetry_task() updates them every period, and
   telemetry_session_start() resets them from whichever task runs the CLI.
   The two are not pinned to a core, so on this dual-core part they can be
   inside these structures at the same instant — "the CLI has the lower
   priority" does not serialise them. Everything below goes through s_lock. */
static sampler_t s_sampler;
static metrics_t s_metrics;
static uint32_t s_dropped;
static session_t s_session;

static SemaphoreHandle_t s_lock;

/* --- 4. the shared part -------------------------------------------------- */

static bool session_active_locked(void) {
  bool active;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  active = session_is_active(&s_session);
  xSemaphoreGive(s_lock);
  return active;
}

bool telemetry_session_start(void) {
  size_t n = 0;
  char buf[TELEMETRY_LINE_MAX];

  xSemaphoreTake(s_lock, portMAX_DELAY);

  bool started = session_start(&s_session);

  if (started) {
    sampler_init(&s_sampler, MAX_GAP_US);
    metrics_init(&s_metrics);
    s_dropped = 0;

    n = telemetry_encode_header(buf, sizeof buf, SAMPLE_RATE_HZ);
  }

  xSemaphoreGive(s_lock);

  if (n > 0) {
    platform_uart_write(buf, n);
  }

  return started;
}

bool telemetry_session_stop(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool stopped = session_stop(&s_session);
  xSemaphoreGive(s_lock);

  return stopped;
}

const char *telemetry_state_name(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const char *name = session_state_name(&s_session);
  xSemaphoreGive(s_lock);

  return name;
}

/* --- 3. what runs by itself ----------------------------------------------
 */

static void send_summary(void) {
  char buf[TELEMETRY_LINE_MAX];

  session_metrics_t result;
  telemetry_summary_t summary = (telemetry_summary_t){0};

  xSemaphoreTake(s_lock, portMAX_DELAY);

  metrics_result(&s_metrics, &result);
  summary.consumed_mah = result.consumed_mah;
  summary.consumed_wh = result.consumed_wh;
  summary.min_voltage_v = result.min_voltage_v;
  summary.max_current_a = result.max_current_a;
  summary.sample_count = result.sample_count;
  summary.dropped = s_dropped;
  summary.gaps = s_sampler.gaps;
  summary.sensor_failures = s_sampler.sensor_failures;
  summary.rejected_time = s_sampler.rejected_time;
  summary.rejected_value = s_sampler.rejected_value;

  size_t n = telemetry_encode_summary(buf, sizeof buf, &summary);

  xSemaphoreGive(s_lock);

  if (n > 0) {
    platform_uart_write(buf, n);
  }
}

/*
 * One period of work. Every early exit here is a `return`, which leaves this
 * function and lands back in telemetry_task()'s loop just before the delay —
 * so no path through a period can skip blocking. A `continue` inside the loop
 * itself would skip it, and a task at this priority that never blocks starves
 * its core until the watchdog reboots the board.
 */
static void run_one_period(void) {
  /* Only this task ever reaches it, so it needs no lock of its own. */
  static uint32_t periods_since_summary;

  power_sample_t sample;
  char           buf[TELEMETRY_LINE_MAX];

  if (!session_active_locked()) {
    /* Nothing to summarise between sessions, and the next one should get a
       full period of its own rather than inheriting a part-finished count. */
    periods_since_summary = 0;
    return;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  sample_result_t result = sampler_take(&s_sampler, &sample);

  if (result == SAMPLE_OK) {
    metrics_add(&s_metrics, &sample);
  }
  xSemaphoreGive(s_lock);

  /* Checked a second time rather than folded into the block above: encoding
     and transmitting must happen outside the lock, and by this point `sample`
     is a private copy, which is exactly why sampler_take() fills a caller's
     struct instead of returning a pointer into its own. */
  if (result == SAMPLE_OK) {
    size_t n = telemetry_encode_sample(buf, sizeof buf, &sample);

    if (n > 0) {
      platform_uart_write(buf, n);
    }
  }

  /* Counted in periods rather than milliseconds, so a summary always covers a
     known number of attempted samples. Deliberately outside the SAMPLE_OK
     check: a session where every read fails still has to report that, and it
     is the one case where the summary matters most. */
  if (++periods_since_summary >= SUMMARY_PERIOD_MS / SAMPLE_PERIOD_MS) {
    periods_since_summary = 0;

    /* s_lock is not recursive, so this must be called without holding it —
       taking it twice from one task blocks that task against itself, with no
       diagnostic from FreeRTOS. */
    send_summary();
  }
}

static void telemetry_task(void *arg) {
  TickType_t last_wake = xTaskGetTickCount();

  (void)arg;

  for (;;) {
    run_one_period();

    /* xTaskDelayUntil counts from the previous wake-up, so the time spent
     * in the body above does not push the next one later. vTaskDelay would
     * add the period *after* the work, and 0.3 ms of body would turn
     * 500 Hz into 435 Hz over an hour.
     *
     * It returns pdFALSE when the deadline had already passed — meaning a
     * period was missed and a sample was never taken. That is what
     * s_dropped counts, and it is a more honest number than "the port was
     * busy": it says the bench could not keep up. */
    if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS)) ==
        pdFALSE) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      s_dropped++;
      xSemaphoreGive(s_lock);
    }
  }
}

void telemetry_task_start(void) {
  s_lock = xSemaphoreCreateMutex();
  configASSERT(s_lock != NULL);

  sampler_init(&s_sampler, MAX_GAP_US);
  metrics_init(&s_metrics);
  session_init(&s_session);
  s_dropped = 0;

  xTaskCreate(telemetry_task, "telemetry", TASK_STACK, NULL, TASK_PRIORITY,
              NULL);
}
