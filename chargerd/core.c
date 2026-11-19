// linht-chargerd: one poll of the charger, without D-Bus

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core.h"
#include "store.h"

#define FAULT0_MASK 0x7f    // REG20 and REG26 without IBAT_REG (bit 7)
#define FAULT1_MASK 0xf4    // REG21 and REG27, defined bits

int core_log_quiet;

// stderr with a priority prefix, which journald understands.
void core_log(int prio, const char *fmt, ...)
{
    if (core_log_quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "<%d>", prio);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

double core_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int core_usb(const struct core *c)
{
    return c->have_meas && c->m.vbus_present && c->m.power_good;
}

// The battery supplies the unit: no USB, or HIZ on USB.
int core_discharging(const struct core *c)
{
    return c->have_meas && (!core_usb(c) || (c->regs[BQ_REG_CTRL0] & BQ_EN_HIZ));
}

static void save_state(struct core *c, double now, double wall, int clean)
{
    struct tel_saved s;
    tel_save(&c->tel, &s);
    s.wall = wall;
    s.clean_stop = clean;
    int rc = store_save(c->cfg.state_dir, &s);
    if (rc < 0)
        core_log(4, "state file not written: %s", strerror(-rc));
    c->last_save = now;
}

int core_init(struct core *c, double now)
{
    int rc = bq_read_all(&c->io, c->regs);
    if (rc < 0)
        return rc;
    bq_decode(c->regs, &c->m);
    if (c->m.part != 1)
    {
        core_log(3, "REG48 = 0x%02x: PN is %d, not 1 (BQ25792), no writes", c->regs[BQ_REG_PART],
                 c->m.part);
        return -ENODEV;
    }
    if (c->m.cells != c->cfg.cells)
        core_log(4, "CELL says %dS, the config says %dS", c->m.cells, c->cfg.cells);
    if (c->m.pwm_750k)
        core_log(4, "PWM_FREQ is 750 kHz, LinHT needs 1.5 MHz (L9 is 1 uH). Not changed.");

    policy_init(&c->pol, &c->cfg);
    tel_init(&c->tel, &c->cfg);

    // Leases do not survive a restart: HIZ off first.
    rc = bq_update(&c->io, BQ_REG_CTRL0, BQ_EN_HIZ, 0, 0);
    if (rc < 0)
        return rc;
    if ((rc = bq_read_all(&c->io, c->regs)) < 0)
        return rc;
    struct bq_want w = { .hiz = 0, .chg = 1, .pfm = c->cfg.pfm };
    if ((rc = bq_enforce(&c->io, c->regs, &w)) < 0)
        return rc;
    c->applied = w;
    c->applied_valid = 1;
    if ((rc = bq_kick(&c->io)) < 0)
        return rc;
    c->last_kick = now;

    // The ADC results are old until the first conversion lands.
    memcpy(c->adc_last, c->regs + BQ_REG_IBUS, sizeof(c->adc_last));
    c->adc_changed = -1e9;
    c->last_fix_log = c->last_wd_log = c->last_stale_log = -1e9;
    c->last_save = now;
    c->recover_ref = now;
    c->fresh_since = -1;
    c->vbat_fresh = NAN;

    rc = store_load(c->cfg.state_dir, &c->saved);
    if (rc == 0)
    {
        c->restore_pending = 1;
        if (!c->saved.clean_stop)
            core_log(4, "last run did not stop cleanly");
    }
    else if (rc != -ENOENT)
    {
        core_log(4, "state file not read (%s), starting fresh", strerror(-rc));
    }
    core_log(6, "charger BQ25792 rev %d, %dS, PFM %s, full-hold %s, rated %.1f Wh",
             c->regs[BQ_REG_PART] & 7, c->m.cells, c->cfg.pfm ? "on" : "off",
             c->cfg.full_hold ? "on" : "off", c->cfg.capacity_rated_wh);
    return 0;
}

// The fault is only VBAT_OVP (status or recent flag), on USB, and the last
// fresh VBAT was at the end of charge: from VREG minus CORE_OVP_FULL_V up to
// 101 % of VREG, the lowest OVP release level of the data sheet. VREG must
// be a sane value for the cells. The daemon never writes VREG.
static int ovp_at_full(const struct core *c, double now)
{
    uint8_t f0 = c->m.fault0 & FAULT0_MASK, f1 = c->m.fault1 & FAULT1_MASK;
    if (now < c->fault_until)
    {
        f0 |= c->fflag0_recent;
        f1 |= c->fflag1_recent;
    }
    double vreg = c->m.vreg, v = c->vbat_fresh;
    return f0 == BQ_VBAT_OVP && !f1 && c->m.vbus_present && !isnan(v) && vreg > 0 &&
           vreg <= c->cfg.cells * 4.2 + 0.005 && v >= vreg - CORE_OVP_FULL_V && v <= vreg * 1.01;
}

static void fill_input(struct core *c, struct policy_in *in, double now, int chip_ok)
{
    memset(in, 0, sizeof(*in));
    in->now = now;
    in->chip_ok = chip_ok;
    in->usb = core_usb(c);
    in->fault = (c->m.fault0 & FAULT0_MASK) || (c->m.fault1 & FAULT1_MASK) || now < c->fault_until;
    in->ovp_full = in->fault && ovp_at_full(c, now);
    in->chg_done = c->m.chg_stat == BQ_CHG_DONE;
    in->vbat = c->valid ? c->m.vbat : NAN;
    in->ibat = c->valid ? c->m.ibat : NAN;
    in->vreg = c->m.vreg;
    in->soc = c->tel.soc;
    in->soc_trusted = c->tel.anchored;
}

// Writes the wanted bits. A difference to what the daemon wrote before
// means a watchdog expiry, a plug in or another writer: logged once a minute.
static int enforce(struct core *c, double now)
{
    struct bq_want w = { .hiz = c->pol.want_hiz, .chg = c->pol.want_chg, .pfm = c->cfg.pfm };
    int same = c->applied_valid && !memcmp(&w, &c->applied, sizeof(w));
    int rc = bq_enforce(&c->io, c->regs, &w);
    if (rc < 0)
        return rc;
    c->applied = w;
    c->applied_valid = 1;
    // REG2E after HIZ is expected: the chip turns its ADC off there.
    int unexpected = rc & ~(c->pol.want_hiz ? BQ_FIX_REG2E : 0);
    if (same && unexpected && now - c->last_fix_log >= CORE_LOG_S)
    {
        core_log(5, "charger bits changed outside the daemon, set again (%s%s%s%s)",
                 rc & BQ_FIX_REG0F ? " REG0F" : "", rc & BQ_FIX_REG12 ? " REG12" : "",
                 rc & BQ_FIX_REG14 ? " REG14" : "", rc & BQ_FIX_REG2E ? " REG2E" : "");
        c->last_fix_log = now;
    }
    return 0;
}

void core_apply(struct core *c, double now)
{
    struct policy_in in = c->pol.last;
    in.now = now;
    policy_step(&c->pol, &in);
    c->pending_full |= c->pol.full_event;
    // Between the two halves of a recovery the next poll writes.
    if (c->pol.state != ST_CHIP_ERROR && !c->recover_phase && enforce(c, now) < 0)
        c->fails++;
}

static void chip_failed(struct core *c, double now, int rc)
{
    struct policy_in in;
    if (++c->fails == CORE_FAIL_MAX)
        core_log(3, "charger does not answer (%s), retry every %.0f s", strerror(-rc), CORE_RETRY_S);
    if (c->fails >= CORE_FAIL_MAX)
        c->next_try = now + CORE_RETRY_S;
    fill_input(c, &in, now, c->fails < CORE_FAIL_MAX);
    policy_step(&c->pol, &in);
}

static void low_battery(struct core *c)
{
    double soc = c->tel.soc;
    if (core_usb(c) || isnan(soc))
    {
        c->low_level = 0;
        return;
    }
    int level = soc < c->cfg.critical_soc ? 2 : soc < c->cfg.low_soc ? 1 : 0;
    if (level > c->low_level)
        c->lowbat_event = level;
    c->low_level = level;
}

// Stopped ADC. Seen on Rev C at the end of a charge: VBAT_OVP, then every
// ADC result stays the same while REG2E still says continuous conversion.
// ADC_EN off and on did not help, a daemon restart did. So the recovery is
// what a restart writes: first the stock values of bq_restore() except
// REG0F, then one poll later the start-up writes (enforce() and a kick).
// Only with VBUS present and outside HIZ (HIZ turns the ADC off, quiet
// leases and full-hold are left alone), after CORE_RECOVER_S of stale
// results, then with a doubling wait up to CORE_RECOVER_MAX_S. Results
// fresh for CORE_RECOVER_OK_S start the backoff over. REG0F is never
// written by the recovery, so EN_CHG stays as the policy has it and the
// chip's own OVP keeps the converter off. VREG, ICHG and the limits are
// never written.
static int recover_due(struct core *c, double now)
{
    if (c->valid)
    {
        if (c->fresh_since < 0)
            c->fresh_since = now;
        if (c->recover_wait)
        {
            core_log(5, "ADC results fresh again after the charger init");
            c->recover_wait = 0;
        }
        if (c->recover_n && now - c->fresh_since >= CORE_RECOVER_OK_S)
        {
            c->recover_n = 0;
            c->recover_next = 0;
        }
        return 0;
    }
    c->fresh_since = -1;
    if (!c->m.vbus_present || (c->regs[BQ_REG_CTRL0] & BQ_EN_HIZ) || c->pol.want_hiz)
    {
        c->recover_ref = now;       // the stale time starts again after HIZ
        return 0;
    }
    double since = c->adc_changed > c->recover_ref ? c->adc_changed : c->recover_ref;
    return now - since >= CORE_RECOVER_S && now >= c->recover_next;
}

static void recover_start(struct core *c, double now)
{
    double wait = CORE_RECOVER_S * pow(2, c->recover_n + 1);
    if (wait > CORE_RECOVER_MAX_S)
        wait = CORE_RECOVER_MAX_S;
    c->recover_n++;
    c->recoveries++;
    c->recover_next = now + wait;
    c->recover_wait = 1;
    uint8_t f0 = (c->m.fault0 | (now < c->fault_until ? c->fflag0_recent : 0)) & FAULT0_MASK;
    double stale = c->adc_changed > 0 ? c->age : now - c->recover_ref;
    core_log(4, "ADC results unchanged for %.0f s with USB present%s, charger init again "
             "(attempt %d, the next one in %.0f s at the earliest)", stale,
             f0 & BQ_VBAT_OVP ? " and VBAT_OVP" : "", c->recover_n, wait);
    if (bq_restore_keep0f(&c->io) < 0)
        c->fails++;
    c->recover_phase = 1;
    c->applied_valid = 0;           // the next enforce() is not an outside change
    c->last_kick = now;
}

void core_poll(struct core *c, double now, double wall)
{
    struct policy_in in;
    int rc;

    if (c->fails >= CORE_FAIL_MAX && now < c->next_try)
    {
        fill_input(c, &in, now, 0);
        policy_step(&c->pol, &in);
        return;
    }
    if ((rc = bq_read_all(&c->io, c->regs)) < 0)
    {
        chip_failed(c, now, rc);
        return;
    }
    if (c->fails >= CORE_FAIL_MAX)
        core_log(5, "charger answers again");
    c->fails = 0;

    bq_decode(c->regs, &c->m);
    c->have_meas = 1;
    for (int i = 0; i < 6; i++)
        c->flags_acc[i] |= c->regs[BQ_REG_FLAG0 + i];
    uint8_t f0 = c->m.fflag0 & FAULT0_MASK, f1 = c->m.fflag1 & FAULT1_MASK;
    if (f0 || f1)
    {
        if (now >= c->fault_until)
            c->fflag0_recent = c->fflag1_recent = 0;
        c->fflag0_recent |= f0;
        c->fflag1_recent |= f1;
        c->fault_until = now + CORE_FAULT_HOLD_S;
        core_log(4, "charger fault flags REG26 0x%02x REG27 0x%02x", c->m.fflag0, c->m.fflag1);
    }
    else if (now >= c->fault_until)
    {
        c->fflag0_recent = c->fflag1_recent = 0;
    }

    // Stale detection: the result registers stop changing.
    if (memcmp(c->adc_last, c->regs + BQ_REG_IBUS, sizeof(c->adc_last)))
    {
        memcpy(c->adc_last, c->regs + BQ_REG_IBUS, sizeof(c->adc_last));
        c->adc_changed = now;
    }
    c->age = now - c->adc_changed;
    c->valid = c->age <= CORE_STALE_S;
    if (c->valid)
        c->vbat_fresh = c->m.vbat;
    if (c->recover_phase)
    {
        // Second half of a recovery: the start-up writes in this poll.
        c->recover_phase = 0;
        c->recover_ref = now;
        c->last_kick = -1e9;
    }
    if (!c->valid && c->adc_changed > 0 && now - c->last_stale_log >= CORE_LOG_S)
    {
        core_log(4, "ADC results unchanged for %.0f s, marked stale", c->age);
        c->last_stale_log = now;
    }
    if (c->m.wd_expired && now - c->last_wd_log >= CORE_LOG_S)
    {
        core_log(4, "charger watchdog expired, settings applied again");
        c->last_wd_log = now;
    }

    if (c->restore_pending && c->valid)
    {
        c->restore_pending = 0;
        int reused = tel_restore(&c->tel, &c->saved, c->m.vbat);
        core_log(6, "state file: learned %.2f Wh, counter %s", c->tel.cap_wh,
                 reused ? "reused" : "dropped (voltage disagrees or no anchor)");
        save_state(c, now, wall, 0);
    }

    fill_input(c, &in, now, 1);
    policy_step(&c->pol, &in);
    c->pending_full |= c->pol.full_event;
    if (in.ovp_full && !c->ovp_full)
        core_log(5, "VBAT_OVP at the end of charge (last fresh VBAT %.3f V, VREG %.2f V), "
                 "state full, the fault stays in Faults", c->vbat_fresh, c->m.vreg);
    c->ovp_full = in.ovp_full;
    if (recover_due(c, now))
        recover_start(c, now);
    else if (enforce(c, now) < 0)
        c->fails++;

    if (now - c->last_kick >= CORE_KICK_S)
    {
        if (bq_kick(&c->io) < 0)
            c->fails++;
        else
            c->last_kick = now;
    }

    struct tel_in t = {
        .now = now, .wall = wall,
        .vbat = c->m.vbat, .ibat = c->m.ibat, .tdie = c->m.tdie,
        .valid = c->valid,
        .discharging = core_discharging(c),
        .charging = c->pol.state == ST_CHARGING,
        .full_event = c->pending_full,
    };
    c->pending_full = 0;
    if (!c->restore_pending)
    {
        tel_sample(&c->tel, &t);
        if (t.full_event)
            core_log(6, "battery full, SoC anchor at 100 %%");
        if (c->tel.learned)
            core_log(6, "capacity learned: %.2f Wh (%.0f %% of rated)", c->tel.cap_wh, tel_soh(&c->tel));
        if (c->tel.learned_partial)
            core_log(6, "partial cycle: %.1f %% deep, %.2f Wh, weight %.2f, partial capacity %.2f Wh "
                     "from %d cycles (low precision)", c->tel.part_depth, c->tel.part_est,
                     c->tel.part_weight, c->tel.part_wh, c->tel.part_n);
        if (c->tel.cycle_ready)
        {
            rc = store_append_cycle(c->cfg.state_dir, &c->tel.cycle);
            if (rc < 0)
                core_log(4, "cycle log not written: %s", strerror(-rc));
        }
        if (c->tel.changed || now - c->last_save >= CORE_SAVE_S)
            save_state(c, now, wall, 0);
        low_battery(c);
    }
}

int core_stop(struct core *c, uint8_t rb[5])
{
    int rc = bq_restore(&c->io, rb);
    if (!c->restore_pending)
        save_state(c, 0, (double)time(NULL), 1);
    return rc;
}

int core_faults(const struct core *c, const char **out, int max)
{
    const char *a[CORE_MAX_FAULTS];
    int n = bq_fault_names(c->m.fault0 & FAULT0_MASK, c->m.fault1 & FAULT1_MASK, out, max);
    // Plus the flags seen in the last seconds, without duplicates.
    int k = bq_fault_names(c->fflag0_recent, c->fflag1_recent, a, CORE_MAX_FAULTS);
    for (int i = 0; i < k && n < max; i++)
    {
        int dup = 0;
        for (int j = 0; j < n; j++)
            dup |= out[j] == a[i];
        if (!dup)
            out[n++] = a[i];
    }
    return n;
}
