/*
 * DroneBench ATE — STM32 application layer.
 *
 * Everything below the CLI table is the same core the ESP32 build runs and
 * the same core `make test` runs on the host. Nothing in firmware/core/ was
 * changed to make this file work; if that ever stops being true, the
 * architecture has failed and the commit that broke it should say so.
 */
#include "app_stm32.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dronebench/cli.h"
#include "dronebench/metrics.h"
#include "dronebench/platform.h"
#include "dronebench/sampler.h"
#include "dronebench/session.h"
#include "dronebench/simulator.h"
#include "dronebench/telemetry.h"
#include "platform_stm32.h"
#include "stm32f4xx_hal.h"

extern UART_HandleTypeDef huart2;

#define FW_VERSION "0.1.0"

/*
 * Same rate as the ESP32 build, so a session logged from either board is the
 * same measurement and the host tools cannot tell them apart.
 *
 * It fits, but only because the port is fast. One sample line is
 * "S,<us>,4.123,1.234,1.240\n" — about 32 bytes, and at 460800 baud that is
 * 32 * 10 / 460800 = 0.7 ms of blocking transmit inside a 2 ms period. A
 * third of the budget, with the ADC burst on top.
 *
 * At 115200 it would not fit: 2.8 ms of transmit into a 2 ms period is a
 * telemetry stream that falls permanently behind its own clock, and the
 * symptom would be a growing timestamp gap rather than an error. 460800 is
 * also what tools/serial_logger/logger.py defaults to, so USART2 has to be
 * set to it in CubeMX for `make log` to connect at all.
 */
#define SAMPLE_RATE_HZ 500
#define SAMPLE_PERIOD_US (1000000u / SAMPLE_RATE_HZ)

/* Headroom of a few periods, not one: a sample later than this is still
   accepted, but counted, because charge integrated across a gap is an
   estimate. */
#define MAX_GAP_US (10u * 1000u)

/* Largest thing written in one go is a summary line; CLI replies are bounded
   by CLI_LINE_MAX. */
#define OUT_BUF 192

/* An `adc` burst long enough to see the noise, short enough not to stall the
   console for a noticeable time. */
#define ADC_MAX_READS 1024

static cli_t     s_cli;
static session_t s_session;
static sampler_t s_sampler;
static metrics_t s_metrics;

/* When the next sample is due, in the platform clock. Advanced by a fixed
   period rather than measured from now, so the transmit time of one line does
   not push the next sample later and accumulate into a drifting rate. */
static uint64_t s_next_sample_us;
static bool     s_streaming;

/* ------------------------------------------------------------------------ */
/* Console                                                                  */
/* ------------------------------------------------------------------------ */

static void out(const char *text) { platform_uart_write(text, strlen(text)); }

static void cli_out(void *ctx, const char *text)
{
    (void)ctx;
    out(text);
}

/*
 * Non-blocking receive.
 *
 * Reads the data register directly rather than going through
 * HAL_UART_Receive(). HAL's blocking receive owns huart2.RxState for the
 * duration of the call, and a zero timeout still walks the state machine —
 * the result is a console that fights the transmit path for the handle. The
 * register is simpler and cannot deadlock against anything.
 *
 * The overrun flag has to be cleared explicitly. On the F4 an overrun stops
 * RXNE from ever being set again, so a single burst of pasted input that
 * arrives while a telemetry line is transmitting would silently kill the
 * console for the rest of the session — the exact class of failure this
 * project keeps finding: no error, just a thing that quietly stops working.
 */
static void console_poll(void)
{
    unsigned guard = 64; /* bound the work done per poll */

    while (guard-- > 0u) {
        uint32_t sr = huart2.Instance->SR;

        if ((sr & USART_SR_ORE) != 0u) {
            (void)huart2.Instance->DR; /* reading SR then DR clears ORE */
            continue;
        }
        if ((sr & USART_SR_RXNE) == 0u) {
            return;
        }
        cli_feed_char(&s_cli, (char)(huart2.Instance->DR & 0xFFu));
    }
}

/* ------------------------------------------------------------------------ */
/* Banner                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Why the reset cause is in the banner.
 *
 * The ESP32 build printed it too, and on 2026-08-28 that line was the whole
 * diagnosis: "brownout" said the supply had collapsed, not that the firmware
 * had crashed. On the STM32 the same information sits in RCC->CSR and is
 * sticky across resets, so it must be read and then cleared — an uncleared
 * flag makes every later reset look like the first one.
 */
static const char *reset_reason(void)
{
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) return "low-power";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) return "window watchdog";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) return "independent watchdog";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  return "software reset";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))  return "brownout";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))  return "power-on";
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  return "reset pin";
    return "unknown";
}

static void print_banner(void)
{
    char  buf[OUT_BUF];
    float vdda = platform_stm32_vdda_mv();

    out("\n=================================\n");
    out(" DroneBench ATE\n");
    out("=================================\n");

    snprintf(buf, sizeof buf, " firmware     %s\n", FW_VERSION);
    out(buf);
    snprintf(buf, sizeof buf, " built        %s %s\n", __DATE__, __TIME__);
    out(buf);
    snprintf(buf, sizeof buf, " chip         STM32F446RE\n");
    out(buf);
    snprintf(buf, sizeof buf, " sysclk       %" PRIu32 " Hz\n", HAL_RCC_GetSysClockFreq());
    out(buf);

    /* Not "3300". If the reference could not be established the bench says so
       rather than reporting a nominal number that reads exactly as plausible
       as a measured one. */
    if (vdda > 0.0f) {
        snprintf(buf, sizeof buf, " vdda         %.1f mV (from VREFINT)\n", (double)vdda);
    } else {
        snprintf(buf, sizeof buf, " vdda         unknown - ADC refuses\n");
    }
    out(buf);

    snprintf(buf, sizeof buf, " reset reason %s\n", reset_reason());
    out(buf);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    out("=================================\n\n");
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/*                                                                          */
/* Replies are machine-readable on purpose: in phase 3 the Python runner      */
/* sends these same commands and has to tell success from failure            */
/* programmatically, not by reading prose.                                   */
/* ------------------------------------------------------------------------ */

static void cmd_help(cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cli_print_help(cli);
}

static void cmd_version(cli_t *cli, int argc, char **argv)
{
    char buf[OUT_BUF];

    (void)argc;
    (void)argv;
    snprintf(buf, sizeof buf, "OK,version,fw=%s,built=%s %s,target=stm32f446re\n",
             FW_VERSION, __DATE__, __TIME__);
    cli_write(cli, buf);
}

static void cmd_status(cli_t *cli, int argc, char **argv)
{
    char buf[OUT_BUF];

    (void)argc;
    (void)argv;
    snprintf(buf, sizeof buf,
             "OK,status,state=%s,uptime_s=%" PRIu32 ",accepted=%" PRIu32
             ",failed=%" PRIu32 ",gaps=%" PRIu32 "\n",
             session_state_name(&s_session),
             /*
              * Deliberately narrowed to 32 bits. newlib-nano drops %llu
              * unless the image is linked with -u _printf_long_long, and it
              * drops it silently: the board printed "uptime_s=lu" for a week
              * before anyone ran it on hardware. Seconds overflow uint32_t
              * after 136 years, so the 64-bit formatter buys nothing here.
              */
             (uint32_t)(platform_time_us() / 1000000u),
             s_sampler.accepted, s_sampler.sensor_failures, s_sampler.gaps);
    cli_write(cli, buf);
}

static void cmd_vdda(cli_t *cli, int argc, char **argv)
{
    char  buf[OUT_BUF];
    float vdda = platform_stm32_vdda_mv();

    (void)argc;
    (void)argv;

    /*
     * Exists to be compared against a multimeter on the 3V3 pin. Two
     * independent instruments on one rail, the same cross-check that caught
     * the bad probe contact on day 11. If they disagree by more than a few
     * millivolts, one is wrong, and the answer is to find out which before
     * trusting either.
     */
    if (vdda <= 0.0f) {
        cli_write(cli, "ERR,vdda,unknown\n");
        return;
    }
    snprintf(buf, sizeof buf, "OK,vdda,mv=%.1f\n", (double)vdda);
    cli_write(cli, buf);
}

static void cmd_start(cli_t *cli, int argc, char **argv)
{
    char   buf[OUT_BUF];
    size_t n;

    (void)argc;
    (void)argv;

    if (!session_start(&s_session)) {
        cli_write(cli, "ERR,start,bad_state\n");
        return;
    }

    sampler_init(&s_sampler, MAX_GAP_US);
    metrics_init(&s_metrics);
    s_next_sample_us = platform_time_us();
    s_streaming      = true;

    n = telemetry_encode_header(buf, sizeof buf, SAMPLE_RATE_HZ);
    if (n > 0u) {
        platform_uart_write(buf, n);
    }
}

static void emit_summary(void)
{
    char                buf[OUT_BUF];
    session_metrics_t   result;
    telemetry_summary_t summary = {0};
    size_t              n;

    metrics_result(&s_metrics, &result);

    summary.consumed_mah  = result.consumed_mah;
    summary.consumed_wh   = result.consumed_wh;
    summary.min_voltage_v = result.min_voltage_v;
    summary.max_current_a = result.max_current_a;
    summary.sample_count  = result.sample_count;

    summary.sensor_failures = s_sampler.sensor_failures;
    summary.rejected_time   = s_sampler.rejected_time;
    summary.rejected_value  = s_sampler.rejected_value;
    summary.gaps            = s_sampler.gaps;

    /* Always zero on this build, and honestly so: a superloop that encodes
       and transmits in the same breath has nowhere to drop a sample. The
       ESP32 build has a queue between the two and can. */
    summary.dropped = 0u;

    n = telemetry_encode_summary(buf, sizeof buf, &summary);
    if (n > 0u) {
        platform_uart_write(buf, n);
    }
}

static void cmd_stop(cli_t *cli, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!session_stop(&s_session)) {
        cli_write(cli, "ERR,stop,bad_state\n");
        return;
    }
    s_streaming = false;
    emit_summary();
}

static void cmd_simulate(cli_t *cli, int argc, char **argv)
{
    char          buf[OUT_BUF];
    sim_profile_t profile;

    if (argc < 2) {
        snprintf(buf, sizeof buf, "OK,simulate,enabled=%d,profile=%s\n",
                 platform_stm32_simulation_enabled() ? 1 : 0,
                 simulator_profile_name(platform_stm32_simulation_profile()));
        cli_write(cli, buf);
        return;
    }

    if (session_is_active(&s_session)) {
        /* Changing the source mid-session would put two different
           measurements under one header. */
        cli_write(cli, "ERR,simulate,session_active\n");
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        platform_stm32_set_simulation(false, SIM_PROFILE_NORMAL);
        cli_write(cli, "OK,simulate,enabled=0\n");
        return;
    }

    if (!simulator_profile_from_name(argv[1], &profile)) {
        cli_write(cli, "ERR,simulate,unknown_profile\n");
        return;
    }

    platform_stm32_set_simulation(true, profile);
    snprintf(buf, sizeof buf, "OK,simulate,enabled=1,profile=%s\n",
             simulator_profile_name(profile));
    cli_write(cli, buf);
}

static void cmd_adc(cli_t *cli, int argc, char **argv)
{
    char  buf[OUT_BUF];
    long  requested = 1;
    long  i;
    float mv;
    float sum  = 0.0f;
    float lo   = 0.0f;
    float hi   = 0.0f;
    long  ok   = 0;
    long  fail = 0;

    if (argc >= 2) {
        requested = strtol(argv[1], NULL, 10);
        if (requested < 1 || requested > ADC_MAX_READS) {
            cli_write(cli, "ERR,adc,bad_count\n");
            return;
        }
    }

    for (i = 0; i < requested; i++) {
        if (!platform_adc_read_millivolts(&mv)) {
            fail++;
            continue;
        }
        if (ok == 0 || mv < lo) lo = mv;
        if (ok == 0 || mv > hi) hi = mv;
        sum += mv;
        ok++;
    }

    /* Refused rather than reported as zero. A calibration point taken from a
       failed read is worse than a missing one. */
    if (ok == 0) {
        cli_write(cli, "ERR,adc,no_reading\n");
        return;
    }

    if (requested == 1) {
        snprintf(buf, sizeof buf, "OK,adc,mv=%.1f\n", (double)sum);
    } else {
        snprintf(buf, sizeof buf,
                 "OK,adc,n=%ld,mv=%.2f,min=%.1f,max=%.1f,pp=%.1f,failed=%ld\n",
                 ok, (double)(sum / (float)ok), (double)lo, (double)hi,
                 (double)(hi - lo), fail);
    }
    cli_write(cli, buf);
}

static const cli_command_t COMMANDS[] = {
    {"help", "list available commands", cmd_help},
    {"version", "firmware version and build date", cmd_version},
    {"status", "current bench state", cmd_status},
    {"start", "begin a measurement session", cmd_start},
    {"stop", "end the current session", cmd_stop},
    {"simulate", "select a simulated profile, or off", cmd_simulate},
    {"adc", "raw millivolts at the ADC pin, for calibration", cmd_adc},
    {"vdda", "the supply the ADC measures against, from VREFINT", cmd_vdda},
};

/* ------------------------------------------------------------------------ */
/* Entry points                                                             */
/* ------------------------------------------------------------------------ */

void app_stm32_init(void)
{
    platform_stm32_init();

    cli_init(&s_cli, COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0], cli_out,
             NULL);
    session_init(&s_session);
    sampler_init(&s_sampler, MAX_GAP_US);
    metrics_init(&s_metrics);
    platform_stm32_set_simulation(true, SIM_PROFILE_NORMAL);

    print_banner();
    out("type 'help' for commands\n");
}

void app_stm32_poll(void)
{
    console_poll();
    platform_watchdog_feed();

    if (!s_streaming) {
        return;
    }

    {
        uint64_t now = platform_time_us();

        /* Unsigned comparison, so this is written as a difference against the
           deadline rather than "now > due" — but both operands are absolute
           and monotonic, and platform_time_us() guarantees the second part.
           A clock that went backwards here would stall the stream forever
           rather than produce a wrong number, which is the better of the two
           failures and the reason the guarantee is in the interface. */
        if (now < s_next_sample_us) {
            return;
        }

        {
            power_sample_t sample;

            if (sampler_take(&s_sampler, &sample) == SAMPLE_OK) {
                char   buf[OUT_BUF];
                size_t n;

                metrics_add(&s_metrics, &sample);
                n = telemetry_encode_sample(buf, sizeof buf, &sample);
                if (n > 0u) {
                    platform_uart_write(buf, n);
                }
            }
        }

        s_next_sample_us += SAMPLE_PERIOD_US;

        /* If the loop fell so far behind that catching up would mean a burst
           of back-to-back samples, give up the lost periods instead. A burst
           would arrive with timestamps that say 2 ms apart while really being
           microseconds apart, and the host has no way to tell. */
        if (s_next_sample_us < now) {
            s_next_sample_us = now + SAMPLE_PERIOD_US;
        }
    }
}
