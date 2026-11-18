// linht-chargerd: charger state machine and quiet leases. No I/O.

#include <math.h>
#include <string.h>

#include "policy.h"

#define FULL_VREG_MARGIN 0.05   // V below VREG, second full rule
#define FULL_IBAT_MAX    0.2    // A
#define FULL_HOLD_S      60.0   // s the second rule must hold
#define UNFULL_IBAT      0.3    // A, charging again
#define UNFULL_VREG      0.15   // V below VREG
#define UNFULL_HOLD_S    10.0   // s

void policy_init(struct policy *p, const struct chargerd_config *cfg)
{
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;
    p->charging_enabled = 1;
    p->full_cond_since = -1;
    p->unfull_since = -1;
    p->state = ST_CHARGING;
    p->want_chg = 1;
    p->last.soc = NAN;
    p->last.vbat = NAN;
    p->last.chip_ok = 1;
}

static int find(const struct policy *p, const char *owner)
{
    for (int i = 0; i < p->nlease; i++)
        if (!strcmp(p->lease[i].owner, owner))
            return i;
    return -1;
}

static void drop(struct policy *p, int i)
{
    p->lease[i] = p->lease[--p->nlease];
}

static void end_all(struct policy *p)
{
    if (p->nlease)
        p->leases_ended = 1;
    p->nlease = 0;
}

int policy_has_lease(const struct policy *p, const char *owner)
{
    return find(p, owner) >= 0;
}

int policy_release(struct policy *p, const char *owner)
{
    int i = find(p, owner);
    if (i < 0)
        return 0;
    drop(p, i);
    return 1;
}

static int below_min(const struct policy *p, const struct policy_in *in)
{
    if (in->soc_trusted && !isnan(in->soc))
        return in->soc < p->cfg->quiet_min_soc;
    return in->vbat < p->cfg->quiet_min_vbat;
}

static int below_end(const struct policy *p, const struct policy_in *in)
{
    if (in->soc_trusted && !isnan(in->soc))
        return in->soc < p->cfg->quiet_end_soc;
    return in->vbat < p->cfg->quiet_end_vbat;
}

static enum pblock blocked(const struct policy *p, const struct policy_in *in)
{
    if (!in->chip_ok)
        return BLK_CHIP;
    if (in->fault)
        return BLK_FAULT;
    // The battery limits apply only on USB. On battery quiet is free.
    if (in->usb && (p->low_block || below_min(p, in)))
        return BLK_LOW_BATTERY;
    return BLK_NONE;
}

int policy_quiet(struct policy *p, const char *owner, unsigned seconds, double now,
                 unsigned *granted)
{
    *granted = 0;
    if (seconds == 0)
    {
        policy_release(p, owner);
        return PERR_OK;
    }

    switch (blocked(p, &p->last))
    {
    case BLK_CHIP:
        return PERR_CHIP;
    case BLK_FAULT:
        return PERR_FAULT;
    case BLK_LOW_BATTERY:
        return PERR_LOW_BATTERY;
    case BLK_NONE:
        break;
    }

    if (seconds > p->cfg->quiet_max_s)
        seconds = p->cfg->quiet_max_s;

    int i = find(p, owner);
    if (i < 0)
    {
        if (p->nlease == POLICY_MAX_LEASES)
            return PERR_TOO_MANY;
        i = p->nlease++;
        strncpy(p->lease[i].owner, owner, POLICY_OWNER_LEN - 1);
        p->lease[i].owner[POLICY_OWNER_LEN - 1] = 0;
    }
    p->lease[i].expiry = now + seconds;
    *granted = seconds;
    return PERR_OK;
}

void policy_set_charging(struct policy *p, int enable)
{
    p->charging_enabled = !!enable;
}

// The "full" latch. Updated only while the converter charges, because in
// HIZ or with charging off the battery current says nothing about full.
static void update_full(struct policy *p, const struct policy_in *in)
{
    double now = in->now;
    int near_vreg = in->vbat >= in->vreg - FULL_VREG_MARGIN;

    if (!p->full)
    {
        p->unfull_since = -1;
        if (in->chg_done)
        {
            p->full = 1;
        }
        else if (near_vreg && in->ibat < FULL_IBAT_MAX)
        {
            // A weak USB source keeps the charger in VINDPM, where it
            // never terminates. 60 s near VREG at low current is full too.
            if (p->full_cond_since < 0)
                p->full_cond_since = now;
            if (now - p->full_cond_since >= FULL_HOLD_S)
                p->full = 1;
        }
        else
        {
            p->full_cond_since = -1;
        }
        if (p->full)
        {
            p->full_event = 1;
            p->full_cond_since = -1;
        }
        return;
    }

    // Full is left when the charger charges again (recharge threshold).
    if (!in->chg_done && (in->ibat >= UNFULL_IBAT || in->vbat < in->vreg - UNFULL_VREG))
    {
        if (p->unfull_since < 0)
            p->unfull_since = now;
        if (now - p->unfull_since >= UNFULL_HOLD_S)
        {
            p->full = 0;
            p->unfull_since = -1;
        }
    }
    else
    {
        p->unfull_since = -1;
    }
}

void policy_step(struct policy *p, const struct policy_in *in)
{
    const struct chargerd_config *cfg = p->cfg;

    p->full_event = 0;
    p->leases_ended = 0;
    p->last = *in;

    for (int i = 0; i < p->nlease;)
        if (p->lease[i].expiry <= in->now)
            drop(p, i);
        else
            i++;

    if (!in->chip_ok)
    {
        // Nothing can be written. Leases are kept, the wanted bits too.
        p->state = ST_CHIP_ERROR;
        p->block = BLK_CHIP;
        return;
    }

    if (in->fault)
    {
        end_all(p);
        p->holding = 0;
        p->state = ST_FAULT;
        p->block = BLK_FAULT;
        p->want_hiz = 0;
        p->want_chg = p->charging_enabled;
        return;
    }

    if (!in->usb)
    {
        p->full = 0;
        p->holding = 0;
        p->full_cond_since = -1;
        p->unfull_since = -1;
        p->state = ST_ON_BATTERY;
        p->block = blocked(p, in);
        p->want_hiz = 0;
        p->want_chg = p->charging_enabled;
        return;
    }

    // On USB: the low battery limits.
    if (below_end(p, in))
    {
        if (p->nlease)
            p->low_block = 1;
        end_all(p);
        p->holding = 0;
    }
    if (p->low_block && !isnan(in->soc) && in->soc >= cfg->quiet_resume_soc)
        p->low_block = 0;
    p->block = blocked(p, in);

    // The full latch is updated only while the converter charges.
    if (p->state == ST_CHARGING || p->state == ST_FULL)
        update_full(p, in);

    if (p->full && cfg->full_hold && p->charging_enabled && !p->holding)
        p->holding = 1;
    if (p->holding && (!cfg->full_hold || !p->charging_enabled ||
                       (!isnan(in->soc) && in->soc < cfg->full_hold_resume_soc)))
    {
        p->holding = 0;
        p->full = 0;
    }

    p->want_chg = p->charging_enabled;
    if (p->nlease > 0)
    {
        p->state = ST_QUIET;
        p->want_hiz = 1;
    }
    else if (p->holding)
    {
        p->state = ST_FULL_HOLD;
        p->want_hiz = 1;
    }
    else if (!p->charging_enabled)
    {
        p->state = ST_CHARGE_OFF;
        p->want_hiz = 0;
    }
    else if (p->full)
    {
        p->state = ST_FULL;
        p->want_hiz = 0;
    }
    else
    {
        p->state = ST_CHARGING;
        p->want_hiz = 0;
    }
}

int policy_quiet_active(const struct policy *p)
{
    return p->nlease > 0 && (p->state == ST_QUIET || p->state == ST_ON_BATTERY);
}

const char *policy_state_name(enum pstate s)
{
    static const char *const n[] = { "on-battery", "charging", "full", "full-hold",
                                     "quiet", "charge-off", "fault", "chip-error" };
    return n[s];
}

const char *policy_block_name(enum pblock b)
{
    static const char *const n[] = { "", "low-battery", "fault", "chip" };
    return n[b];
}
