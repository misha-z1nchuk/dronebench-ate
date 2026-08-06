/*
 * Specification for the CLI parser.
 *
 * These tests are the contract in cli.h expressed as executable checks. If a
 * test and the header ever disagree, the test wins — it is the one that runs.
 */
#include "dronebench/cli.h"

#include <stdio.h>
#include <string.h>

#include "test_framework.h"
#include "tests.h"

#define SINK_MAX     1024
#define REC_ARGS_MAX 16
#define REC_ARG_LEN  64

typedef struct {
    char   out[SINK_MAX];
    size_t out_len;

    int  dispatches;
    int  argc;
    char argv[REC_ARGS_MAX][REC_ARG_LEN];
} fixture_t;

static fixture_t g_fx;

static void sink(void *ctx, const char *text)
{
    fixture_t *fx = (fixture_t *)ctx;
    size_t     n  = strlen(text);

    if (fx->out_len + n + 1 < SINK_MAX) {
        memcpy(fx->out + fx->out_len, text, n);
        fx->out_len += n;
        fx->out[fx->out_len] = '\0';
    }
}

static void record(cli_t *cli, int argc, char **argv)
{
    (void)cli;

    g_fx.dispatches++;
    g_fx.argc = argc;
    for (int i = 0; i < argc && i < REC_ARGS_MAX; i++) {
        snprintf(g_fx.argv[i], REC_ARG_LEN, "%s", argv[i]);
    }
}

static const cli_command_t COMMANDS[] = {
    { "version", "print firmware version", record },
    { "status", "print bench status", record },
    { "set", "set a parameter", record },
};

static void setup(cli_t *cli)
{
    memset(&g_fx, 0, sizeof g_fx);
    memset(cli, 0, sizeof *cli);
    cli_init(cli, COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0], sink, &g_fx);
}

static void feed(cli_t *cli, const char *text)
{
    cli_feed(cli, text, strlen(text));
}

void test_cli(void)
{
    cli_t cli;

    TF_CASE("cli_write passes text through to the output callback");
    {
        setup(&cli);
        cli_write(&cli, "hello");
        CHECK_STR(g_fx.out, "hello");
    }

    TF_CASE("a known command is dispatched once the line ends");
    {
        setup(&cli);
        feed(&cli, "version\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_INT(g_fx.argc, 1);
        CHECK_STR(g_fx.argv[0], "version");
    }

    TF_CASE("nothing is dispatched before the line ends");
    {
        setup(&cli);
        feed(&cli, "version");
        CHECK_INT(g_fx.dispatches, 0);
        feed(&cli, "\n");
        CHECK_INT(g_fx.dispatches, 1);
    }

    TF_CASE("cli_feed_char accepts input one character at a time");
    {
        const char *text = "status\n";

        setup(&cli);
        for (const char *p = text; *p != '\0'; p++) {
            cli_feed_char(&cli, *p);
        }
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "status");
    }

    TF_CASE("arguments are split on spaces");
    {
        setup(&cli);
        feed(&cli, "set sample-rate 500\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_INT(g_fx.argc, 3);
        CHECK_STR(g_fx.argv[0], "set");
        CHECK_STR(g_fx.argv[1], "sample-rate");
        CHECK_STR(g_fx.argv[2], "500");
    }

    TF_CASE("runs of spaces collapse, leading and trailing ones are ignored");
    {
        setup(&cli);
        feed(&cli, "   set    sample-rate     500   \n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_INT(g_fx.argc, 3);
        CHECK_STR(g_fx.argv[0], "set");
        CHECK_STR(g_fx.argv[1], "sample-rate");
        CHECK_STR(g_fx.argv[2], "500");
    }

    TF_CASE("an empty line produces no output and no dispatch");
    {
        setup(&cli);
        feed(&cli, "\n");
        CHECK_INT(g_fx.dispatches, 0);
        CHECK_STR(g_fx.out, "");
    }

    TF_CASE("a line of only spaces produces no output and no dispatch");
    {
        setup(&cli);
        feed(&cli, "    \n");
        CHECK_INT(g_fx.dispatches, 0);
        CHECK_STR(g_fx.out, "");
    }

    TF_CASE("CRLF ends one line, not two");
    {
        setup(&cli);
        feed(&cli, "version\r\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.out, "");
    }

    TF_CASE("an unknown command is reported and dispatches nothing");
    {
        setup(&cli);
        feed(&cli, "nosuch arg\n");
        CHECK_INT(g_fx.dispatches, 0);
        CHECK_STR(g_fx.out, "ERR,unknown_command,nosuch\n");
    }

    TF_CASE("the buffer is cleared after every line, whatever happened to it");
    {
        setup(&cli);

        /* A rejected line must not leak into the next one. An operator types
           commands back to back; if a typo poisoned the buffer, the CLI would
           stay broken until reboot — with a motor possibly still spinning. */
        feed(&cli, "nosuch\n");
        memset(&g_fx, 0, sizeof g_fx);
        feed(&cli, "version\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");

        /* Same for a line that held only whitespace. */
        feed(&cli, "   \n");
        memset(&g_fx, 0, sizeof g_fx);
        feed(&cli, "status\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "status");

        /* And after a command that ran successfully. */
        feed(&cli, "set a b\n");
        memset(&g_fx, 0, sizeof g_fx);
        feed(&cli, "version\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_INT(g_fx.argc, 1);
        CHECK_STR(g_fx.argv[0], "version");

        /* And after a line rejected for having too many arguments. This is
           the dangerous one: the leftover text still starts with a valid
           command name, so a parser that keeps it does not report an error —
           it silently runs the *previous* command instead of the new one. */
        feed(&cli, "set a b c d e f g h\n");
        memset(&g_fx, 0, sizeof g_fx);
        feed(&cli, "version\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");
        CHECK_STR(g_fx.out, "");
    }

    TF_CASE("an over-long line is reported and the parser recovers");
    {
        char big[200];

        setup(&cli);
        memset(big, 'a', sizeof big);
        cli_feed(&cli, big, sizeof big);
        feed(&cli, "\n");

        CHECK_INT(g_fx.dispatches, 0);
        CHECK_STR(g_fx.out, "ERR,line_too_long\n");

        /* The next line must work normally — a parser that stays broken after
           one bad line would hang the bench mid-test. */
        memset(&g_fx, 0, sizeof g_fx);
        feed(&cli, "version\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.out, "");
    }

    TF_CASE("exactly CLI_ARGS_MAX tokens are accepted");
    {
        setup(&cli);
        feed(&cli, "set a b c d e f g\n"); /* 8 tokens */
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_INT(g_fx.argc, CLI_ARGS_MAX);
        CHECK_STR(g_fx.argv[CLI_ARGS_MAX - 1], "g");
    }

    TF_CASE("more than CLI_ARGS_MAX tokens is reported and dispatches nothing");
    {
        setup(&cli);
        feed(&cli, "set a b c d e f g h\n"); /* 9 tokens */
        CHECK_INT(g_fx.dispatches, 0);
        CHECK_STR(g_fx.out, "ERR,too_many_args\n");
    }

    TF_CASE("backspace removes the last character");
    {
        setup(&cli);
        feed(&cli, "versionX\b\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");
    }

    TF_CASE("DEL behaves as backspace");
    {
        setup(&cli);
        feed(&cli, "versionX\x7f\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");
    }

    TF_CASE("backspace on an empty line is harmless");
    {
        setup(&cli);
        feed(&cli, "\b\b\bversion\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");
    }

    TF_CASE("non-printable characters are ignored");
    {
        setup(&cli);
        feed(&cli, "ver\x01sio\x1bn\n");
        CHECK_INT(g_fx.dispatches, 1);
        CHECK_STR(g_fx.argv[0], "version");
    }

    TF_CASE("cli_print_help lists every command in table order");
    {
        setup(&cli);
        cli_print_help(&cli);
        CHECK_STR(g_fx.out,
                  "version - print firmware version\n"
                  "status - print bench status\n"
                  "set - set a parameter\n");
    }
}
