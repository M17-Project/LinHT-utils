// linht-chargerd tests: the poll against a fake chip (register logic,
// watchdog, quiet, USB plug events, chip errors, stop)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fake_chip.h"
#include "store.h"

static struct fake_chip f;
static struct core c;
static double now;
static char dir[128];

static void poll_for(int seconds)
{
    for (int k = 0; k < seconds; k++)
    {
        now += 1;
        fake_time(&f, now);
        core_poll(&c, now, 1.7e9 + now);
    }
}

static void setup(void)
{
    memset(&c, 0, sizeof(c));
    config_defaults(&c.cfg);
    c.cfg.charge_voltage_v = 8.40;              // the Rev C numbers below
    snprintf(c.cfg.state_dir, sizeof(c.cfg.state_dir), "%s", dir);
    fake_init(&f, &c.io);
    now = 100;
    fake_time(&f, now);
}

// Writes only to the registers of section 4.1 of the spec.
static int only_allowed_writes(void)
{
    for (int r = 0; r < BQ_NREGS; r++)
        if (f.writes[r] && r != BQ_REG_CTRL0 && r != BQ_REG_CTRL1 && r != BQ_REG_CTRL3 &&
            r != BQ_REG_CTRL5 && r != BQ_REG_ADC && r != BQ_REG_VREG && r != BQ_REG_VREG + 1)
            return 0;
    return 1;
}

static void start_and_stop(void)
{
    uint8_t rb[5];
    setup();
    f.r[BQ_REG_CTRL0] |= BQ_EN_HIZ;             // left in HIZ by a crashed run
    CHECK(core_init(&c, now) == 0);
    CHECK(f.r[BQ_REG_CTRL0] == 0xa2);           // HIZ cleared, EN_CHG on
    CHECK(f.r[BQ_REG_CTRL3] == BQ_PFM_FWD_DIS); // pfm = off
    CHECK(f.r[BQ_REG_CTRL5] == (0x16 | BQ_EN_IBAT));
    CHECK(f.r[BQ_REG_ADC] == BQ_ADC_EN);        // continuous, 15 bit
    CHECK(f.writes[BQ_REG_CTRL1] == 1);         // one kick
    CHECK(f.r[BQ_REG_CTRL1] == 0x85);           // WD_RST cleared itself

    poll_for(3);
    CHECK(c.pol.state == ST_CHARGING);
    CHECK_NEAR(c.m.vbat, 7.9, 1e-9);
    CHECK_NEAR(c.m.ibat, 0.5, 1e-9);
    CHECK_NEAR(c.m.vbus, 5.0, 1e-9);
    CHECK_NEAR(c.m.tdie, 30.0, 1e-9);
    CHECK_NEAR(c.m.vreg, 8.4, 1e-9);
    CHECK(c.valid && c.age < 1.5);
    CHECK(only_allowed_writes());

    CHECK(core_stop(&c, rb) == 0);
    CHECK(rb[0] == 0xa2 && rb[1] == 0x85 && rb[2] == 0x00 && rb[3] == 0x16 && rb[4] == 0x30);
    CHECK(only_allowed_writes());

    struct tel_saved s;
    CHECK(store_load(dir, &s) == 0);
    CHECK(s.clean_stop == 1);
    CHECK_NEAR(s.cap_wh, 14.8, 1e-6);

    // pfm = on writes PFM_FWD_DIS = 0.
    setup();
    c.cfg.pfm = 1;
    f.r[BQ_REG_CTRL3] = BQ_PFM_FWD_DIS;
    CHECK(core_init(&c, now) == 0);
    CHECK(f.r[BQ_REG_CTRL3] == 0);

    // Not a BQ25792: refuse, no writes.
    setup();
    f.r[BQ_REG_PART] = 0x19;
    CHECK(core_init(&c, now) < 0);
    for (int r = 0; r < BQ_NREGS; r++)
        CHECK(f.writes[r] == 0);
}

static void watchdog(void)
{
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(600);
    CHECK(f.wd_expiries == 0);
    CHECK(f.writes[BQ_REG_CTRL1] >= 60 && f.writes[BQ_REG_CTRL1] <= 62);
    CHECK(f.r[BQ_REG_CTRL5] & BQ_EN_IBAT);

    // The daemon was stopped for 41 s (SIGSTOP): the chip falls back to
    // default mode. The next poll sets everything again.
    now += 41;
    fake_time(&f, now);
    f.r[BQ_REG_CTRL0] = 0xa2;
    poll_for(1);
    CHECK(f.wd_expiries == 1);
    CHECK(c.m.wd_expired);
    CHECK(f.r[BQ_REG_CTRL5] & BQ_EN_IBAT);
    CHECK(f.r[BQ_REG_ADC] & BQ_ADC_EN);
    poll_for(15);
    CHECK(!(f.r[BQ_REG_STAT0] & BQ_WD_STAT));   // kicked again
    CHECK(f.wd_expiries == 1);
    CHECK(only_allowed_writes());
}

static void quiet_and_usb(void)
{
    unsigned g;
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(2);

    CHECK(policy_quiet(&c.pol, ":1.7", 60, now, &g) == PERR_OK);
    core_apply(&c, now);                        // acts at once, no poll wait
    CHECK(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ);
    CHECK(!(f.r[BQ_REG_ADC] & BQ_ADC_EN));      // the fake turned the ADC off
    poll_for(1);
    CHECK(c.pol.state == ST_QUIET);
    CHECK(f.r[BQ_REG_ADC] & BQ_ADC_EN);         // set again
    CHECK(core_discharging(&c));

    // USB unplugged during the lease, then plugged in again: the chip
    // clears EN_HIZ, the next poll sets it again.
    fake_usb(&f, 0);
    poll_for(1);
    CHECK(c.pol.state == ST_ON_BATTERY && policy_quiet_active(&c.pol));
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
    fake_usb(&f, 1);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
    poll_for(1);
    CHECK(c.pol.state == ST_QUIET && (f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));

    // Expiry ends HIZ.
    poll_for(60);
    CHECK(c.pol.state == ST_CHARGING && !(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));

    // SetCharging(false): EN_CHG = 0, kept against the watchdog.
    policy_set_charging(&c.pol, 0);
    core_apply(&c, now);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_CHG));
    poll_for(120);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_CHG) && c.pol.state == ST_CHARGE_OFF);

    // Stop restores EN_CHG and clears HIZ even in quiet with charging off.
    CHECK(policy_quiet(&c.pol, ":1.8", 60, now, &g) == PERR_OK);
    core_apply(&c, now);
    uint8_t rb[5];
    CHECK(core_stop(&c, rb) == 0);
    CHECK(rb[0] == 0xa2);
    CHECK(only_allowed_writes());
}

static void chip_errors_and_faults(void)
{
    unsigned g;
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(2);
    CHECK(policy_quiet(&c.pol, ":1.9", 300, now, &g) == PERR_OK);
    core_apply(&c, now);

    f.fail = 1;
    poll_for(2);
    CHECK(c.pol.state == ST_QUIET);             // two failures are tolerated
    poll_for(1);
    CHECK(c.pol.state == ST_CHIP_ERROR && c.pol.nlease == 1);
    CHECK(policy_quiet(&c.pol, ":1.10", 300, now, &g) == PERR_CHIP);
    f.fail = 0;
    poll_for(3);                                // retry every 5 s only
    CHECK(c.pol.state == ST_CHIP_ERROR);
    poll_for(3);
    CHECK(c.pol.state == ST_QUIET && c.fails == 0);

    // A fault flag: quiet ends, HIZ released, "fault" for 10 s.
    f.r[BQ_REG_FFLAG0] = 0x40;                  // VBUS_OVP flag
    poll_for(1);
    CHECK(c.pol.state == ST_FAULT && c.pol.nlease == 0);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
    CHECK(f.r[BQ_REG_FFLAG0] == 0);             // cleared by the read
    CHECK(c.flags_acc[BQ_REG_FFLAG0 - BQ_REG_FLAG0] == 0x40);
    const char *names[4];
    CHECK(core_faults(&c, names, 4) == 1 && !strcmp(names[0], "vbus-ovp"));
    poll_for(10);
    CHECK(c.pol.state == ST_CHARGING);
    CHECK(core_faults(&c, names, 4) == 0);
}

static void stale(void)
{
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(2);
    CHECK(c.valid);
    f.adc_frozen = 1;
    poll_for(5);
    CHECK(c.valid);
    poll_for(1);
    CHECK(!c.valid && c.age > 5);
    f.adc_frozen = 0;
    poll_for(1);
    CHECK(c.valid);
}

// The Rev C end of charge: VBAT_OVP every second and every ADC result
// frozen. The chip sets the OVP flag again on every new edge, the test
// sets it before each poll.
static int ovp_on;

static void poll_ovp(int seconds)
{
    for (int k = 0; k < seconds; k++)
    {
        if (ovp_on)
            f.r[BQ_REG_FFLAG0] |= BQ_VBAT_OVP;
        poll_for(1);
    }
}

static void ovp_start(void)
{
    f.r[BQ_REG_FAULT0] = BQ_VBAT_OVP;
    f.r[BQ_REG_STAT1] = 0;                      // not charging
    f.adc_frozen = 1;
    ovp_on = 1;
}

static void ovp_end(void)
{
    f.r[BQ_REG_FAULT0] = 0;
    ovp_on = 0;
}

// Polls until the next recovery attempt, at most max seconds. Returns the
// seconds polled, or -1 without an attempt.
static int until_recovery(int max)
{
    unsigned n = c.recoveries;
    for (int k = 1; k <= max; k++)
    {
        poll_ovp(1);
        if (c.recoveries != n)
            return k;
    }
    return -1;
}

static void end_of_charge(void)
{
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.44, 0.23);
    f.r[BQ_REG_STAT1] = 4 << 5;                 // taper
    poll_for(10);
    CHECK(c.valid && c.pol.state == ST_CHARGING);
    double anchors = c.tel.anchored;

    ovp_start();
    poll_ovp(10);
    CHECK(!c.valid);
    CHECK(c.pol.state == ST_FULL);              // not "fault"
    CHECK(c.pol.block == BLK_NONE);             // quiet is granted
    const char *names[4];
    CHECK(core_faults(&c, names, 4) == 1 && !strcmp(names[0], "vbat-ovp"));
    CHECK(c.tel.anchored == anchors);           // no SoC anchor from the OVP

    // First attempt after 60 s of stale results.
    unsigned w2e = f.writes[BQ_REG_ADC];
    int k = until_recovery(120);
    CHECK(k > 40 && k <= 55);                   // 60 s after the last change
    CHECK(c.recoveries == 1);
    CHECK(f.r[BQ_REG_ADC] == BQ_STOCK_REG2E);   // stock values first
    CHECK(!(f.r[BQ_REG_CTRL5] & BQ_EN_IBAT));
    CHECK(!(f.r[BQ_REG_CTRL3] & BQ_PFM_FWD_DIS));
    CHECK(f.r[BQ_REG_CTRL0] == 0xa2);           // REG0F not touched
    poll_ovp(1);
    CHECK(f.r[BQ_REG_ADC] == BQ_ADC_EN);        // then the start-up writes
    CHECK(f.r[BQ_REG_CTRL5] & BQ_EN_IBAT);
    CHECK(f.r[BQ_REG_CTRL3] & BQ_PFM_FWD_DIS);
    CHECK(f.writes[BQ_REG_ADC] == w2e + 2);
    CHECK(only_allowed_writes());
    CHECK(f.wd_expiries == 0);

    // Still frozen: the waits double up to one hour.
    double t = now - 1;
    double gaps[8];
    for (int i = 0; i < 8; i++)
    {
        k = until_recovery(4000);
        gaps[i] = now - t;
        t = now;
    }
    CHECK_NEAR(gaps[0], 120, 1.5);
    CHECK_NEAR(gaps[1], 240, 1.5);
    CHECK_NEAR(gaps[2], 480, 1.5);
    CHECK_NEAR(gaps[3], 960, 1.5);
    CHECK_NEAR(gaps[4], 1920, 1.5);
    CHECK_NEAR(gaps[5], 3600, 1.5);
    CHECK_NEAR(gaps[6], 3600, 1.5);
    CHECK(c.recoveries == 9);
    CHECK(only_allowed_writes());

    // The init helped: fresh results, charging again.
    ovp_end();
    f.adc_frozen = 0;
    fake_adc(&f, 8.39, 0.46);
    f.r[BQ_REG_STAT1] = 3 << 5;
    poll_for(11);
    CHECK(c.valid && !c.recover_wait);
    CHECK(c.pol.state == ST_CHARGING);
    CHECK(c.recover_n == 9);                    // backoff kept for 10 min
    poll_for(600);
    CHECK(c.recover_n == 0);

    // The next end of charge gets a quick attempt again.
    fake_adc(&f, 8.44, 0.23);
    poll_for(5);
    ovp_start();
    k = until_recovery(120);
    CHECK(k > 50 && k <= 60);
    CHECK(c.recoveries == 10);
    ovp_end();
    f.adc_frozen = 0;
    poll_for(5);
}

// SetCharging(false) stays: the recovery never writes EN_CHG.
static void ovp_charging_off(void)
{
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.42, 0.1);
    poll_for(5);
    policy_set_charging(&c.pol, 0);
    core_apply(&c, now);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_CHG));
    ovp_start();
    unsigned w0f = f.writes[BQ_REG_CTRL0];
    int bad = 0;
    for (int i = 0; i < 450; i++)
    {
        poll_ovp(1);
        bad |= !!(f.r[BQ_REG_CTRL0] & (BQ_EN_CHG | BQ_EN_HIZ));
    }
    CHECK(!bad);
    CHECK(f.writes[BQ_REG_CTRL0] == w0f);
    CHECK(c.recoveries == 3);                   // about 60, 180 and 420 s
    ovp_end();
    f.adc_frozen = 0;
}

// A quiet lease during VBAT_OVP at the end of charge: granted, HIZ on, no
// recovery while it is held, the 60 s start again when it ends.
static void ovp_full_quiet(void)
{
    unsigned g;
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.44, 0.23);
    f.r[BQ_REG_STAT1] = 4 << 5;
    poll_for(10);
    ovp_start();
    poll_ovp(10);
    CHECK(c.pol.state == ST_FULL && c.pol.block == BLK_NONE);
    CHECK(policy_quiet(&c.pol, ":1.20", 120, now, &g) == PERR_OK && g == 120);
    core_apply(&c, now);
    CHECK(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ);
    CHECK(c.pol.state == ST_QUIET);
    unsigned n = c.recoveries;
    poll_ovp(119);
    CHECK(c.pol.state == ST_QUIET && (f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
    CHECK(c.recoveries == n);
    poll_ovp(1);                                // the lease ends
    CHECK(c.pol.state == ST_FULL && !(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
    CHECK(until_recovery(59) < 0);
    CHECK(until_recovery(5) > 0);
    CHECK(only_allowed_writes());

    // The OVP ends during the lease: the lease stays.
    CHECK(policy_quiet(&c.pol, ":1.20", 60, now, &g) == PERR_OK);
    core_apply(&c, now);
    poll_ovp(5);
    ovp_end();
    poll_for(15);
    CHECK(c.pol.state == ST_QUIET && c.pol.nlease == 1);
    CHECK(policy_quiet(&c.pol, ":1.20", 0, now, &g) == PERR_OK);
    f.adc_frozen = 0;
    poll_for(2);
    CHECK(!(f.r[BQ_REG_CTRL0] & BQ_EN_HIZ));
}

// HIZ turns the ADC off by design: no recovery during a quiet lease, and
// the 60 s start again when the lease ends.
static void stale_in_quiet(void)
{
    unsigned g;
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(5);
    CHECK(policy_quiet(&c.pol, ":1.30", 300, now, &g) == PERR_OK && g == 300);
    core_apply(&c, now);
    f.adc_frozen = 1;
    poll_for(299);
    CHECK(c.pol.state == ST_QUIET && !c.valid);
    CHECK(c.recoveries == 0);
    poll_for(1);                                // the lease ends
    CHECK(c.pol.state == ST_CHARGING);
    CHECK(until_recovery(59) < 0);
    CHECK(until_recovery(5) > 0);
    CHECK(c.recoveries == 1);
    f.adc_frozen = 0;

    // Full-hold is HIZ too.
    setup();
    c.cfg.full_hold = 1;
    CHECK(core_init(&c, now) == 0);
    poll_for(2);
    f.r[BQ_REG_STAT1] = BQ_CHG_DONE << 5;
    poll_for(2);
    CHECK(c.pol.state == ST_FULL_HOLD);
    f.adc_frozen = 1;
    poll_for(600);
    CHECK(c.recoveries == 0);
    f.adc_frozen = 0;

    // On battery there is no recovery either.
    setup();
    CHECK(core_init(&c, now) == 0);
    poll_for(2);
    fake_usb(&f, 0);
    f.adc_frozen = 1;
    poll_for(600);
    CHECK(c.recoveries == 0);
    f.adc_frozen = 0;
}

// VBAT_OVP that is not the end of charge stays "fault".
static void ovp_is_fault(void)
{
    // The last fresh VBAT was above 101 % of VREG.
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.60, 0.0);
    poll_for(5);
    ovp_start();
    poll_ovp(3);
    CHECK(c.pol.state == ST_FAULT && c.pol.block == BLK_FAULT);
    unsigned g;
    CHECK(policy_quiet(&c.pol, ":1.21", 60, now, &g) == PERR_FAULT);
    ovp_end();

    // Far below VREG.
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 7.9, 0.5);
    poll_for(5);
    ovp_start();
    poll_ovp(3);
    CHECK(c.pol.state == ST_FAULT);
    ovp_end();

    // A second fault next to VBAT_OVP.
    setup();
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.42, 0.1);
    poll_for(5);
    ovp_start();
    f.r[BQ_REG_FAULT1] = 0x04;                  // thermal shutdown
    poll_ovp(3);
    CHECK(c.pol.state == ST_FAULT);
    CHECK(policy_quiet(&c.pol, ":1.21", 60, now, &g) == PERR_FAULT);
    f.r[BQ_REG_FAULT1] = 0;
    ovp_end();

    // Frozen from the start: no fresh VBAT to judge by.
    setup();
    fake_adc(&f, 8.42, 0.1);
    f.r[BQ_REG_FAULT0] = BQ_VBAT_OVP;
    f.adc_frozen = 1;
    CHECK(core_init(&c, now) == 0);
    poll_for(3);
    CHECK(c.pol.state == ST_FAULT);
    // A restart into a stopped ADC is recovered too.
    CHECK(until_recovery(60) == 57);
    ovp_end();
    f.adc_frozen = 0;
}

// charge_voltage_v: written at start, set again when it differs, read back
// as VREG, and the full rule, the OVP at full and the quiet voltage limits
// follow it.
static void charge_voltage(void)
{
    setup();
    c.cfg.charge_voltage_v = 8.30;              // the default
    CHECK(core_init(&c, now) == 0);
    CHECK(f.r[BQ_REG_VREG] == 0x03 && f.r[BQ_REG_VREG + 1] == 0x3e);   // 830 x 10 mV
    CHECK(f.writes[BQ_REG_VREG] == 1 && f.writes[BQ_REG_VREG + 1] == 1);  // one transfer
    poll_for(3);
    CHECK_NEAR(c.m.vreg, 8.30, 1e-9);
    CHECK(f.writes[BQ_REG_VREG + 1] == 1);      // no rewrite while it holds
    CHECK(f.ignored_writes == 0);
    CHECK(only_allowed_writes());
    CHECK_NEAR(c.pol.end_vbat, 7.32, 0.005);    // 20 % of the 8.30 V table
    CHECK_NEAR(c.pol.min_vbat, 7.42, 0.005);

    // A reset of VREG (REG_RST, a power on reset of the charger): set again.
    fake_set16(&f, BQ_REG_VREG, 840);
    poll_for(2);
    CHECK(f.r[BQ_REG_VREG + 1] == 0x3e);
    CHECK_NEAR(c.m.vreg, 8.30, 1e-9);

    // Full by the second rule near 8.30 V, which is below 8.40 - 0.05 V.
    fake_adc(&f, 8.27, 0.1);
    poll_for(70);
    CHECK(c.pol.state == ST_FULL && c.tel.anchored);
    CHECK_NEAR(c.tel.soc, 100, 1e-9);

    // VBAT_OVP at 8.33 V is at the end of charge for 8.30 V (8.25 to 8.383 V).
    setup();
    c.cfg.charge_voltage_v = 8.30;
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.33, 0.23);
    f.r[BQ_REG_STAT1] = 4 << 5;
    poll_for(10);
    ovp_start();
    poll_ovp(10);
    CHECK(c.pol.state == ST_FULL && c.pol.block == BLK_NONE);
    ovp_end();
    f.adc_frozen = 0;
    poll_for(15);

    // The same OVP at 8.40 V is above 101 % of 8.30 V: a fault.
    setup();
    c.cfg.charge_voltage_v = 8.30;
    CHECK(core_init(&c, now) == 0);
    fake_adc(&f, 8.40, 0.23);
    f.r[BQ_REG_STAT1] = 4 << 5;
    poll_for(10);
    ovp_start();
    poll_ovp(10);
    CHECK(c.pol.state == ST_FAULT && c.pol.block == BLK_FAULT);

    // VREG lost with the ADC stopped: set again at the next poll, and the
    // recovery of the ADC keeps it.
    ovp_end();
    f.r[BQ_REG_STAT1] = 3 << 5;
    fake_set16(&f, BQ_REG_VREG, 840);
    poll_for(1);
    CHECK(f.r[BQ_REG_VREG + 1] == 0x3e);
    CHECK(until_recovery(120) > 0);
    poll_for(1);
    CHECK(f.r[BQ_REG_VREG] == 0x03 && f.r[BQ_REG_VREG + 1] == 0x3e);
    CHECK(only_allowed_writes());
    CHECK(f.ignored_writes == 0);
    f.adc_frozen = 0;
}

// The learning state and a learning discharge in progress survive the
// state file. A file of the older daemon (no learn_weight) loads with the
// weight 0, which tel_restore() takes as the prior.
static void store_learning(void)
{
    struct tel_saved a, b;
    memset(&a, 0, sizeof(a));
    a.cap_wh = 11.25;
    a.learn_w = 2.75;
    a.learn_n = 3;
    a.anchored = 1;
    a.e_wh = -4.5;
    a.learn_active = 1;
    a.learn_rest = 1;
    a.learn_rest_e = 4.25;
    a.learn_rest_socv = 58.5;
    CHECK(store_save(dir, &a) == 0);
    CHECK(store_load(dir, &b) == 0);
    CHECK_NEAR(b.cap_wh, 11.25, 1e-6);
    CHECK_NEAR(b.learn_w, 2.75, 1e-6);
    CHECK(b.learn_n == 3 && b.learn_active == 1 && b.learn_rest == 1);
    CHECK_NEAR(b.learn_rest_e, 4.25, 1e-6);
    CHECK_NEAR(b.learn_rest_socv, 58.5, 1e-6);
    a.learn_rest = 0;
    CHECK(store_save(dir, &a) == 0);
    CHECK(store_load(dir, &b) == 0);
    CHECK(b.learn_rest == 0 && b.learn_active == 1);

    char file[300];
    snprintf(file, sizeof(file), "%s/%s", dir, STORE_STATE);
    FILE *f = fopen(file, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs("{\n  \"capacity_learned_wh\": 14.800000,\n  \"learn_n50\": 0,\n"
          "  \"discharged_total_wh\": 53.687611,\n  \"energy_since_full_wh\": -4.434179,\n"
          "  \"anchored\": 1,\n  \"learn_active\": 0,\n  \"capacity_partial_wh\": 10.901481,\n"
          "  \"clean_stop\": 0\n}\n", f);
    fclose(f);
    CHECK(store_load(dir, &b) == 0);
    CHECK_NEAR(b.cap_wh, 14.8, 1e-6);
    CHECK(b.learn_w == 0 && b.learn_n == 0 && b.learn_rest == 0);
}

static void cycle_log(void)
{
    struct tel_cycle cy;
    memset(&cy, 0, sizeof(cy));
    cy.kind = TEL_DISCHARGE;
    cy.soc_start = 100;
    cy.soc_end = 20;
    for (int i = 0; i < STORE_CYCLES_MAX + 5; i++)
    {
        cy.t_start = i;
        CHECK(store_append_cycle(dir, &cy) == 0);
    }
    char file[300], line[1024];
    snprintf(file, sizeof(file), "%s/%s", dir, STORE_CYCLES);
    FILE *fp = fopen(file, "r");
    int n = 0;
    while (fp && fgets(line, sizeof(line), fp))
        n++;
    CHECK(n == STORE_CYCLES_MAX);
    CHECK(strstr(line, "\"start\":1004,") != NULL);    // the newest is kept
    CHECK(strstr(line, "\"vbat_curve\":[") != NULL);
    if (fp)
        fclose(fp);
}

// The shipped default file parses and matches the built-in defaults.
static void config_file(void)
{
    struct chargerd_config a, b;
    config_defaults(&a);
    config_defaults(&b);
    b.pfm = 1;
    b.capacity_rated_wh = 1;
    CHECK(config_load(&b, "chargerd.conf") == 0);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK(config_load(&b, "/nonexistent/chargerd.conf") == 1);
    CHECK(isnan(a.quiet_min_vbat) && isnan(a.quiet_end_vbat));     // auto
    CHECK_NEAR(a.charge_voltage_v, 8.30, 1e-9);

    // The charge voltage must be 4.00 to 4.20 V per cell.
    char file[300];
    snprintf(file, sizeof(file), "%s/test.conf", dir);
    const char *const bad[] = { "charge_voltage_v = 8.45\n", "charge_voltage_v = 7.9\n",
                                "charge_voltage_v = auto\n", "cells = 3\ncharge_voltage_v = 8.3\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        FILE *fp = fopen(file, "w");
        fputs(bad[i], fp);
        fclose(fp);
        config_defaults(&b);
        CHECK(config_load(&b, file) == -1);
    }
    FILE *fp = fopen(file, "w");
    fputs("charge_voltage_v = 8.0\nquiet_end_vbat = 7.2\nquiet_min_vbat = auto\n", fp);
    fclose(fp);
    config_defaults(&b);
    CHECK(config_load(&b, file) == 0);
    CHECK_NEAR(b.charge_voltage_v, 8.0, 1e-9);
    CHECK_NEAR(b.quiet_end_vbat, 7.2, 1e-9);
    CHECK(isnan(b.quiet_min_vbat));
    struct policy p;
    policy_init(&p, &b);
    CHECK_NEAR(p.end_vbat, 7.2, 1e-9);
    CHECK_NEAR(p.min_vbat, 7.3, 1e-9);
}

void test_core(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/chargerd-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir))
    {
        perror("mkdtemp");
        check_failed++;
        return;
    }
    start_and_stop();
    watchdog();
    quiet_and_usb();
    chip_errors_and_faults();
    stale();
    end_of_charge();
    ovp_charging_off();
    ovp_full_quiet();
    stale_in_quiet();
    ovp_is_fault();
    charge_voltage();
    store_learning();
    cycle_log();
    config_file();

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
}
