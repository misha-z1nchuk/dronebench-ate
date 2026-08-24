/*
 * ESP32 implementation of the platform interface.
 *
 * The ADC channels are deliberately still failures — the analog front-end does
 * not exist yet, and a bench that returns a plausible number it did not
 * measure is worse than one that admits it cannot.
 *
 * Pin assignments come from plan section 4.5.
 */
#include "platform_esp32.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "dronebench/platform.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#define PLATFORM_LED_GPIO 2

/* Sized for a host that pastes a whole command at once. It only has to outlast
   the scheduling gap between two runs of whoever is draining it. */
#define PLATFORM_UART_RX_BUFFER 512

void platform_esp32_init(void) {
  const gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << PLATFORM_LED_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);

  ESP_ERROR_CHECK(err);

  platform_status_led_set(false);

  /* The console port belongs to the platform, not to whoever happens to use
     it first. Anything calling platform_uart_write() after this function has
     run gets a working port, with no hidden requirement to call some other
     module's init beforehand. */
  const uart_config_t uart_conf = {
      .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_param_config(PLATFORM_CONSOLE_UART_PORT, &uart_conf));
  ESP_ERROR_CHECK(uart_driver_install(
      PLATFORM_CONSOLE_UART_PORT, PLATFORM_UART_RX_BUFFER, 2048, 0, NULL, 0));
}

uint64_t platform_time_us(void) {
  /*
   * The core requires this never to go backwards — every duration, every
   * mAh integration step and every timeout is a subtraction of two of
   * these. A clock that jumps back turns a positive interval into a huge
   * positive one, because the result is unsigned.
   */
  return (uint64_t)esp_timer_get_time();
}

/*
 * Simulation state. Enabled by default because the ADC path below cannot yet
 * answer: the dividers are not built, and putting 4.35 V on a 3.3 V pin to
 * find out would cost a board.
 */
static struct {
  bool enabled;
  simulator_t sim;
} s_source = {
    .enabled = true,
};

void platform_esp32_set_simulation(bool enabled, sim_profile_t profile) {
  s_source.enabled = enabled;
  simulator_init(&s_source.sim, profile);
}

bool platform_esp32_simulation_enabled(void) { return s_source.enabled; }

sim_profile_t platform_esp32_simulation_profile(void) {
  return s_source.sim.profile;
}

/*
 * Both channels are read from one simulator call so they stay consistent with
 * each other. Sampling them separately would let the timeline advance between
 * the two, and voltage would then describe a slightly different instant than
 * the current it is supposed to pair with — small, invisible, and exactly the
 * kind of thing that shows up later as an unexplained offset in the power
 * figure.
 */
static void simulated_pair(float *voltage_v, float *current_a) {
  simulator_read(&s_source.sim, platform_time_us(), voltage_v, current_a);
}

bool platform_adc_read_voltage(float *value) {
  if (s_source.enabled) {
    float current;

    simulated_pair(value, &current);
    return true;
  }

  /* Day 12. Until the divider exists and is calibrated, reporting a number
     here would be worse than reporting nothing. */
  return false;
}

bool platform_adc_read_current(float *value) {
  if (s_source.enabled) {
    float voltage;

    simulated_pair(&voltage, value);
    return true;
  }

  /* Day 13. */
  return false;
}

bool platform_ref_read_current(float *value) {
  /* Day 14, and only if the INA226 is actually fitted. */
  (void)value;
  return false;
}

void platform_uart_write(const char *data, size_t size) {
  /*
   * Takes a length rather than a NUL-terminated string on purpose: the
   * binary protocol in day 7 will send frames containing zero bytes.
   */
  uart_write_bytes(PLATFORM_CONSOLE_UART_PORT, data, size);
}

void platform_watchdog_feed(void) {
  /*
   * Only succeeds for a task that has subscribed to the task watchdog. No task
   * has yet, so today every call returns ESP_ERR_NOT_FOUND and feeds nothing —
   * which is indistinguishable from a working watchdog until the day it is
   * needed. Day 10 subscribes the tasks and verifies this actually fires.
   *
   * ESP_ERROR_CHECK is deliberately absent: it would abort on every call in
   * the current state.
   */
  esp_task_wdt_reset();
}

void platform_status_led_set(bool on) {
  gpio_set_level(PLATFORM_LED_GPIO, on ? 1 : 0);
}
