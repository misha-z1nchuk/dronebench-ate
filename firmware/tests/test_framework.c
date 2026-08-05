#include "test_framework.h"

#include <stdarg.h>
#include <stdio.h>

static const char *g_current_case = "(no case)";
static int g_checks = 0;
static int g_failures = 0;
static int g_cases = 0;
static int g_failed_in_case = 0;

void tf_begin_case(const char *name)
{
    if (g_cases > 0 && g_failed_in_case == 0) {
        printf("  ok   %s\n", g_current_case);
    }
    g_current_case = name;
    g_failed_in_case = 0;
    g_cases++;
}

void tf_count_check(void)
{
    g_checks++;
}

void tf_failf(const char *file, int line, const char *expr, const char *fmt, ...)
{
    va_list args;

    if (g_failed_in_case == 0) {
        printf("  FAIL %s\n", g_current_case);
    }
    g_failures++;
    g_failed_in_case++;

    printf("       %s:%d  %s\n", file, line, expr);
    printf("       ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

int tf_report(void)
{
    if (g_cases > 0 && g_failed_in_case == 0) {
        printf("  ok   %s\n", g_current_case);
    }

    printf("\n%d checks, %d failures, %d cases\n", g_checks, g_failures,
           g_cases);

    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
