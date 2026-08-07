#include <stdio.h>

#include "test_framework.h"
#include "tests.h"

int main(void)
{
    printf("DroneBench core tests\n\n");

    test_platform();
    test_cli();

    return tf_report();
}
