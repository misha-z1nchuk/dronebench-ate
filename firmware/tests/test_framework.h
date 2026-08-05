/*
 * Minimal test framework.
 *
 * Deliberately not a third-party framework: the whole thing is ~80 lines, has
 * no build dependencies, and cross-compiles anywhere. For a project whose
 * point is that the core runs everywhere, dragging in Unity or CMock would
 * cost more than it gives.
 *
 * A failing check does not abort the run — every check reports, so one broken
 * function does not hide the state of the rest.
 */
#ifndef DRONEBENCH_TEST_FRAMEWORK_H
#define DRONEBENCH_TEST_FRAMEWORK_H

#include <stdbool.h>
#include <string.h>

void tf_begin_case(const char *name);
void tf_count_check(void);
void tf_failf(const char *file, int line, const char *expr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Returns 0 if everything passed, 1 otherwise — use as main()'s exit code. */
int tf_report(void);

#define TF_CASE(name) tf_begin_case(name)

#define CHECK(expr)                                                           \
    do {                                                                      \
        tf_count_check();                                                     \
        if (!(expr)) {                                                        \
            tf_failf(__FILE__, __LINE__, #expr, "expected true");             \
        }                                                                     \
    } while (0)

#define CHECK_INT(actual, expected)                                           \
    do {                                                                      \
        tf_count_check();                                                     \
        long tf_actual_ = (long)(actual);                                     \
        long tf_expected_ = (long)(expected);                                 \
        if (tf_actual_ != tf_expected_) {                                     \
            tf_failf(__FILE__, __LINE__, #actual, "expected %ld, got %ld",    \
                     tf_expected_, tf_actual_);                               \
        }                                                                     \
    } while (0)

#define CHECK_STR(actual, expected)                                           \
    do {                                                                      \
        tf_count_check();                                                     \
        const char *tf_a_ = (actual);                                         \
        const char *tf_e_ = (expected);                                       \
        if (tf_a_ == NULL || strcmp(tf_a_, tf_e_) != 0) {                     \
            tf_failf(__FILE__, __LINE__, #actual,                             \
                     "expected \"%s\", got \"%s\"", tf_e_,                    \
                     tf_a_ ? tf_a_ : "(null)");                               \
        }                                                                     \
    } while (0)

/* Float comparison always needs a tolerance — never compare with ==. */
#define CHECK_NEAR(actual, expected, eps)                                     \
    do {                                                                      \
        tf_count_check();                                                     \
        double tf_a_ = (double)(actual);                                      \
        double tf_e_ = (double)(expected);                                    \
        double tf_d_ = tf_a_ - tf_e_;                                         \
        if (tf_d_ < 0) {                                                      \
            tf_d_ = -tf_d_;                                                   \
        }                                                                     \
        if (!(tf_d_ <= (double)(eps))) {                                      \
            tf_failf(__FILE__, __LINE__, #actual,                             \
                     "expected %g +/- %g, got %g", tf_e_, (double)(eps),      \
                     tf_a_);                                                  \
        }                                                                     \
    } while (0)

#endif /* DRONEBENCH_TEST_FRAMEWORK_H */
