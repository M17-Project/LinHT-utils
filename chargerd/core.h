// linht-chargerd: one poll of the charger. Reads the chip, runs the policy
// and the telemetry and writes the wanted bits back. No D-Bus here, so the
// unit tests can drive it with a fake chip.

#ifndef CHARGERD_CORE_H
#define CHARGERD_CORE_H

#include <stdint.h>

#include "bq25792.h"
#include "config.h"
#include "policy.h"
#include "telemetry.h"

#define CORE_KICK_S        10.0     // WD_RST period, the chip watchdog is 40 s
#define CORE_RETRY_S       5.0      // poll period in chip-error
#define CORE_FAIL_MAX      3        // failed polls in a row for chip-error
#define CORE_STALE_S       5.0      // ADC results unchanged this long
#define CORE_FAULT_HOLD_S  10.0     // a fault flag keeps "fault" this long
#define CORE_SAVE_S        900.0    // state file period while it changes
#define CORE_LOG_S         60.0     // repeated messages at most this often
#define CORE_MAX_FAULTS    16

struct core
{
    struct chargerd_config cfg;
    struct bq_io io;
    uint8_t regs[BQ_NREGS];         // last poll
    uint8_t flags_acc[6];           // OR of REG22 to REG27 since GetRegisters
    struct bq_meas m;
    int have_meas;
    int fails;
    double next_try;

    struct policy pol;
    struct telemetry tel;
    int pending_full;

    struct bq_want applied;         // last bits the daemon wrote
    int applied_valid;
    double last_kick;
    double last_fix_log, last_wd_log, last_stale_log;

    uint8_t adc_last[BQ_REG_TDIE + 2 - BQ_REG_IBUS];
    double adc_changed;
    double age;
    int valid;

    uint8_t fflag0_recent, fflag1_recent;
    double fault_until;

    struct tel_saved saved;
    int restore_pending;
    double last_save;

    int low_level;                  // 0 none, 1 low, 2 critical
    int lowbat_event;               // level to signal, 0 = none
};

extern int core_log_quiet;          // the tests turn the log off
void core_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// c->cfg and c->io must be set. Probes the chip, clears EN_HIZ, sets EN_IBAT,
// the ADC and PFM, kicks the watchdog and loads the state file.
// Returns 0 or a negative errno (-ENODEV if the chip is not a BQ25792).
int core_init(struct core *c, double now);

// The 1 s poll. wall is the Unix time for the cycle log.
void core_poll(struct core *c, double now, double wall);

// Runs the policy again on the last input and writes the result. Used
// right after Quiet, Release and SetCharging, so they act at once.
void core_apply(struct core *c, double now);

// Stock values back (section 4.6 of the spec), read back into rb[5] and
// saves the state file with the clean stop flag.
int core_stop(struct core *c, uint8_t rb[5]);

double core_now(void);    // CLOCK_MONOTONIC in seconds
int core_faults(const struct core *c, const char **out, int max);
int core_usb(const struct core *c);
int core_discharging(const struct core *c);

#endif
