/*
 * Line-oriented command interface.
 *
 * Fed one character at a time from an interrupt or a task; dispatches a
 * command once a full line arrives. No dynamic allocation, no unbounded
 * copies, no assumption that input is well-formed — the thing on the other end
 * of the wire is a human with a terminal, and humans paste garbage.
 *
 * ---------------------------------------------------------------------------
 * CONTRACT — the unit tests enforce every line of this. Read before writing
 * the implementation.
 * ---------------------------------------------------------------------------
 *
 * Character handling in cli_feed_char():
 *
 *   '\n' or '\r'      end of line (see below)
 *   0x08 or 0x7F      backspace: removes the last buffered character, if any
 *   0x20 .. 0x7E      printable: appended, or sets the overflow flag if the
 *                     buffer is full
 *   anything else     ignored
 *
 * End of line:
 *
 *   1. If the overflow flag is set: emit "ERR,line_too_long\n", clear the
 *      buffer and the flag, dispatch nothing.
 *   2. Otherwise if the buffer is empty: emit nothing, dispatch nothing.
 *      (This is what makes "\r\n" behave as one line ending, not two.)
 *   3. Otherwise: split the line and dispatch.
 *
 * Splitting:
 *
 *   Tokens are separated by spaces (0x20). Runs of spaces collapse; leading
 *   and trailing spaces are ignored. "  set   rate  500  " yields exactly
 *   {"set", "rate", "500"}.
 *
 *   More than CLI_ARGS_MAX tokens: emit "ERR,too_many_args\n", dispatch
 *   nothing.
 *
 * Dispatch:
 *
 *   argv[0] is matched against the command table with strcmp. No match: emit
 *   "ERR,unknown_command," followed by argv[0] and "\n", dispatch nothing.
 *   Match: call the handler with argc and argv.
 *
 *   argv points into the cli's own line buffer and is valid only for the
 *   duration of the handler call. A handler that needs to keep a value must
 *   copy it.
 *
 * The buffer is reset after every line, whichever branch was taken. A parser
 * that cannot recover from bad input is a parser that hangs the bench in the
 * middle of a test.
 */
#ifndef DRONEBENCH_CLI_H
#define DRONEBENCH_CLI_H

#include <stdbool.h>
#include <stddef.h>

/* Including the terminating NUL. */
#define CLI_LINE_MAX 128
#define CLI_ARGS_MAX 8

typedef struct cli cli_t;

/* Emits text to the transport. Called with the ctx given to cli_init(). */
typedef void (*cli_output_fn)(void *ctx, const char *text);

/* argv[0] is the command name itself, so argc is always >= 1. */
typedef void (*cli_handler_fn)(cli_t *cli, int argc, char **argv);

typedef struct {
    const char    *name;
    const char    *help;
    cli_handler_fn handler;
} cli_command_t;

struct cli {
    char   line[CLI_LINE_MAX];
    size_t len;
    bool   overflow;

    const cli_command_t *commands;
    size_t               command_count;

    cli_output_fn output;
    void         *output_ctx;

    /* Free for the application: session state, config, whatever handlers
       need. The CLI itself never touches it. */
    void *user;
};

/*
 * commands must outlive the cli — the table is normally a file-scope
 * static const array, not a stack local.
 */
void cli_init(cli_t *cli, const cli_command_t *commands, size_t command_count,
              cli_output_fn output, void *output_ctx);

void cli_feed_char(cli_t *cli, char c);
void cli_feed(cli_t *cli, const char *data, size_t size);

/* Passes text straight to the output callback. */
void cli_write(cli_t *cli, const char *text);

/*
 * Writes one line per registered command, in table order, as:
 *
 *     name - help\n
 */
void cli_print_help(cli_t *cli);

#endif /* DRONEBENCH_CLI_H */
