// linht-chargerd tests: state machine and leases

#include <math.h>

#include "check.h"
#include "policy.h"

static struct chargerd_config cfg;
static struct policy p;
static struct policy_in in;

static void setup(void)
{
    config_defaults(&cfg);
    policy_init(&p, &cfg);
    in = (struct policy_in){ .now = 0, .chip_ok = 1, .usb = 1, .vbat = 7.9, .ibat = 0.5,
                             .vreg = 8.4, .soc = 60, .soc_trusted = 1 };
    policy_step(&p, &in);
}

static void step(double now)
{
    in.now = now;
    policy_step(&p, &in);
}

static void lease_basics(void)
{
    unsigned g;
    setup();
    CHECK(p.state == ST_CHARGING && !p.want_hiz && p.want_chg);

    CHECK(policy_quiet(&p, ":1.10", 1000, 0, &g) == PERR_OK);
    CHECK(g == 300);                            // clamped to quiet_max_s
    step(1);
    CHECK(p.state == ST_QUIET && p.want_hiz && p.want_chg);
    CHECK(policy_quiet_active(&p));

    // Renewing replaces the expiry, one lease per name.
    CHECK(policy_quiet(&p, ":1.10", 60, 250, &g) == PERR_OK && g == 60);
    CHECK(p.nlease == 1);
    step(305);
    CHECK(p.state == ST_QUIET);
    step(310);                                  // 250 + 60
    CHECK(p.state == ST_CHARGING && !p.want_hiz && p.nlease == 0);

    // Quiet(0) and Release end the lease at once.
    CHECK(policy_quiet(&p, ":1.11", 100, 400, &g) == PERR_OK);
    CHECK(policy_quiet(&p, ":1.11", 0, 401, &g) == PERR_OK && g == 0);
    step(401);
    CHECK(p.state == ST_CHARGING);
    CHECK(policy_quiet(&p, ":1.11", 100, 402, &g) == PERR_OK);
    CHECK(policy_release(&p, ":1.11") == 1);
    CHECK(policy_release(&p, ":1.11") == 0);
}

static void two_holders(void)
{
    unsigned g;
    setup();
    CHECK(policy_quiet(&p, ":1.20", 100, 0, &g) == PERR_OK);
    CHECK(policy_quiet(&p, ":1.21", 50, 0, &g) == PERR_OK);
    step(1);
    CHECK(p.nlease == 2 && p.state == ST_QUIET);

    // The holder of the longer lease leaves the bus (crash), the other
    // keeps quiet until its own expiry.
    policy_release(&p, ":1.20");
    step(2);
    CHECK(p.nlease == 1 && p.state == ST_QUIET);
    step(50);
    CHECK(p.nlease == 0 && p.state == ST_CHARGING && !p.want_hiz);
}

static void low_battery(void)
{
    unsigned g;
    setup();
    in.soc = 24;                                // below quiet_min_soc 25
    step(1);
    CHECK(p.block == BLK_LOW_BATTERY);
    CHECK(policy_quiet(&p, ":1.30", 60, 1, &g) == PERR_LOW_BATTERY && g == 0);

    in.soc = 30;
    step(2);
    CHECK(p.block == BLK_NONE);
    CHECK(policy_quiet(&p, ":1.30", 300, 2, &g) == PERR_OK);
    step(3);
    CHECK(p.state == ST_QUIET);

    // Below quiet_end_soc: all leases end, charging resumes.
    in.soc = 19.5;
    step(4);
    CHECK(p.nlease == 0 && p.state == ST_CHARGING && !p.want_hiz && p.leases_ended);
    CHECK(p.block == BLK_LOW_BATTERY);

    // Refused until quiet_resume_soc, even above quiet_min_soc.
    in.soc = 35;
    step(5);
    CHECK(policy_quiet(&p, ":1.30", 60, 5, &g) == PERR_LOW_BATTERY);
    in.soc = 40;
    step(6);
    CHECK(p.block == BLK_NONE);
    CHECK(policy_quiet(&p, ":1.30", 60, 6, &g) == PERR_OK);

    // SoC not trusted: the voltage limits apply instead.
    setup();
    in.soc_trusted = 0;
    in.soc = 50;
    in.vbat = 7.25;                             // below quiet_min_vbat 7.3
    step(1);
    CHECK(policy_quiet(&p, ":1.31", 60, 1, &g) == PERR_LOW_BATTERY);
    in.vbat = 7.4;
    step(2);
    CHECK(policy_quiet(&p, ":1.31", 60, 2, &g) == PERR_OK);
    in.vbat = 7.05;                             // below quiet_end_vbat 7.1
    in.soc = 3;                                 // the voltage table agrees
    step(3);
    CHECK(p.nlease == 0 && p.low_block);

    // On battery quiet is free: no limits.
    setup();
    in.usb = 0;
    in.soc = 5;
    step(1);
    CHECK(p.state == ST_ON_BATTERY && p.block == BLK_NONE);
    CHECK(policy_quiet(&p, ":1.32", 60, 1, &g) == PERR_OK);
    step(2);
    CHECK(policy_quiet_active(&p) && !p.want_hiz);
}

static void usb_unplug(void)
{
    unsigned g;
    setup();
    CHECK(policy_quiet(&p, ":1.40", 300, 0, &g) == PERR_OK);
    step(1);
    CHECK(p.state == ST_QUIET && p.want_hiz);
    in.usb = 0;
    step(2);
    CHECK(p.state == ST_ON_BATTERY && !p.want_hiz && p.nlease == 1 && policy_quiet_active(&p));
    in.usb = 1;
    step(3);
    CHECK(p.state == ST_QUIET && p.want_hiz);
}

static void fault_and_chip(void)
{
    unsigned g;
    setup();
    CHECK(policy_quiet(&p, ":1.50", 300, 0, &g) == PERR_OK);
    in.fault = 1;
    step(1);
    CHECK(p.state == ST_FAULT && !p.want_hiz && p.nlease == 0 && p.block == BLK_FAULT);
    CHECK(policy_quiet(&p, ":1.50", 300, 1, &g) == PERR_FAULT);
    in.fault = 0;
    step(2);
    CHECK(p.state == ST_CHARGING);

    // Chip error: leases are kept, new ones refused, wanted bits unchanged.
    CHECK(policy_quiet(&p, ":1.51", 300, 2, &g) == PERR_OK);
    step(3);
    in.chip_ok = 0;
    step(4);
    CHECK(p.state == ST_CHIP_ERROR && p.nlease == 1 && p.want_hiz);
    CHECK(policy_quiet(&p, ":1.52", 300, 4, &g) == PERR_CHIP);
    in.chip_ok = 1;
    step(5);
    CHECK(p.state == ST_QUIET);
}

static void charge_off(void)
{
    unsigned g;
    setup();
    policy_set_charging(&p, 0);
    step(1);
    CHECK(p.state == ST_CHARGE_OFF && !p.want_chg && !p.want_hiz);
    CHECK(policy_quiet(&p, ":1.60", 30, 1, &g) == PERR_OK);
    step(2);
    CHECK(p.state == ST_QUIET && p.want_hiz && !p.want_chg);
    policy_release(&p, ":1.60");
    policy_set_charging(&p, 1);
    step(3);
    CHECK(p.state == ST_CHARGING && p.want_chg);
}

static void full(void)
{
    setup();
    // Rule 1: CHG_STAT done.
    in.chg_done = 1;
    step(1);
    CHECK(p.state == ST_FULL && p.full_event);
    step(2);
    CHECK(p.state == ST_FULL && !p.full_event);

    // Rule 2: near VREG with a small current for 60 s (weak USB, VINDPM).
    setup();
    in.vbat = 8.38;
    in.ibat = 0.15;
    step(10);
    step(69);
    CHECK(p.state == ST_CHARGING);
    step(70);
    CHECK(p.state == ST_FULL && p.full_event);

    // Recharge: a real current for 10 s leaves full.
    in.ibat = 0.8;
    in.vbat = 8.2;
    step(71);
    CHECK(p.state == ST_FULL);
    step(81);
    CHECK(p.state == ST_CHARGING);

    // full-hold: HIZ until the SoC falls below full_hold_resume_soc.
    setup();
    cfg.full_hold = 1;
    in.chg_done = 1;
    in.soc = 100;
    step(1);
    CHECK(p.state == ST_FULL_HOLD && p.want_hiz);
    in.chg_done = 0;
    in.soc = 91;
    step(100);
    CHECK(p.state == ST_FULL_HOLD);
    in.soc = 89;
    step(200);
    CHECK(p.state == ST_CHARGING && !p.want_hiz && !p.full);
}

void test_policy(void)
{
    lease_basics();
    two_holders();
    low_battery();
    usb_unplug();
    fault_and_chip();
    charge_off();
    full();
}
