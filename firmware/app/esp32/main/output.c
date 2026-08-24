/*
 * The console port's only writer. Contract in output.h.
 */
#include "freertos/FreeRTOS.h" /* must come first: the rest need its config */
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

#include "esp_task_wdt.h"

#include "dronebench/platform.h"
#include "output.h"

/* Above processing (5) so a full queue drains faster than it fills, below
   measurement (7) whose loss is the only one that cannot be recovered at all.
   The four priorities only mean anything next to each other — see the table
   in PROGRESS.md for day 10. */
#define OUTPUT_TASK_PRIORITY 6

/* Holds one output_line_t (194 bytes) plus the UART driver's frames beneath
   platform_uart_write. Measured 2026-08-24: 808 bytes used of 2048. */
#define OUTPUT_TASK_STACK 2048

/* Longest this task may sleep waiting for work.
 *
 * It exists because of the watchdog, not because of the queue: blocking on
 * portMAX_DELAY would be free and correct right up until the first quiet
 * second, when the task would fail to report itself alive and the board would
 * reboot for having nothing to say. The task watchdog fires at 5 s
 * (CONFIG_ESP_TASK_WDT_TIMEOUT_S), so this leaves five times the margin. */
#define OUTPUT_IDLE_MS 1000

static QueueHandle_t s_queue;

/*
 * Guards the two counters below. They are written from every task that sends
 * and read from whoever reports, so `s_dropped++` is three instructions —
 * load, add, store — that two cores can interleave. This is the day 10
 * question about a shared variable, in its simplest form.
 *
 * A spinlock rather than a mutex because the critical section is two
 * assignments: taking a mutex would cost more than the work it protects, and
 * a task cannot be blocked here anyway — output_send() promises not to block.
 */
static portMUX_TYPE s_counters = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_dropped;
static uint32_t     s_high_water;

static void output_task(void *arg) {
  output_line_t line;

  (void)arg;

  /* NULL means "this task", so it has to run inside the task rather than in
     output_start() — which executes on whichever task called it, and that one
     is about to exit. */
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  for (;;) {
    /* pdFALSE means the timeout expired and `line` was left untouched, so it
       still holds whatever the previous iteration put there. Sending it would
       put a line on the wire that nothing ever composed — the same contract
       as sampler_take(), which leaves *out alone on anything but success. */
    if (xQueueReceive(s_queue, &line, pdMS_TO_TICKS(OUTPUT_IDLE_MS)) ==
        pdTRUE) {
      /* Measured here rather than after a send, and the first attempt got it
         wrong: this task runs above every sender, so it drains each line
         before the sender resumes, and a reading taken there is always zero.
         What is left in the queue at this point is the genuine backlog. */
      UBaseType_t backlog = uxQueueMessagesWaiting(s_queue);

      portENTER_CRITICAL(&s_counters);
      if ((uint32_t)backlog + 1 > s_high_water) {
        s_high_water = (uint32_t)backlog + 1;
      }
      portEXIT_CRITICAL(&s_counters);

      /* length, not strlen: text carries no terminator, and the framed
         protocol of a later phase will contain zero bytes. */
      platform_uart_write(line.text, line.length);
    }

    /* Outside the branch on purpose. A bench with nothing to say is in a
       perfectly good state, and rebooting it for being quiet would turn the
       watchdog from a safety net into a fault of its own. */
    platform_watchdog_feed();
  }
}

bool output_send(const char *text, size_t size) {
  output_line_t line;

  /* Refused rather than truncated. A clipped telemetry line is worse than a
     missing one: the host parses whichever fields survived and calls the
     result a measurement. */
  if (size == 0 || size > sizeof line.text) {
    portENTER_CRITICAL(&s_counters);
    s_dropped++;
    portEXIT_CRITICAL(&s_counters);
    return false;
  }

  line.length = (uint16_t)size;
  memcpy(line.text, text, size);

  /* Zero ticks is the whole point: a caller must never wait here. Waiting
     would put the sender's priority behind a busy UART, which is the exact
     situation this module exists to prevent. */
  if (xQueueSend(s_queue, &line, 0) != pdTRUE) {
    portENTER_CRITICAL(&s_counters);
    s_dropped++;
    portEXIT_CRITICAL(&s_counters);
    return false;
  }

  return true;
}

uint32_t output_dropped(void) {
  uint32_t value;

  portENTER_CRITICAL(&s_counters);
  value = s_dropped;
  portEXIT_CRITICAL(&s_counters);
  return value;
}

uint32_t output_high_water(void) {
  uint32_t value;

  portENTER_CRITICAL(&s_counters);
  value = s_high_water;
  portEXIT_CRITICAL(&s_counters);
  return value;
}

void output_start(void) {
  s_queue = xQueueCreate(OUTPUT_QUEUE_DEPTH, sizeof(output_line_t));

  /* xQueueCreate returns NULL when the heap could not supply the memory.
     Without this check every later xQueueSend would quietly operate on a null
     handle, and the symptom would be "telemetry stopped" rather than "the
     bench ran out of memory at start-up". */
  configASSERT(s_queue != NULL);

  xTaskCreate(output_task, "output", OUTPUT_TASK_STACK, NULL,
              OUTPUT_TASK_PRIORITY, NULL);
}
