#include <stdio.h>

#include "test_framework.h"
#include "tests.h"

int main(void)
{
    printf("DroneBench core tests\n\n");

    test_platform();
    test_cli();
    test_sampler();
    test_metrics();
    test_session();
    test_telemetry();

    return tf_report();
}
