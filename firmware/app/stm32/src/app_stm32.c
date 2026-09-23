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

#include "dronebench/calibration.h"
#include "dronebench/cli.h"
#include "dronebench/current_sensor.h"
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

/* Longest a single sample took, from reading the clock to the end of its
   transmit. The period budget in platform_stm32.c is arithmetic with an
   estimate in it; this is the measurement that settles it. Above 2000 the
   stream cannot keep 500 Hz and `gaps` will say so too. */
static uint32_t s_worst_us;

/* Day 12 points collected by `cal add`, not yet fitted. */
static calibration_point_t s_cal_points[CALIBRATION_MAX_POINTS];
static uint32_t            s_cal_count;

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
             ",failed=%" PRIu32 ",gaps=%" PRIu32 ",worst_us=%" PRIu32 "\n",
             session_state_name(&s_session),
             /*
              * Deliberately narrowed to 32 bits. newlib-nano drops %llu
              * unless the image is linked with -u _printf_long_long, and it
              * drops it silently: the board printed "uptime_s=lu" for a week
              * before anyone ran it on hardware. Seconds overflow uint32_t
              * after 136 years, so the 64-bit formatter buys nothing here.
              */
             (uint32_t)(platform_time_us() / 1000000u),
             s_sampler.accepted, s_sampler.sensor_failures, s_sampler.gaps,
             s_worst_us);
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
    s_worst_us       = 0u;
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

/* ------------------------------------------------------------------------ */
/* Days 12 and 13: calibration on the bench                                 */
/*                                                                          */
/* These read the hardware whatever `simulate` says. Calibrating against the */
/* simulator would be calibrating nothing.                                  */
/* ------------------------------------------------------------------------ */

/* An optional count argument, 1..ADC_MAX_READS. False on anything else. */
static bool parse_count(int argc, char **argv, int index, long fallback,
                        long *count)
{
    char *end;
    long  value;

    if (argc <= index) {
        *count = fallback;
        return true;
    }
    value = strtol(argv[index], &end, 10);
    if (*end != '\0' || value < 1 || value > ADC_MAX_READS) {
        return false;
    }
    *count = value;
    return true;
}

/* Running mean and spread of a burst of readings. */
typedef struct {
    float sum;
    float lo;
    float hi;
    long  n;
} spread_t;

static void spread_add(spread_t *s, float x)
{
    if (s->n == 0 || x < s->lo) s->lo = x;
    if (s->n == 0 || x > s->hi) s->hi = x;
    s->sum += x;
    s->n++;
}

static float spread_mean(const spread_t *s) { return s->sum / (float)s->n; }

/*
 * The wiring check. What the multimeter should agree with, node by node,
 * before anything is calibrated: A is the battery divider, B and C are the
 * ACS724's OUT and VCC. Day 11 on the ESP32 read B = 1.20 V and C = 1.58 V
 * with the sensor at 4.80 V; the same wires on this board should say the same.
 */
static void cmd_nodes(cli_t *cli, int argc, char **argv)
{
    char                    buf[OUT_BUF];
    float                   a_mv;
    float                   out_mv;
    float                   vcc_mv;
    float                   vcc_v = 0.0f;
    float                   ratio = 0.0f;
    const current_sensor_t *sensor = platform_stm32_current_sensor();
    bool                    ratio_ok;

    (void)argc;
    (void)argv;

    if (!platform_adc_read_millivolts(&a_mv) ||
        !platform_stm32_read_current_nodes(&out_mv, &vcc_mv)) {
        cli_write(cli, "ERR,nodes,no_reading\n");
        return;
    }

    (void)current_sensor_vcc_v(sensor, vcc_mv, &vcc_v);
    ratio_ok = current_sensor_ratio(sensor, out_mv, vcc_mv, &ratio);

    /* ratio=none rather than a number when the sensor's own checks refuse
       it: VCC off or out of range, or OUT at the clip. */
    if (ratio_ok) {
        snprintf(buf, sizeof buf,
                 "OK,nodes,a_mv=%.1f,b_mv=%.1f,c_mv=%.1f,vcc_v=%.3f,ratio=%.5f,"
                 "vdda_mv=%.1f\n",
                 (double)a_mv, (double)out_mv, (double)vcc_mv, (double)vcc_v,
                 (double)ratio, (double)platform_stm32_vdda_mv());
    } else {
        snprintf(buf, sizeof buf,
                 "OK,nodes,a_mv=%.1f,b_mv=%.1f,c_mv=%.1f,vcc_v=%.3f,ratio=none,"
                 "vdda_mv=%.1f\n",
                 (double)a_mv, (double)out_mv, (double)vcc_mv, (double)vcc_v,
                 (double)platform_stm32_vdda_mv());
    }
    cli_write(cli, buf);
}

static void print_calibration(cli_t *cli, const char *verb,
                              const calibration_t *cal)
{
    char buf[OUT_BUF];

    snprintf(buf, sizeof buf,
             "OK,cal,%s,valid=%d,scale_v_per_mv=%.8f,offset_v=%.5f,"
             "rms_mv=%.2f,max_mv=%.2f,points=%" PRIu32 ",pending=%" PRIu32 "\n",
             verb, cal->valid ? 1 : 0, (double)cal->scale_v_per_mv,
             (double)cal->offset_v, (double)(cal->rms_residual_v * 1000.0f),
             (double)(cal->max_residual_v * 1000.0f), cal->points, s_cal_count);
    cli_write(cli, buf);
}

/* Readings averaged per calibration point: 64 reads of 32 conversions each,
   about 25 ms, so the point is taken while the operator is still looking at
   the same number on the meter. */
#define CAL_POINT_READS 64

static void cal_add(cli_t *cli, const char *arg)
{
    char     buf[OUT_BUF];
    char    *end;
    float    ref_v = strtof(arg, &end);
    spread_t s     = {0};
    long     i;

    /* A typo is the likeliest way to ruin a fit, and the least likely to be
       noticed afterwards, so the value is checked as hard as it can be. The
       window is the battery's plausible range with margin either side. */
    if (*end != '\0' || !(ref_v >= 0.5f && ref_v <= 6.0f)) {
        cli_write(cli, "ERR,cal,bad_reference\n");
        return;
    }
    if (s_cal_count >= CALIBRATION_MAX_POINTS) {
        cli_write(cli, "ERR,cal,full\n");
        return;
    }

    for (i = 0; i < CAL_POINT_READS; i++) {
        float mv;

        /* All or nothing: a point averaged over the reads that happened to
           succeed is not the point the operator was looking at. */
        if (!platform_adc_read_millivolts(&mv)) {
            cli_write(cli, "ERR,cal,no_reading\n");
            return;
        }
        spread_add(&s, mv);
    }

    s_cal_points[s_cal_count].measured_mv = spread_mean(&s);
    s_cal_points[s_cal_count].reference_v = ref_v;
    s_cal_count++;

    /* pp is the thing to watch: a point where the node moved while it was
       being read is a point where the meter and the ADC saw different
       voltages. A few tenths of a millivolt is noise; more is a loose wire. */
    snprintf(buf, sizeof buf,
             "OK,cal,add,i=%" PRIu32 ",ref_v=%.4f,mv=%.2f,pp=%.2f\n",
             s_cal_count, (double)ref_v, (double)spread_mean(&s),
             (double)(s.hi - s.lo));
    cli_write(cli, buf);
}

static void cal_fit(cli_t *cli)
{
    char          buf[OUT_BUF];
    calibration_t cal;
    uint32_t      i;

    calibration_init(&cal);
    if (!calibration_fit(&cal, s_cal_points, s_cal_count)) {
        /* Fewer than three points, two identical readings, or a negative
           slope — calibration.h lists the refusals. */
        cli_write(cli, "ERR,cal,fit_refused\n");
        return;
    }
    platform_stm32_set_voltage_calibration(&cal);

    /* Per-point residuals: the table for PROGRESS.md, and the only way to see
       whether the miss is spread evenly (noise) or grows toward one end
       (non-linearity, the plan's risk 3). */
    for (i = 0; i < s_cal_count; i++) {
        float fit_v = 0.0f;

        (void)calibration_apply(&cal, s_cal_points[i].measured_mv, &fit_v);
        snprintf(buf, sizeof buf,
                 "OK,cal,point,i=%" PRIu32 ",ref_v=%.4f,mv=%.2f,fit_v=%.4f,"
                 "res_mv=%+.2f\n",
                 i + 1u, (double)s_cal_points[i].reference_v,
                 (double)s_cal_points[i].measured_mv, (double)fit_v,
                 (double)((s_cal_points[i].reference_v - fit_v) * 1000.0f));
        cli_write(cli, buf);
    }

    print_calibration(cli, "fit", &cal);

    /* Ready to paste into bench_calibration.h, so the numbers that go into
       the commit are the ones the board printed, not ones retyped. */
    snprintf(buf, sizeof buf,
             "#define BENCH_VOLTAGE_SCALE_V_PER_MV %.8ff\n"
             "#define BENCH_VOLTAGE_OFFSET_V       %.6ff\n",
             (double)cal.scale_v_per_mv, (double)cal.offset_v);
    cli_write(cli, buf);
    snprintf(buf, sizeof buf,
             "#define BENCH_VOLTAGE_RMS_RESIDUAL_V %.6ff\n"
             "#define BENCH_VOLTAGE_MAX_RESIDUAL_V %.6ff\n"
             "#define BENCH_VOLTAGE_POINTS         %" PRIu32 "u\n",
             (double)cal.rms_residual_v, (double)cal.max_residual_v, cal.points);
    cli_write(cli, buf);
}

/*
 * Day 12, end to end:
 *
 *   cal add 4.203    multimeter reads 4.203 V at the divider input; the board
 *                    pairs it with its own reading of the node, now
 *   cal undo         drop the last point (a typo, a wire that moved)
 *   cal fit          least squares over the points, applied immediately,
 *                    residual per point, and the lines for the header
 *   cal clear        drop the pending points
 *   cal              the calibration in use
 */
static void cmd_cal(cli_t *cli, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        print_calibration(cli, "show", platform_stm32_voltage_calibration());
        return;
    }
    if (strcmp(argv[1], "add") == 0 && argc == 3) {
        cal_add(cli, argv[2]);
        return;
    }
    if (strcmp(argv[1], "undo") == 0) {
        if (s_cal_count > 0u) {
            s_cal_count--;
        }
        print_calibration(cli, "undo", platform_stm32_voltage_calibration());
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        s_cal_count = 0u;
        print_calibration(cli, "clear", platform_stm32_voltage_calibration());
        return;
    }
    if (strcmp(argv[1], "fit") == 0) {
        cal_fit(cli);
        return;
    }
    cli_write(cli, "ERR,cal,usage: cal [show|add <volts>|undo|clear|fit]\n");
}

/* Calibrated battery volts, one-shot. What the stream would say, without a
   session: the check against the multimeter after `cal fit`. */
static void cmd_volts(cli_t *cli, int argc, char **argv)
{
    char                 buf[OUT_BUF];
    const calibration_t *cal = platform_stm32_voltage_calibration();
    spread_t             s   = {0};
    long                 count;
    long                 i;

    if (!parse_count(argc, argv, 1, 16, &count)) {
        cli_write(cli, "ERR,volts,bad_count\n");
        return;
    }
    if (!cal->valid) {
        cli_write(cli, "ERR,volts,not_calibrated\n");
        return;
    }

    for (i = 0; i < count; i++) {
        float mv;
        float v;

        if (platform_adc_read_millivolts(&mv) && calibration_apply(cal, mv, &v)) {
            spread_add(&s, v);
        }
    }
    if (s.n == 0) {
        cli_write(cli, "ERR,volts,no_reading\n");
        return;
    }

    snprintf(buf, sizeof buf, "OK,volts,v=%.4f,n=%ld,pp_mv=%.2f,failed=%ld\n",
             (double)spread_mean(&s), s.n, (double)((s.hi - s.lo) * 1000.0f),
             count - s.n);
    cli_write(cli, buf);
}

/* Reads averaged for a zero: about 0.3 ms each, so 512 is ~150 ms. */
#define ZERO_READS 512

/*
 * Day 13: the ratio OUT / VCC with no current through the sensor, stored as
 * the zero. Taken as a ratio rather than volts so it keeps holding when USB
 * moves — current_sensor.h has the argument.
 *
 * Nothing on the power terminals when this runs. The sensor cannot tell a
 * zero from a steady current, and current_sensor_set_zero() only catches a
 * large one.
 */
static void cmd_zero(cli_t *cli, int argc, char **argv)
{
    char              buf[OUT_BUF];
    current_sensor_t *sensor = platform_stm32_current_sensor();
    spread_t          s      = {0};
    float             vcc_sum = 0.0f;
    long              i;

    (void)argc;
    (void)argv;

    for (i = 0; i < ZERO_READS; i++) {
        float out_mv;
        float vcc_mv;
        float ratio;
        float vcc_v;

        if (!platform_stm32_read_current_nodes(&out_mv, &vcc_mv) ||
            !current_sensor_ratio(sensor, out_mv, vcc_mv, &ratio) ||
            !current_sensor_vcc_v(sensor, vcc_mv, &vcc_v)) {
            /* Refused whole, same reason as a calibration point. The most
               likely cause is the VCC wire, and `nodes` will show it. */
            cli_write(cli, "ERR,zero,no_reading\n");
            return;
        }
        spread_add(&s, ratio);
        vcc_sum += vcc_v;
    }

    if (!current_sensor_set_zero(sensor, spread_mean(&s))) {
        /* Further from 0.5 than any healthy offset: current is flowing, or
           OUT and VCC are on each other's pins. */
        snprintf(buf, sizeof buf, "ERR,zero,implausible,ratio=%.5f\n",
                 (double)spread_mean(&s));
        cli_write(cli, buf);
        return;
    }

    /* The spread in amps is the noise of a single read, which is what the
       stream will carry — worth seeing next to the zero it was averaged into. */
    snprintf(buf, sizeof buf,
             "OK,zero,ratio=%.5f,offset_a=%+.3f,pp_a=%.3f,vcc_v=%.3f,n=%ld\n"
             "#define BENCH_CURRENT_ZERO_RATIO %.5ff\n",
             (double)spread_mean(&s),
             (double)((spread_mean(&s) - 0.5f) * sensor->amps_per_ratio),
             (double)((s.hi - s.lo) * sensor->amps_per_ratio),
             (double)(vcc_sum / (float)s.n), s.n, (double)spread_mean(&s));
    cli_write(cli, buf);
}

/* Current, one-shot, with VCC beside it so a drifting supply is visible. */
static void cmd_amps(cli_t *cli, int argc, char **argv)
{
    char                    buf[OUT_BUF];
    const current_sensor_t *sensor = platform_stm32_current_sensor();
    spread_t                s      = {0};
    float                   vcc_v  = 0.0f;
    long                    count;
    long                    i;

    if (!parse_count(argc, argv, 1, 64, &count)) {
        cli_write(cli, "ERR,amps,bad_count\n");
        return;
    }
    if (!sensor->zeroed) {
        cli_write(cli, "ERR,amps,not_zeroed\n");
        return;
    }

    for (i = 0; i < count; i++) {
        float out_mv;
        float vcc_mv;
        float a;

        if (platform_stm32_read_current_nodes(&out_mv, &vcc_mv) &&
            current_sensor_amps(sensor, out_mv, vcc_mv, &a)) {
            spread_add(&s, a);
            (void)current_sensor_vcc_v(sensor, vcc_mv, &vcc_v);
        }
    }
    if (s.n == 0) {
        cli_write(cli, "ERR,amps,no_reading\n");
        return;
    }

    snprintf(buf, sizeof buf,
             "OK,amps,a=%+.3f,n=%ld,pp_a=%.3f,vcc_v=%.3f,failed=%ld\n",
             (double)spread_mean(&s), s.n, (double)(s.hi - s.lo),
             (double)vcc_v, count - s.n);
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
    {"nodes", "millivolts at all three divider nodes, for wiring", cmd_nodes},
    {"cal", "day 12: cal add <volts> | undo | clear | fit | show", cmd_cal},
    {"volts", "calibrated battery volts [reads]", cmd_volts},
    {"zero", "day 13: store the no-current ratio of the ACS724", cmd_zero},
    {"amps", "calibrated current [reads]", cmd_amps},
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
            uint64_t       took_us;

            if (sampler_take(&s_sampler, &sample) == SAMPLE_OK) {
                char   buf[OUT_BUF];
                size_t n;

                metrics_add(&s_metrics, &sample);
                n = telemetry_encode_sample(buf, sizeof buf, &sample);
                if (n > 0u) {
                    platform_uart_write(buf, n);
                }
            }

            /* From the deadline check, so it includes all four ADC channels,
               the encode and the blocking transmit — everything the period
               has to fit. */
            took_us = platform_time_us() - now;
            if (took_us > s_worst_us) {
                s_worst_us = (took_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)took_us;
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
