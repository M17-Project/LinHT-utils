// linht-gnssd tests: minimal check macros

#ifndef GNSSD_CHECK_H
#define GNSSD_CHECK_H

#include <math.h>
#include <stdio.h>

extern int check_failed, check_count;

#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        check_count++;                                                           \
        if (!(cond))                                                             \
        {                                                                        \
            check_failed++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do                                                                           \
    {                                                                            \
        double a_ = (a), b_ = (b);                                               \
        check_count++;                                                           \
        if (!(fabs(a_ - b_) <= (tol)))                                           \
        {                                                                        \
            check_failed++;                                                      \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, expected %g\n",   \
                    __FILE__, __LINE__, #a, a_, b_);                             \
        }                                                                        \
    } while (0)

void test_nmea(void);
void test_core(void);
void test_timesync(void);

#endif
