// linht-chargerd: averages, energy counter, state of charge and capacity
// learning. No I/O.

#include <math.h>
#include <string.h>

#include "telemetry.h"

#define VBAT_TAU_S       30.0   // s, voltage average for the voltage SoC
#define DT_MAX_S         5.0    // s, longer gaps are not integrated
#define CYCLE_MIN_SOC    10.0   // %, shorter cycles are not logged
#define CHARGE_IBAT_MIN  0.05   // A, a real charge ends a learning discharge

// Capacity learning. A learning discharge starts at the full anchor (SoC
// 100 % by definition) and ends at the first charge current. It is
// evaluated at its last rest point: C_est = E_out / (1 - SoC_v / 100), with
// E_out the counter since the anchor and SoC_v the voltage SoC at that
// point. A rest point is a moment after at least 10 min of discharge without
// charge current at a light mean load (10 min and 1 min), so VBAT is near
// the open circuit voltage at the load of the radio in RX. Heavy use before
// it does not matter, the energy is counted. The table error at SoC_v moves
// C_est by that error over the depth, so deeper discharges weigh more.
#define LEARN_REST_S     600.0  // s of discharge without charge before a rest point
#define LEARN_REST_I     0.3    // A, the 10 min and 1 min mean load of a rest point
#define LEARN_DEPTH_MIN  30.0   // %, shallower discharges are not used
#define LEARN_DEPTH_REF  40.0   // %, depth of weight 1 (100 -> 60 %)
#define LEARN_W_MAX      4.0    // weight limit (80 % deep)
#define LEARN_PRIOR_W    0.5    // weight of the rated value at the start
#define LEARN_DECAY      0.9    // older discharges lose 10 % per new one
#define LEARN_W_RELIABLE 2.0    // two of 40 % or one of 50 % deep
#define LEARN_EST_LO     0.4    // an estimate outside 0.4 to 1.6 times rated
#define LEARN_EST_HI     1.6    //   is dropped as implausible

// Per cell open circuit points of a generic NMC cell, the table of
// field-log.py, 100 % at 4.20 V. Under load and while charging it is off by
// 10 to 20 %. The daemon scales it so that 100 % is the configured charge
// voltage: at 4.15 V per cell the table says 95 %, which becomes 100 %.
static const double ocv[][2] = {
    { 3.30, 0 }, { 3.50, 5 }, { 3.60, 10 }, { 3.70, 25 }, { 3.75, 40 }, { 3.80, 50 },
    { 3.87, 60 }, { 3.95, 70 }, { 4.03, 80 }, { 4.10, 90 }, { 4.20, 100 },
};

void tel_init(struct telemetry *t, const struct chargerd_config *cfg)
{
    memset(t, 0, sizeof(*t));
    t->cfg = cfg;
    t->cap_wh = cfg->capacity_rated_wh;
    t->learn_w = LEARN_PRIOR_W;
    t->vbat_avg = NAN;
    t->soc = NAN;
    t->soc_voltage = NAN;
    t->discharge_since = -1;
    t->charge_since = -1;
    t->learn_rest_e = t->learn_rest_socv = NAN;
    t->learn_e = t->learn_socv = t->learn_depth = t->learn_est = t->learn_weight = NAN;
}

#define OCV_N ((int)(sizeof(ocv) / sizeof(ocv[0])))

static double table_soc(double v)       // per cell
{
    if (v <= ocv[0][0])
        return 0;
    for (int i = 1; i < OCV_N; i++)
        if (v <= ocv[i][0])
            return ocv[i - 1][1] + (ocv[i][1] - ocv[i - 1][1]) * (v - ocv[i - 1][0]) /
                   (ocv[i][0] - ocv[i - 1][0]);
    return 100;
}

static double table_voltage(double soc) // per cell
{
    if (soc <= ocv[0][1])
        return ocv[0][0];
    for (int i = 1; i < OCV_N; i++)
        if (soc <= ocv[i][1])
            return ocv[i - 1][0] + (ocv[i][0] - ocv[i - 1][0]) * (soc - ocv[i - 1][1]) /
                                       (ocv[i][1] - ocv[i - 1][1]);
    return ocv[OCV_N - 1][0];
}

static int cells_of(const struct chargerd_config *cfg)
{
    return cfg->cells > 0 ? cfg->cells : 2;
}

// Table SoC of the configured charge voltage, the 100 % of the daemon.
static double table_top(const struct chargerd_config *cfg)
{
    double v = cfg->charge_voltage_v;
    if (!(v > 0))
        return 100;
    double top = table_soc(v / cells_of(cfg));
    return top >= 50 ? top : 100;
}

double tel_ocv_soc(const struct chargerd_config *cfg, double vbat)
{
    if (isnan(vbat) || vbat < 1.0)
        return NAN;
    double soc = 100.0 * table_soc(vbat / cells_of(cfg)) / table_top(cfg);
    return soc > 100 ? 100 : soc;
}

double tel_ocv_voltage(const struct chargerd_config *cfg, double soc)
{
    if (isnan(soc))
        return NAN;
    if (soc > 100)
        soc = 100;
    return cells_of(cfg) * table_voltage(soc * table_top(cfg) / 100.0);
}

double tel_soc_from_voltage(const struct telemetry *t, double vbat)
{
    return tel_ocv_soc(t->cfg, vbat);
}

double tel_voltage_from_soc(const struct telemetry *t, double soc)
{
    return tel_ocv_voltage(t->cfg, soc);
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

// End of a learning discharge, evaluated at its last rest point.
static void learn_finish(struct telemetry *t)
{
    const struct chargerd_config *cfg = t->cfg;

    if (!t->learn_active)
        return;
    t->learn_active = 0;
    t->learned = 1;
    t->changed = 1;
    t->learn_socv = t->learn_depth = t->learn_est = t->learn_weight = NAN;
    if (!t->learn_rest)
    {
        // The energy since the anchor, so the log can tell a real discharge
        // without a rest point from a top-off after the anchor.
        t->learn_e = -t->e_wh;
        t->learn_result = TEL_LEARN_NO_REST;
        return;
    }
    t->learn_rest = 0;
    t->learn_e = t->learn_rest_e;
    t->learn_socv = t->learn_rest_socv;
    double depth = 100.0 - t->learn_socv;
    t->learn_depth = depth;
    if (depth < LEARN_DEPTH_MIN)
    {
        t->learn_result = TEL_LEARN_SHALLOW;
        return;
    }
    double est = t->learn_e / (depth / 100.0);
    t->learn_est = est;
    if (est < LEARN_EST_LO * cfg->capacity_rated_wh || est > LEARN_EST_HI * cfg->capacity_rated_wh)
    {
        t->learn_result = TEL_LEARN_IMPLAUSIBLE;
        return;
    }

    // A table error of a few % of SoC at the rest point moves C_est by that
    // error over the depth: weight by depth squared.
    double w = (depth / LEARN_DEPTH_REF) * (depth / LEARN_DEPTH_REF);
    if (w > LEARN_W_MAX)
        w = LEARN_W_MAX;
    t->learn_w = t->learn_w * LEARN_DECAY + w;
    t->cap_wh += w / t->learn_w * (est - t->cap_wh);
    t->learn_n++;
    t->learn_weight = w;
    t->learn_result = TEL_LEARN_USED;
}

// Rest points of a learning discharge. Called after the ring got the
// sample of this poll.
static void rest_sample(struct telemetry *t, const struct tel_in *in, double i, double dt)
{
    if (in->discharging && i <= CHARGE_IBAT_MIN)
        t->rest_t += dt;
    else
        t->rest_t = 0;

    if (!t->learn_active || !in->discharging || t->rest_t < LEARN_REST_S || !in->valid ||
        isnan(t->soc_voltage))
        return;
    double i10 = -tel_avg_current(t, (unsigned)LEARN_REST_S);
    double i1 = -tel_avg_current(t, 60);
    if (isnan(i10) || isnan(i1) || i10 > LEARN_REST_I || i1 > LEARN_REST_I)
        return;
    t->learn_rest = 1;
    t->learn_rest_e = -t->e_wh;
    t->learn_rest_socv = t->soc_voltage;
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
        learn_finish(t);
        t->e_wh = 0;
        t->anchored = 1;
        t->learn_active = 1;
        t->learn_rest = 0;
        t->changed = 1;
    }
    else if (in->charging && i > CHARGE_IBAT_MIN)
    {
        learn_finish(t);
    }
    update_soc(t);
    rest_sample(t, in, i, dt);

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
    return t->cap_wh;
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
    return t->learn_w >= LEARN_W_RELIABLE;
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
    s->learn_w = t->learn_w;
    s->learn_n = t->learn_n;
    s->out_total_wh = t->out_total_wh;
    s->e_wh = t->e_wh;
    s->anchored = t->anchored;
    s->soc = t->soc;
    s->learn_active = t->learn_active;
    s->learn_rest = t->learn_active && t->learn_rest;
    s->learn_rest_e = s->learn_rest ? t->learn_rest_e : NAN;
    s->learn_rest_socv = s->learn_rest ? t->learn_rest_socv : NAN;
}

int tel_restore(struct telemetry *t, const struct tel_saved *s, double vbat)
{
    const struct chargerd_config *cfg = t->cfg;
    double rated = cfg->capacity_rated_wh;

    if (s->cap_wh >= LEARN_EST_LO * rated && s->cap_wh <= LEARN_EST_HI * rated)
        t->cap_wh = s->cap_wh;
    if (s->learn_w > 0)
        t->learn_w = s->learn_w;
    t->learn_n = s->learn_n;
    t->out_total_wh = s->out_total_wh;
    t->learn_active = s->learn_active;
    t->learn_rest = s->learn_active && s->learn_rest && !isnan(s->learn_rest_e) &&
                    !isnan(s->learn_rest_socv);
    if (t->learn_rest)
    {
        t->learn_rest_e = s->learn_rest_e;
        t->learn_rest_socv = s->learn_rest_socv;
    }
    t->vbat_avg = vbat;

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
        // learning discharge is evaluated with its last rest point.
        learn_finish(t);
        t->anchored = 0;
        t->learn_active = 0;
        t->e_wh = 0;
    }
    update_soc(t);
    return reuse;
}
