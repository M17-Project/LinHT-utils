// linht-gnssd tests: runner

#include <stdio.h>

#include "check.h"
#include "core.h"

int check_failed, check_count;

int main(void)
{
    core_log_quiet = 1;
    test_nmea();
    test_core();
    test_timesync();
    printf("%d checks, %d failed\n", check_count, check_failed);
    return check_failed != 0;
}
