// linht-chargerd: averages, energy counter, state of charge and capacity
// learning. No I/O.

#include <math.h>
#include <string.h>

#include "telemetry.h"

#define VBAT_TAU_S       30.0   // s, voltage average for the voltage SoC
#define DT_MAX_S         5.0    // s, longer gaps are not integrated
#define LEARN_P_MAX      3.0    // W, light load for learning
#define LEARN_SOCV_MAX   50.0   // %, voltage SoC the discharge must reach
#define CYCLE_MIN_SOC    10.0   // %, shorter cycles are not logged
#define CHARGE_IBAT_MIN  0.05   // A, a real charge ends a learning discharge

// Partial cycles. In HIZ the load is small and steady, so VBAT is close to
// the open circuit voltage and the voltage table gives the SoC at both ends
// of a discharge segment. The load drop is about the same at both ends and
// mostly cancels. The estimate is low precision: the table is generic.
#define PART_SETTLE_S    600.0  // s, VBAT relaxes after a charge, not counted
#define PART_DEPTH_MIN   15.0   // %, shallower segments are not used
#define PART_DEPTH_REF   20.0   // %, depth of weight 1
#define PART_W_DEPTH_MAX 9.0    // depth weight limit (60 % deep)
#define PART_I_REF       0.25   // A, load current that halves the weight
#define PART_I_MAX       0.6    // A, heavier mean loads are not used
#define PART_EST_LO      0.4    // a segment estimate outside 0.4 to 1.6
#define PART_EST_HI      1.6    //   times rated is dropped as implausible
#define PART_PRIOR_W     1.0    // weight of the starting value
#define PART_DECAY       0.9    // older segments lose 10 % per new one

enum { PART_IDLE, PART_SETTLE, PART_COUNT };

// Per cell open circuit points of a generic NMC cell, the table of
// field-log.py. Under load and while charging it is off by 10 to 20 %.
static const double ocv[][2] = {
    { 3.30, 0 }, { 3.50, 5 }, { 3.60, 10 }, { 3.70, 25 }, { 3.75, 40 }, { 3.80, 50 },
    { 3.87, 60 }, { 3.95, 70 }, { 4.03, 80 }, { 4.10, 90 }, { 4.20, 100 },
};

void tel_init(struct telemetry *t, const struct chargerd_config *cfg)
{
    memset(t, 0, sizeof(*t));
    t->cfg = cfg;
    t->cap_wh = cfg->capacity_rated_wh;
    t->vbat_avg = NAN;
    t->soc = NAN;
    t->soc_voltage = NAN;
    t->discharge_since = -1;
    t->charge_since = -1;
    t->learn_socv = NAN;
    t->part_wh = NAN;
    t->part_socv0 = t->part_socv = NAN;
    t->part_est = t->part_weight = t->part_depth = NAN;
}

double tel_soc_from_voltage(const struct telemetry *t, double vbat)
{
    int n = (int)(sizeof(ocv) / sizeof(ocv[0]));
    if (isnan(vbat) || vbat < 1.0)
        return NAN;
    double v = vbat / (t->cfg->cells > 0 ? t->cfg->cells : 2);
    if (v <= ocv[0][0])
        return 0;
    for (int i = 1; i < n; i++)
        if (v <= ocv[i][0])
            return ocv[i - 1][1] + (ocv[i][1] - ocv[i - 1][1]) * (v - ocv[i - 1][0]) /
                   (ocv[i][0] - ocv[i - 1][0]);
    return 100;
}

double tel_voltage_from_soc(const struct telemetry *t, double soc)
{
    int n = (int)(sizeof(ocv) / sizeof(ocv[0]));
    int cells = t->cfg->cells > 0 ? t->cfg->cells : 2;
    if (isnan(soc))
        return NAN;
    if (soc <= ocv[0][1])
        return cells * ocv[0][0];
    for (int i = 1; i < n; i++)
        if (soc <= ocv[i][1])
            return cells * (ocv[i - 1][0] + (ocv[i][0] - ocv[i - 1][0]) * (soc - ocv[i - 1][1]) /
                                                (ocv[i][1] - ocv[i - 1][1]));
    return cells * ocv[n - 1][0];
}

static double ring_avg(const struct telemetry *t, const float *buf, unsigned window)
{
    unsigned n = t->count < window ? t->count : window;
    if (n * 2 < window || n == 0)
        return NAN;
    double sum = 0;
    for (unsigned k = 0; k < n; k++)
        sum += buf[(t->head + TEL_RING - 1 - k) % TEL_RING];
    return sum / n;
}

double tel_avg_power(const struct telemetry *t, unsigned window)
{
    return ring_avg(t, t->pbat, window);
}

double tel_avg_current(const struct telemetry *t, unsigned window)
{
    return ring_avg(t, t->ibat, window);
}

static double clamp(double x, double lo, double hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static void update_soc(struct telemetry *t)
{
    t->soc_voltage = tel_soc_from_voltage(t, t->vbat_avg);
    if (t->anchored)
    {
        t->soc = clamp(100.0 * (1.0 + t->e_wh / tel_capacity_wh(t)), 0, 100);
        t->soc_from_counter = 1;
    }
    else
    {
        t->soc = t->soc_voltage;
        t->soc_from_counter = 0;
    }
}

// End of a learning discharge: C_est = E_out / (1 - SoC_v / 100).
static void learn_finish(struct telemetry *t)
{
    const struct chargerd_config *cfg = t->cfg;
    double socv = t->learn_socv;

    t->learn_active = 0;
    if (isnan(socv) || socv > LEARN_SOCV_MAX || t->learn_t <= 0 || t->learn_e <= 0)
        return;
    if (t->learn_e / (t->learn_t / 3600.0) >= LEARN_P_MAX)
        return;

    double depth = 1.0 - socv / 100.0;
    double est = t->learn_e / depth;
    t->cap_wh += 0.5 * depth * (est - t->cap_wh);
    t->cap_wh = clamp(t->cap_wh, 0.5 * cfg->capacity_rated_wh, 1.1 * cfg->capacity_rated_wh);
    if (depth >= 0.7)
        t->n70++;
    if (depth >= 0.5)
        t->n50++;
    t->learned = 1;
    t->changed = 1;
}

// End of a partial discharge segment: C_est = E_out / (SoC_v0 - SoC_v1).
static void partial_finish(struct telemetry *t)
{
    const struct chargerd_config *cfg = t->cfg;
    int counting = t->part_state == PART_COUNT;

    t->part_state = PART_IDLE;
    if (!counting || t->part_t <= 0 || t->part_e <= 0)
        return;
    double depth = t->part_socv0 - t->part_socv;
    if (isnan(depth) || depth < PART_DEPTH_MIN)
        return;
    double i_mean = t->part_q / (t->part_t / 3600.0);
    if (i_mean > PART_I_MAX)
        return;
    double est = t->part_e / (depth / 100.0);
    if (est < PART_EST_LO * cfg->capacity_rated_wh || est > PART_EST_HI * cfg->capacity_rated_wh)
        return;

    // The table error at the two ends is a few % of SoC, so the error of
    // C_est falls with the depth: weight by depth squared. The load moves
    // VBAT away from the open circuit voltage: less weight under load.
    double wd = (depth / PART_DEPTH_REF) * (depth / PART_DEPTH_REF);
    if (wd > PART_W_DEPTH_MAX)
        wd = PART_W_DEPTH_MAX;
    double w = wd / (1.0 + (i_mean / PART_I_REF) * (i_mean / PART_I_REF));

    if (t->part_n == 0)
    {
        t->part_wh = t->cap_wh;
        t->part_w = PART_PRIOR_W;
    }
    t->part_w = t->part_w * PART_DECAY + w;
    t->part_wh += w / t->part_w * (est - t->part_wh);
    t->part_wh = clamp(t->part_wh, 0.5 * cfg->capacity_rated_wh, 1.1 * cfg->capacity_rated_wh);
    t->part_n++;
    t->part_est = est;
    t->part_weight = w;
    t->part_depth = depth;
    t->learned_partial = 1;
    t->changed = 1;
}

static void partial_sample(struct telemetry *t, const struct tel_in *in, double p, double i,
                           double dt)
{
    if (in->discharging)
    {
        if (t->part_state == PART_IDLE)
        {
            t->part_state = PART_SETTLE;
            t->part_t = 0;
        }
        if (t->part_state == PART_SETTLE)
        {
            t->part_t += dt;
            if (t->part_t >= PART_SETTLE_S && in->valid && !isnan(t->soc_voltage))
            {
                t->part_state = PART_COUNT;
                t->part_t = t->part_e = t->part_q = 0;
                t->part_socv0 = t->part_socv = t->soc_voltage;
            }
            return;
        }
        if (p < 0)
            t->part_e += -p * dt / 3600.0;
        if (i < 0)
            t->part_q += -i * dt / 3600.0;
        t->part_t += dt;
        if (in->valid)
            t->part_socv = t->soc_voltage;
    }
    else if ((in->charging && i > CHARGE_IBAT_MIN) || in->full_event)
    {
        partial_finish(t);
    }
    // Otherwise (USB without charge, a short gap between leases) the
    // segment waits.
}

static void seg_finish(struct telemetry *t, const struct tel_in *in)
{
    struct tel_cycle *c = &t->seg;

    c->t_end = in->wall;
    c->duration = in->now - t->seg_t0;
    c->vbat_end = t->vbat_avg;
    c->soc_end = t->soc;
    c->energy_wh = t->seg_e;
    c->power_mean_w = c->duration > 0 ? t->seg_e / (c->duration / 3600.0) : NAN;
    if (isnan(c->soc_start) || isnan(c->soc_end) || fabs(c->soc_end - c->soc_start) < CYCLE_MIN_SOC)
        return;

    // VBAT at every 5 % of the energy, interpolated from the samples.
    for (int k = 0; k < TEL_CURVE_PTS; k++)
    {
        double target = t->seg_e * k / (TEL_CURVE_PTS - 1);
        float v = t->seg_n ? t->seg_pv[t->seg_n - 1] : NAN;
        for (unsigned i = 1; i < t->seg_n; i++)
            if (t->seg_pe[i] >= target)
            {
                double e0 = t->seg_pe[i - 1], e1 = t->seg_pe[i];
                double f = e1 > e0 ? (target - e0) / (e1 - e0) : 0;
                v = (float)(t->seg_pv[i - 1] + f * (t->seg_pv[i] - t->seg_pv[i - 1]));
                break;
            }
        if (k == 0 && t->seg_n)
            v = t->seg_pv[0];
        c->curve[k] = v;
    }
    t->cycle = *c;
    t->cycle_ready = 1;
    t->changed = 1;
}

static void seg_start(struct telemetry *t, const struct tel_in *in, int kind)
{
    memset(&t->seg, 0, sizeof(t->seg));
    t->seg_kind = kind;
    t->seg.kind = kind;
    t->seg.t_start = in->wall;
    t->seg.vbat_start = t->vbat_avg;
    t->seg.soc_start = t->soc;
    t->seg.tdie_max = in->tdie;
    t->seg_t0 = in->now;
    t->seg_e = 0;
    t->seg_n = 0;
    t->seg_step = 10;
    t->seg_tick = 0;
}

static void seg_sample(struct telemetry *t, const struct tel_in *in, double p, double i, double dt)
{
    t->seg_e += fabs(p) * dt / 3600.0;
    t->seg.charge_ah += fabs(i) * dt / 3600.0;
    if (in->tdie > t->seg.tdie_max)
        t->seg.tdie_max = in->tdie;
    if (t->seg_tick++ % t->seg_step)
        return;
    if (t->seg_n == TEL_SEG_MAX)
    {
        // Keep every second sample and halve the rate.
        for (unsigned k = 0; k < TEL_SEG_MAX / 2; k++)
        {
            t->seg_pe[k] = t->seg_pe[2 * k];
            t->seg_pv[k] = t->seg_pv[2 * k];
        }
        t->seg_n = TEL_SEG_MAX / 2;
        t->seg_step *= 2;
    }
    t->seg_pe[t->seg_n] = (float)t->seg_e;
    t->seg_pv[t->seg_n] = (float)in->vbat;
    t->seg_n++;
}

void tel_sample(struct telemetry *t, const struct tel_in *in)
{
    double dt = t->have_last ? in->now - t->last_now : 0;
    if (dt < 0 || dt > DT_MAX_S)
        dt = 0;

    t->cycle_ready = 0;
    t->learned = 0;
    t->learned_partial = 0;
    t->changed = 0;

    // A stale measurement: the counter holds the last power.
    double p = in->valid ? in->vbat * in->ibat : t->last_p;
    double i = in->valid ? in->ibat : t->last_i;
    if (in->valid)
    {
        if (isnan(t->vbat_avg))
            t->vbat_avg = in->vbat;
        else if (dt > 0)
            t->vbat_avg += (in->vbat - t->vbat_avg) * (dt / (VBAT_TAU_S + dt));
    }
    if (in->valid || t->have_last)
    {
        t->pbat[t->head] = (float)p;
        t->ibat[t->head] = (float)i;
        t->head = (t->head + 1) % TEL_RING;
        if (t->count < TEL_RING)
            t->count++;
    }

    t->e_wh += p * dt / 3600.0;
    if (t->e_wh > 0)
        t->e_wh = 0;
    if (p < 0)
        t->out_total_wh += -p * dt / 3600.0;

    if (in->full_event)
    {
        if (t->learn_active)
            learn_finish(t);
        t->e_wh = 0;
        t->anchored = 1;
        t->learn_active = 1;
        t->learn_e = 0;
        t->learn_t = 0;
        t->learn_socv = NAN;
        t->changed = 1;
    }
    update_soc(t);

    if (t->learn_active)
    {
        if (in->discharging)
        {
            if (p < 0)
                t->learn_e += -p * dt / 3600.0;
            t->learn_t += dt;
            t->learn_socv = t->soc_voltage;
        }
        else if (in->charging && i > CHARGE_IBAT_MIN)
        {
            learn_finish(t);
        }
    }

    partial_sample(t, in, p, i, dt);

    // Times since the discharge or charge started. Measured, not estimated.
    if (in->discharging)
    {
        if (t->discharge_since < 0)
            t->discharge_since = in->now;
    }
    else
    {
        t->discharge_since = -1;
    }
    if (in->charging)
    {
        if (t->charge_since < 0)
            t->charge_since = in->now;
    }
    else
    {
        t->charge_since = -1;
    }

    int kind = in->discharging ? TEL_DISCHARGE : in->charging ? TEL_CHARGE : TEL_NONE;
    if (kind != t->seg_kind)
    {
        if (t->seg_kind != TEL_NONE)
            seg_finish(t, in);
        t->seg_kind = TEL_NONE;
        if (kind != TEL_NONE && !isnan(t->soc))
            seg_start(t, in, kind);
    }
    if (t->seg_kind != TEL_NONE)
        seg_sample(t, in, p, i, dt);

    t->last_now = in->now;
    t->last_p = p;
    t->last_i = i;
    t->have_last = 1;
}

double tel_energy_remaining(const struct telemetry *t)
{
    return isnan(t->soc) ? NAN : t->soc / 100.0 * tel_capacity_wh(t);
}

double tel_capacity_wh(const struct telemetry *t)
{
    if (!tel_capacity_reliable(t) && t->part_n > 0)
        return t->part_wh;
    return t->cap_wh;
}

const char *tel_capacity_source(const struct telemetry *t)
{
    if (tel_capacity_reliable(t))
        return "full";
    return t->part_n > 0 ? "partial" : "none";
}

double tel_capacity_partial_wh(const struct telemetry *t)
{
    return t->part_n > 0 ? t->part_wh : NAN;
}

double tel_soh(const struct telemetry *t)
{
    return 100.0 * tel_capacity_wh(t) / t->cfg->capacity_rated_wh;
}

double tel_equivalent_cycles(const struct telemetry *t)
{
    return t->out_total_wh / t->cfg->capacity_rated_wh;
}

int tel_capacity_reliable(const struct telemetry *t)
{
    return t->n70 >= 1 || t->n50 >= 2;
}

static double avg_10m_or_1m(const struct telemetry *t)
{
    double p = tel_avg_power(t, 600);
    return isnan(p) ? tel_avg_power(t, 60) : p;
}

long long tel_runtime_s(const struct telemetry *t, int discharging)
{
    double p = avg_10m_or_1m(t);
    if (!discharging || isnan(p) || isnan(t->soc) || p >= -0.01)
        return -1;
    double usable = (t->soc - t->cfg->critical_soc) / 100.0 * tel_capacity_wh(t);
    if (usable < 0)
        usable = 0;
    return (long long)(usable / -p * 3600.0);
}

long long tel_time_to_full_s(const struct telemetry *t, int charging)
{
    double p = avg_10m_or_1m(t);
    if (!charging || isnan(p) || isnan(t->soc) || p <= 0.01)
        return -1;
    return (long long)((100.0 - t->soc) / 100.0 * tel_capacity_wh(t) / p * 3600.0);
}

unsigned long long tel_since(double since, double now)
{
    return since < 0 || now < since ? 0 : (unsigned long long)(now - since);
}

void tel_save(const struct telemetry *t, struct tel_saved *s)
{
    memset(s, 0, sizeof(*s));
    s->cap_wh = t->cap_wh;
    s->n50 = t->n50;
    s->n70 = t->n70;
    s->out_total_wh = t->out_total_wh;
    s->e_wh = t->e_wh;
    s->anchored = t->anchored;
    s->soc = t->soc;
    s->learn_active = t->learn_active;
    s->learn_e = t->learn_e;
    s->learn_t = t->learn_t;
    s->learn_socv = t->learn_socv;
    s->part_wh = t->part_wh;
    s->part_w = t->part_w;
    s->part_n = t->part_n;
}

int tel_restore(struct telemetry *t, const struct tel_saved *s, double vbat)
{
    const struct chargerd_config *cfg = t->cfg;

    if (s->cap_wh > 0)
        t->cap_wh = clamp(s->cap_wh, 0.5 * cfg->capacity_rated_wh, 1.1 * cfg->capacity_rated_wh);
    t->n50 = s->n50;
    t->n70 = s->n70;
    t->out_total_wh = s->out_total_wh;
    t->learn_e = s->learn_e;
    t->learn_t = s->learn_t;
    t->learn_socv = s->learn_socv;
    t->learn_active = s->learn_active;
    t->vbat_avg = vbat;
    if (s->part_n > 0 && s->part_wh > 0 && s->part_w > 0)
    {
        t->part_wh = clamp(s->part_wh, 0.5 * cfg->capacity_rated_wh, 1.1 * cfg->capacity_rated_wh);
        t->part_w = s->part_w;
        t->part_n = s->part_n;
    }

    int reuse = 0;
    if (s->anchored)
    {
        double soc_c = clamp(100.0 * (1.0 + s->e_wh / tel_capacity_wh(t)), 0, 100);
        double soc_v = tel_soc_from_voltage(t, vbat);
        reuse = !isnan(soc_v) && fabs(soc_c - soc_v) <= 15.0;
    }
    if (reuse)
    {
        t->e_wh = s->e_wh;
        t->anchored = 1;
    }
    else
    {
        // The unit was off for a while (empty or charged elsewhere). A
        // learning discharge that ended there is evaluated with its last
        // voltage SoC.
        if (t->learn_active)
            learn_finish(t);
        t->anchored = 0;
        t->e_wh = 0;
    }
    update_soc(t);
    return reuse;
}
