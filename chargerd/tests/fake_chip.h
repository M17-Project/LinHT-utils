// linht-chargerd tests: a fake BQ25792 behind struct bq_io
//
// Models what the daemon depends on: the 40 s watchdog and the bits it
// resets, WD_RST, HIZ turning the ADC off, a plug in clearing EN_HIZ,
// clear on read flags, ADC results that change while the ADC runs, and
// failing transfers.

#ifndef CHARGERD_FAKE_CHIP_H
#define CHARGERD_FAKE_CHIP_H

#include "bq25792.h"

struct fake_chip
{
    uint8_t r[BQ_NREGS];
    double t;                   // fake time, set by the test
    int host;                   // host mode, watchdog running
    double kick_t;
    int fail;                   // transfers fail while set
    unsigned writes[BQ_NREGS];  // writes per register
    unsigned wd_expiries;
    unsigned adc_tick;
    int adc_frozen;             // results stop changing (stale)
};

void fake_init(struct fake_chip *f, struct bq_io *io);
void fake_time(struct fake_chip *f, double t);
void fake_usb(struct fake_chip *f, int on);
void fake_adc(struct fake_chip *f, double vbat, double ibat);
void fake_set16(struct fake_chip *f, uint8_t reg, int value);

#endif
