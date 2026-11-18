// linht-chargerd: charger state machine and quiet leases. No I/O.

#ifndef CHARGERD_POLICY_H
#define CHARGERD_POLICY_H

#include "config.h"

enum pstate
{
    ST_ON_BATTERY,
    ST_CHARGING,
    ST_FULL,
    ST_FULL_HOLD,
    ST_QUIET,
    ST_CHARGE_OFF,
    ST_FAULT,
    ST_CHIP_ERROR,
};

enum pblock
{
    BLK_NONE,
    BLK_LOW_BATTERY,
    BLK_FAULT,
    BLK_CHIP,
};

enum perr
{
    PERR_OK = 0,
    PERR_LOW_BATTERY = -1,
    PERR_FAULT = -2,
    PERR_CHIP = -3,
    PERR_TOO_MANY = -4,
};

#define POLICY_MAX_LEASES 16
#define POLICY_OWNER_LEN 64

struct lease
{
    char owner[POLICY_OWNER_LEN];   // D-Bus unique name
    double expiry;                  // monotonic seconds
};

// One poll worth of input.
struct policy_in
{
    double now;             // monotonic seconds
    int chip_ok;            // 0 after 3 failed transfers in a row
    int usb;                // VBUS present and power good
    int fault;              // fault status or a recent fault flag
    int chg_done;           // CHG_STAT = done
    double vbat, ibat;      // V, A (positive = charging)
    double vreg;            // V
    double soc;             // %, NaN if unknown
    int soc_trusted;        // counter anchored at full
};

struct policy
{
    const struct chargerd_config *cfg;
    struct lease lease[POLICY_MAX_LEASES];
    int nlease;
    int charging_enabled;   // SetCharging, not stored
    int low_block;          // leases ended for a low battery, until resume
    int full;               // latched "battery full"
    double full_cond_since; // start of the second full rule, or -1
    double unfull_since;    // start of the leave-full condition, or -1
    int holding;            // full-hold active
    int full_event;         // set by policy_step when full was entered
    int leases_ended;       // set by policy_step when leases were ended
    enum pstate state;
    enum pblock block;
    int want_hiz, want_chg;
    struct policy_in last;
};

void policy_init(struct policy *p, const struct chargerd_config *cfg);

// Quiet(seconds) from owner. seconds = 0 releases. On success *granted is
// the clamped length. Returns PERR_OK or a negative perr.
int policy_quiet(struct policy *p, const char *owner, unsigned seconds, double now,
                 unsigned *granted);

// Release, also used when the owner left the bus. Returns 1 if a lease ended.
int policy_release(struct policy *p, const char *owner);
int policy_has_lease(const struct policy *p, const char *owner);

void policy_set_charging(struct policy *p, int enable);

// Expires leases and evaluates the state in the priority order of the spec.
void policy_step(struct policy *p, const struct policy_in *in);

// Quiet is held: at least one lease and the radio is quiet (HIZ on USB, or
// on battery where there is no converter to stop).
int policy_quiet_active(const struct policy *p);

const char *policy_state_name(enum pstate s);
const char *policy_block_name(enum pblock b);

#endif
