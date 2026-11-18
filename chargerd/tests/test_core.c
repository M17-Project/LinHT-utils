// linht-chargerd tests: the poll against a fake chip (register logic,
// watchdog, quiet, USB plug events, chip errors, stop)

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
            r != BQ_REG_CTRL5 && r != BQ_REG_ADC)
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
    cycle_log();
    config_file();

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
}
