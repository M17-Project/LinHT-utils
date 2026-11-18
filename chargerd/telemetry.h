// linht-chargerd: averages, energy counter, state of charge and capacity
// learning. No I/O. Everything here except the measured inputs and the
// times is an estimate.

#ifndef CHARGERD_TELEMETRY_H
#define CHARGERD_TELEMETRY_H

#include "config.h"

#define TEL_RING 3600           // 1 s samples, 60 min
#define TEL_SEG_MAX 512         // curve samples of one cycle record
#define TEL_CURVE_PTS 21        // VBAT at every 5 % of energy

enum tel_kind { TEL_NONE, TEL_DISCHARGE, TEL_CHARGE };

struct tel_in
{
    double now;                 // monotonic s
    double wall;                // Unix time, for the cycle log
    double vbat, ibat, tdie;    // V, A, degrees C
    int valid;                  // fresh measurement (not stale)
    int discharging;            // the battery supplies the unit (no USB or HIZ)
    int charging;               // policy state "charging"
    int full_event;             // full anchor in this poll
};

// One finished discharge or charge of at least 10 %.
struct tel_cycle
{
    int kind;
    double t_start, t_end;      // Unix time
    double duration;            // s
    double vbat_start, vbat_end;
    double soc_start, soc_end;
    double energy_wh, charge_ah, power_mean_w, tdie_max;
    float curve[TEL_CURVE_PTS];
};

// What survives a restart (state.json).
struct tel_saved
{
    double cap_wh;              // learned capacity
    int n50, n70;               // learning discharges of 50 % and 70 % depth
    double out_total_wh;        // all discharged energy
    double e_wh;                // counter since the full anchor
    int anchored;
    double soc;                 // last SoC
    double wall;                // when saved
    int learn_active;
    double learn_e, learn_t, learn_socv;
    int clean_stop;
};

struct telemetry
{
    const struct chargerd_config *cfg;

    float pbat[TEL_RING], ibat[TEL_RING];
    unsigned head, count;
    double last_now, last_p, last_i;
    int have_last;

    double vbat_avg;            // exponential, tau 30 s
    double e_wh;                // energy since the full anchor, <= 0
    int anchored;
    double cap_wh;
    int n50, n70;
    double out_total_wh;
    double soc, soc_voltage;
    int soc_from_counter;

    double discharge_since, charge_since;   // monotonic, < 0 when not

    int learn_active;
    double learn_e, learn_t, learn_socv;

    int seg_kind;
    struct tel_cycle seg;
    double seg_t0, seg_e;
    float seg_pe[TEL_SEG_MAX], seg_pv[TEL_SEG_MAX];
    unsigned seg_n, seg_step, seg_tick;

    // Events of the last tel_sample(), cleared by the next call.
    int cycle_ready;
    struct tel_cycle cycle;
    int learned;                // capacity updated
    int changed;                // something worth saving
};

void tel_init(struct telemetry *t, const struct chargerd_config *cfg);
void tel_sample(struct telemetry *t, const struct tel_in *in);

// Mean over the last window seconds, NaN with less than half a window.
double tel_avg_power(const struct telemetry *t, unsigned window);
double tel_avg_current(const struct telemetry *t, unsigned window);

double tel_soc_from_voltage(const struct telemetry *t, double vbat);
double tel_energy_remaining(const struct telemetry *t);
double tel_soh(const struct telemetry *t);
double tel_equivalent_cycles(const struct telemetry *t);
int tel_capacity_reliable(const struct telemetry *t);
long long tel_runtime_s(const struct telemetry *t, int discharging);
long long tel_time_to_full_s(const struct telemetry *t, int charging);
unsigned long long tel_since(double since, double now);

void tel_save(const struct telemetry *t, struct tel_saved *s);
// vbat: present voltage, for the 15 % agreement check. Returns 1 when the
// saved counter was reused, 0 when it was dropped.
int tel_restore(struct telemetry *t, const struct tel_saved *s, double vbat);

#endif
