/*
 * TODO(day2): implement against the contract in cli.h.
 *
 * Run `make test` to see exactly what is expected — the tests in
 * firmware/tests/test_cli.c are the specification.
 *
 * Suggested order, each step turns some tests green:
 *
 *   1. cli_init, cli_write            — the plumbing
 *   2. cli_feed_char buffering        — printable chars, backspace, overflow
 *   3. end-of-line handling           — the three branches from the contract
 *   4. tokenizing                     — collapse spaces, respect CLI_ARGS_MAX
 *   5. dispatch                       — table lookup, unknown-command error
 *   6. cli_print_help
 *
 * Constraints that are part of the exercise, not decoration:
 *
 *   - no malloc/free
 *   - no strcpy, strcat, sprintf, gets
 *   - the line buffer is exactly CLI_LINE_MAX bytes and must always end up
 *     NUL-terminated before anyone reads it as a string
 *   - splitting in place is fine and is the usual approach: overwrite each
 *     separator with '\0' and keep a pointer to the token that follows
 */
#include "dronebench/cli.h"

#include <string.h>

void cli_init(cli_t *cli, const cli_command_t *commands, size_t command_count,
              cli_output_fn output, void *output_ctx)
{
    (void)cli;
    (void)commands;
    (void)command_count;
    (void)output;
    (void)output_ctx;
}

void cli_feed_char(cli_t *cli, char c)
{
    (void)cli;
    (void)c;
}

void cli_feed(cli_t *cli, const char *data, size_t size)
{
    (void)cli;
    (void)data;
    (void)size;
}

void cli_write(cli_t *cli, const char *text)
{
    (void)cli;
    (void)text;
}

void cli_print_help(cli_t *cli)
{
    (void)cli;
}
