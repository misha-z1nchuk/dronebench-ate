/* Every test suite gets one entry point, called from test_main.c. */
#ifndef DRONEBENCH_TESTS_H
#define DRONEBENCH_TESTS_H

void test_cli(void);
void test_platform(void);
void test_sampler(void);
void test_metrics(void);
void test_session(void);
void test_telemetry(void);

#endif /* DRONEBENCH_TESTS_H */
