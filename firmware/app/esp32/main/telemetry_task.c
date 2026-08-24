/*
 * The measurement pipeline: two tasks and a queue between them.
 *
 * One task did all of this until day 10 — take a sample, fold it into the
 * metrics, encode it, put it on the wire. It worked, and it had one flaw that
 * only appears when the port is busy: the sample after a blocking write does
 * not happen. A period lost that way cannot be recovered, and nothing in the
 * data says it went missing.
 *
 * So the work is split by what each half is allowed to wait for:
 *
 *   measurement_task   waits only on its own timer. Never on anything else.
 *   processing_task    waits on the queue, and may take as long as it likes.
 *
 * That is the whole design. Everything below follows from it — including the
 * two different answers to "what do I do when the queue downstream is full",
 * which differ because the losses differ.
 */
#include "telemetry_task.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_task_wdt.h"

#include "dronebench/metrics.h"
#include "dronebench/platform.h"
#include "dronebench/sampler.h"
#include "dronebench/session.h"
#include "dronebench/telemetry.h"

#include "output.h"

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

/*
 * 32 ms of samples. Deep enough that processing can be preempted for several
 * periods without loss, shallow enough that a backlog shows up as drops
 * rather than as telemetry arriving a second late — a stale sample and a
 * missing one are both wrong, but only one of them announces itself.
 *
 * 24 bytes an item, so it costs a fraction of the output queue, whose items
 * are eight times larger.
 */
#define SAMPLE_QUEUE_DEPTH 16

/* Highest in the firmware: a missed sampling period is the only loss here
   that cannot be made good afterwards. Output is 6, processing 5, CLI 4. */
#define MEASUREMENT_PRIORITY 7
#define PROCESSING_PRIORITY 5

/*
 * Measured 2026-08-24 with uxTaskGetStackHighWaterMark after a 500 Hz run:
 * measurement used 656 bytes of 3072, processing 2044 of 4096.
 *
 * Kept generous rather than trimmed to the measurement. The analog front-end
 * arrives on day 12 and adds calibration arithmetic to the measurement path,
 * and a stack that overflows does not fail where it ran out — it writes into
 * the next task's memory and something unrelated breaks later.
 */
#define MEASUREMENT_STACK 3072
#define PROCESSING_STACK 4096

/* How long processing may sleep waiting for a sample. Bounded only because of
   the task watchdog: with no session running there is nothing to receive, and
   a task that sleeps past the 5 s timeout is rebooted for having been idle. */
#define PROCESSING_IDLE_MS 1000

/* --- what this file remembers -------------------------------------------- */

/* Owned by measurement_task alone after the split, so no lock: nothing else
   reaches it, and the reset that used to arrive from the CLI task now happens
   inside that task, driven by the generation counter below. */
static sampler_t s_sampler;

/* Owned by processing_task alone, for the same reason. */
static metrics_t s_metrics;

/* Written by the CLI task through the functions in the header, read by both
   pipeline tasks. Everything reaching it goes through s_lock. */
static session_t         s_session;
static SemaphoreHandle_t s_lock;

/*
 * Bumped by every session start. Each task keeps its own copy and clears its
 * own state when the two disagree.
 *
 * This exists because the alternative is worse. Until day 10 the CLI task
 * called sampler_init() and metrics_init() directly, reaching into structures
 * a 500 Hz task was in the middle of using. Under one lock that was
 * survivable with a single owner; with two it stops being defensible. Now
 * each task resets what it owns, at a moment of its own choosing, and no
 * structure is touched by anyone but its owner.
 */
static uint32_t s_generation;

/* Counters, written from three tasks — measurement drops, session resets — so
   a plain increment is three instructions two cores can interleave. A
   spinlock rather than the mutex above: the section is one assignment, and
   measurement must never block here. */
static portMUX_TYPE s_counters = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_missed_periods; /* ran late, no sample taken at all */
static uint32_t     s_queue_drops;    /* sample taken, queue full, discarded */

static QueueHandle_t s_samples;

/* --- the shared part ------------------------------------------------------ */

/*
 * Reads both pieces of session state under one lock. Two separate calls would
 * let the session end between them, and the caller would act on a pair that
 * never existed at the same instant.
 */
static bool session_active_locked(uint32_t *generation) {
  bool active;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  active = session_is_active(&s_session);
  *generation = s_generation;
  xSemaphoreGive(s_lock);
  return active;
}

bool telemetry_session_start(void) {
  size_t n = 0;
  char   buf[TELEMETRY_LINE_MAX];

  xSemaphoreTake(s_lock, portMAX_DELAY);

  bool started = session_start(&s_session);

  if (started) {
    /* Only the generation changes here. The tasks own their own structures
       and clear them when they notice. */
    s_generation++;
    n = telemetry_encode_header(buf, sizeof buf, SAMPLE_RATE_HZ);
  }

  xSemaphoreGive(s_lock);

  if (started) {
    portENTER_CRITICAL(&s_counters);
    s_missed_periods = 0;
    s_queue_drops = 0;
    portEXIT_CRITICAL(&s_counters);

    /* Samples queued by the previous session belong to a different timeline.
       Safe from here: queue operations carry their own locking. */
    xQueueReset(s_samples);
  }

  if (n > 0) {
    output_send(buf, n);
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

uint32_t telemetry_missed_periods(void) {
  uint32_t value;

  portENTER_CRITICAL(&s_counters);
  value = s_missed_periods;
  portEXIT_CRITICAL(&s_counters);
  return value;
}

uint32_t telemetry_queue_drops(void) {
  uint32_t value;

  portENTER_CRITICAL(&s_counters);
  value = s_queue_drops;
  portEXIT_CRITICAL(&s_counters);
  return value;
}

/* --- measurement ---------------------------------------------------------- */

/*
 * Takes one sample per period and hands it on. Nothing here is allowed to
 * wait, which is the reason the rest of the pipeline exists.
 */
static void measurement_task(void *arg) {
  TickType_t     last_wake = xTaskGetTickCount();
  uint32_t       generation = 0;
  power_sample_t sample;

  (void)arg;
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  for (;;) {
    uint32_t current;
    bool     active = session_active_locked(&current);

    if (current != generation) {
      /* A new session. Reset what this task owns, and only that. */
      sampler_init(&s_sampler, MAX_GAP_US);
      generation = current;
    }

    if (active && sampler_take(&s_sampler, &sample) == SAMPLE_OK) {
      /* Zero ticks. Waiting for room would mean missing the next period —
         precisely the loss this queue was put here to prevent, arriving by a
         different route. A discarded sample costs one point on a graph; a
         late one shifts the timeline every later figure is built from. */
      if (xQueueSend(s_samples, &sample, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_counters);
        s_queue_drops++;
        portEXIT_CRITICAL(&s_counters);
      }
    }

    platform_watchdog_feed();

    /* xTaskDelayUntil counts from the previous wake-up, so time spent above
     * does not push the next one later. vTaskDelay would add the period
     * *after* the work, and 0.3 ms of body would turn 500 Hz into 435 Hz.
     *
     * pdFALSE means the deadline had already passed — a period was missed and
     * no sample was taken. A more honest number than "the port was busy": it
     * says the bench could not keep up with itself. */
    if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS)) ==
        pdFALSE) {
      portENTER_CRITICAL(&s_counters);
      s_missed_periods++;
      portEXIT_CRITICAL(&s_counters);
    }
  }
}

/* --- processing ----------------------------------------------------------- */

static void send_summary(void) {
  char buf[TELEMETRY_LINE_MAX];

  session_metrics_t   result;
  telemetry_summary_t summary = {0};

  metrics_result(&s_metrics, &result);

  summary.consumed_mah = result.consumed_mah;
  summary.consumed_wh = result.consumed_wh;
  summary.min_voltage_v = result.min_voltage_v;
  summary.max_current_a = result.max_current_a;
  summary.sample_count = result.sample_count;

  /* Read without a lock: measurement owns the sampler, and these are single
     32-bit loads of counters that only ever grow. A value one behind is
     harmless in a report that arrives every second; a torn value is not
     possible for a 32-bit load on this core. */
  summary.gaps = s_sampler.gaps;
  summary.sensor_failures = s_sampler.sensor_failures;
  summary.rejected_time = s_sampler.rejected_time;
  summary.rejected_value = s_sampler.rejected_value;

  /* Everything taken but never delivered: periods the sampler was too late
     for, samples the queue could not hold, and lines the output queue could
     not hold. Three separate places, one number, because from the host's side
     they are the same loss. */
  summary.dropped = telemetry_missed_periods() + telemetry_queue_drops() +
                    output_dropped();

  size_t n = telemetry_encode_summary(buf, sizeof buf, &summary);

  if (n > 0) {
    output_send(buf, n);
  }
}

/*
 * Folds each sample into the metrics and puts it on the wire.
 *
 * May block freely: everything it does is either recoverable or already
 * recorded. That is the difference between this task and the one above, and
 * the reason they are two tasks.
 */
static void processing_task(void *arg) {
  uint32_t       generation = 0;
  TickType_t     last_summary = xTaskGetTickCount();
  power_sample_t sample;
  char           buf[TELEMETRY_LINE_MAX];

  (void)arg;
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  for (;;) {
    uint32_t current;
    bool     active = session_active_locked(&current);

    if (current != generation) {
      metrics_init(&s_metrics);
      generation = current;
      last_summary = xTaskGetTickCount();
    }

    /* pdFALSE means the timeout expired and `sample` was left untouched. */
    if (xQueueReceive(s_samples, &sample, pdMS_TO_TICKS(PROCESSING_IDLE_MS)) ==
        pdTRUE) {
      metrics_add(&s_metrics, &sample);

      size_t n = telemetry_encode_sample(buf, sizeof buf, &sample);

      /* The second answer to "the queue downstream is full", and it differs
         from measurement's on purpose. The sample is already in the metrics
         by now, so what is lost is a line on the wire, not a number: the
         firmware's own mAh figure stays correct and only the host's view has
         a hole in it. output_send() counts the loss, and the gap between the
         two integrations is what section 10.1 is built on. */
      if (n > 0) {
        output_send(buf, n);
      }
    }

    /* Timed rather than counted in samples. Counting periods made a summary
       cover a known number of attempts, which mattered before sample_count
       went on the wire; now the host is told the count directly, and a steady
       1 Hz is worth more — a stuttering pipeline is exactly when a summary
       needs to arrive on time. */
    if (active && xTaskGetTickCount() - last_summary >=
                      pdMS_TO_TICKS(SUMMARY_PERIOD_MS)) {
      last_summary = xTaskGetTickCount();
      send_summary();
    }

    platform_watchdog_feed();
  }
}

/* --- start-up ------------------------------------------------------------- */

void telemetry_task_start(void) {
  s_lock = xSemaphoreCreateMutex();
  configASSERT(s_lock != NULL);

  s_samples = xQueueCreate(SAMPLE_QUEUE_DEPTH, sizeof(power_sample_t));
  configASSERT(s_samples != NULL);

  sampler_init(&s_sampler, MAX_GAP_US);
  metrics_init(&s_metrics);
  session_init(&s_session);

  xTaskCreate(measurement_task, "measure", MEASUREMENT_STACK, NULL,
              MEASUREMENT_PRIORITY, NULL);
  xTaskCreate(processing_task, "process", PROCESSING_STACK, NULL,
              PROCESSING_PRIORITY, NULL);
}
