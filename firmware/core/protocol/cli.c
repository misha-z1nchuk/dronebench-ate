/*
 * Command line parser. The contract it implements is in cli.h; the tests in
 * firmware/tests/test_cli.c are that contract made executable, and win if the
 * two ever disagree.
 *
 * Rules this file keeps, none of them decoration:
 *
 *   - no malloc/free — the bench must behave identically on its first minute
 *     and its fifth hour
 *   - no strcpy, strcat, sprintf, gets — none of them know the size of what
 *     they are writing into
 *   - the line is split in place: separators are overwritten with NUL and argv
 *     points into the buffer, so nothing is copied and nothing is allocated
 *   - the buffer is cleared after every line, whichever way that line ended.
 *     A parser that stays poisoned after one bad line is a console that cannot
 *     accept `stop` while a motor is spinning.
 */
#include "dronebench/cli.h"
#include <string.h>

void cli_init(cli_t *cli, const cli_command_t *commands, size_t command_count,
              cli_output_fn output, void *output_ctx) {
  cli->commands = commands;
  cli->command_count = command_count;
  cli->output = output;
  cli->output_ctx = output_ctx;
  cli->len = 0;
  cli->line[0] = '\0';
  cli->overflow = false;
  cli->user = NULL;
}

static bool tokenize_line(cli_t *cli, int *argc, char **argv) {
  *argc = 0;
  char *token = cli->line;
  while (*token != '\0' && *argc < CLI_ARGS_MAX) {
    while (*token == ' ') {
      token++;
    }
    if (*token == '\0') {
      break;
    }
    argv[(*argc)++] = token;
    while (*token != '\0' && *token != ' ') {
      token++;
    }
    if (*token != '\0') {
      *token++ = '\0';
    }
  }
  while (*token == ' ') {
    token++;
  }
  return *token == '\0';
}

static void handle_line(cli_t *cli) {
  if (cli->overflow) {
    cli_write(cli, "ERR,line_too_long\n");
    cli->len = 0;
    cli->line[0] = '\0';
    cli->overflow = false;
    return;
  }

  if (cli->len == 0) {
    return;
  }

  int argc;
  char *argv[CLI_ARGS_MAX];

  if (!tokenize_line(cli, &argc, argv)) {
    cli_write(cli, "ERR,too_many_args\n");
    cli->len = 0;
    cli->line[0] = '\0';
    return;
  }

  if (argc == 0) {
    cli->len = 0;
    cli->line[0] = '\0';
    return;
  }

  const char *command_name = argv[0];
  for (size_t i = 0; i < cli->command_count; ++i) {
    if (strcmp(command_name, cli->commands[i].name) == 0) {
      cli->commands[i].handler(cli, argc, argv);
      cli->len = 0;
      cli->line[0] = '\0';
      return;
    }
  }

  cli_write(cli, "ERR,unknown_command,");
  cli_write(cli, command_name);
  cli_write(cli, "\n");

  cli->len = 0;
  cli->line[0] = '\0';
}

void cli_feed_char(cli_t *cli, char c) {
  if (c == '\n' || c == '\r') {
    handle_line(cli);
    return;
  }

  if (c == 0x08 || c == 0x7F) {
    if (cli->len > 0) {
      cli->len--;
      cli->line[cli->len] = '\0';
    }
    return;
  }

  if (c >= 0x20 && c <= 0x7E) {
    if (cli->len < CLI_LINE_MAX - 1) {
      cli->line[cli->len++] = c;
      cli->line[cli->len] = '\0';
    } else {
      cli->overflow = true;
    }
  }
}

void cli_feed(cli_t *cli, const char *data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    cli_feed_char(cli, data[i]);
  }
}

void cli_write(cli_t *cli, const char *text) {
  if (cli->output) {
    cli->output(cli->output_ctx, text);
  }
}

void cli_print_help(cli_t *cli) {
  for (size_t i = 0; i < cli->command_count; ++i) {
    cli_write(cli, cli->commands[i].name);
    cli_write(cli, " - ");
    cli_write(cli, cli->commands[i].help);
    cli_write(cli, "\n");
  }
}
