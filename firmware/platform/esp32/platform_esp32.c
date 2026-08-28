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
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#define PLATFORM_LED_GPIO 2
/*
 * Conversions averaged per reading. Four times the samples buys one bit, so
 * this is worth three; 256 would be worth four and is the other value the
 * plan suggests. At a few microseconds per conversion both fit inside the
 * 2 ms a 500 Hz period allows, so the choice is free and can be revisited
 * once day 12 has measured what the noise actually is.
 */
#define TIMES_READ_VOLTAGE 64
/* Sized for a host that pastes a whole command at once. It only has to outlast
   the scheduling gap between two runs of whoever is draining it. */
#define PLATFORM_UART_RX_BUFFER 512

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

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

  const adc_oneshot_unit_init_cfg_t init_config1 = {
      .unit_id = ADC_UNIT_1,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };

  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &s_adc));

  const adc_oneshot_chan_cfg_t config = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &config));

  const adc_cali_line_fitting_config_t cali_config = {
      .unit_id = ADC_UNIT_1,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };

  s_cali_ok =
      adc_cali_create_scheme_line_fitting(&cali_config, &s_cali) == ESP_OK;
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

/*
 * Millivolts at the battery divider's node, before any calibration of ours.
 *
 * This is one half of every calibration pair; the other half is what the
 * multimeter says about the same point at the same moment. Contract in
 * platform.h, arithmetic in core/measurements/calibration.c.
 *
 * WHY THE AVERAGE IS TAKEN AFTER THE CONVERSION AND NOT BEFORE
 *
 * The cheaper arrangement is to sum raw counts, divide, and convert once.
 * It gives a worse answer. adc_cali_raw_to_voltage() returns whole
 * millivolts, so converting the average rounds the result into 1 mV steps
 * and everything multisampling bought below that is discarded. Converting
 * first and averaging after keeps it: the ADC's own noise dithers each
 * conversion across the boundary, and the mean of sixty-four of them lands
 * between the steps. The cost is sixty-four calls of arithmetic, which is
 * nothing next to sixty-four conversions of hardware.
 *
 * This only works because there is noise. On a perfectly quiet input every
 * conversion returns the same integer and the average returns it too — which
 * is the correct answer in that case anyway.
 */

/* GPIO34, per plan section 4.5. Channels 7 (GPIO35) and 4 (GPIO32) belong to
   the current path and are configured on day 13, not before there is
   something to read them for. */
#define ADC_CHANNEL_BATTERY ADC_CHANNEL_6

bool platform_adc_read_millivolts(float *value) {
  float sum = 0.0f;

  /*
   * Refused rather than approximated. Without the factory curve the driver
   * falls back to a nominal 1100 mV reference that this particular chip does
   * not have, and the resulting error — a few percent — would be inherited by
   * every voltage the bench ever reports, including the calibration meant to
   * remove it.
   */
  if (!s_cali_ok) {
    return false;
  }

  for (int i = 0; i < TIMES_READ_VOLTAGE; i++) {
    int raw;
    int mv;

    /* Not ESP_ERROR_CHECK: that aborts the firmware, and a driver hiccup
       must not reboot a bench with a motor spinning. sampler_t already
       counts a refused reading as sensor_failures, which is where this
       belongs. */
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_BATTERY, &raw) != ESP_OK) {
      return false;
    }
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
      return false;
    }

    sum += (float)mv;
  }

  /* 64 * ~2200 is about 140 000, exact in a float's 24-bit mantissa with four
     orders to spare, so nothing is lost in the accumulation. */
  *value = sum / (float)TIMES_READ_VOLTAGE;
  return true;
}

bool platform_adc_read_voltage(float *value) {
  if (s_source.enabled) {
    float current;

    simulated_pair(value, &current);
    return true;
  }

  /* Day 12. Until the divider exists and is calibrated, reporting a number
     here would be worse than reporting nothing.

     Once platform_adc_read_millivolts() works, this becomes: read it, then
     calibration_apply(&s_voltage_cal, mv, value). It stays false until the
     fit has been made and stored — an uncalibrated reading is not a rough
     measurement of the battery, it is a measurement of something else. */
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
   * Only succeeds for a task that has subscribed to the task watchdog. Until
   * day 10 no task had, so every call returned ESP_ERR_NOT_FOUND and fed
   * nothing — indistinguishable from a working watchdog right up until the
   * day it was needed. The three long-lived tasks now subscribe themselves
   * with esp_task_wdt_add(NULL), which is deliberately not abstracted here:
   * subscription is a property of a task, and the tasks are platform code.
   *
   * ESP_ERROR_CHECK stays absent. A task that feeds without subscribing is a
   * bug, but aborting the bench mid-session is a worse answer to it than the
   * silence — and the subscription is checked where it happens.
   */
  esp_task_wdt_reset();
}

void platform_status_led_set(bool on) {
  gpio_set_level(PLATFORM_LED_GPIO, on ? 1 : 0);
}
