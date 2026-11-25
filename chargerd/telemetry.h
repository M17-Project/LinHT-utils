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

// Result of the last learning discharge (see tel_sample()).
enum tel_learn { TEL_LEARN_NONE, TEL_LEARN_USED, TEL_LEARN_NO_REST, TEL_LEARN_SHALLOW,
                 TEL_LEARN_IMPLAUSIBLE };

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
    double cap_wh;              // learned capacity, the one in use
    double learn_w;             // its weight (0: not in the file)
    int learn_n;                // learning discharges used
    double out_total_wh;        // all discharged energy
    double e_wh;                // counter since the full anchor
    int anchored;
    double soc;                 // last SoC
    double wall;                // when saved
    int learn_active;           // discharge from the full anchor in progress
    int learn_rest;             // it has a rest point
    double learn_rest_e, learn_rest_socv;   // Wh since full and voltage SoC there
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
    double out_total_wh;
    double soc, soc_voltage;
    int soc_from_counter;

    double discharge_since, charge_since;   // monotonic, < 0 when not

    // Capacity learning (see tel_sample()): a decayed weighted mean of the
    // learning discharges, starting at the rated value.
    double cap_wh;              // learned capacity, the one in use
    double learn_w;             // sum of the decayed weights, the prior included
    int learn_n;                // learning discharges used
    int learn_active;           // discharge from the full anchor, no charge since
    double rest_t;              // s discharging without charge current
    int learn_rest;             // a rest point since the anchor
    double learn_rest_e;        // Wh out since the anchor at the last rest point
    double learn_rest_socv;     // voltage SoC there
    // The last learning discharge, for the log.
    int learn_result;           // enum tel_learn
    double learn_e, learn_socv, learn_depth, learn_est, learn_weight;

    int seg_kind;
    struct tel_cycle seg;
    double seg_t0, seg_e;
    float seg_pe[TEL_SEG_MAX], seg_pv[TEL_SEG_MAX];
    unsigned seg_n, seg_step, seg_tick;

    // Events of the last tel_sample(), cleared by the next call.
    int cycle_ready;
    struct tel_cycle cycle;
    int learned;                // a learning discharge ended, see learn_result
    int changed;                // something worth saving
};

void tel_init(struct telemetry *t, const struct chargerd_config *cfg);
void tel_sample(struct telemetry *t, const struct tel_in *in);

// Mean over the last window seconds, NaN with less than half a window.
double tel_avg_power(const struct telemetry *t, unsigned window);
double tel_avg_current(const struct telemetry *t, unsigned window);

// SoC by the open circuit voltage table. 100 % is the configured charge
// voltage (charge_voltage_v), so the table follows VREG.
double tel_soc_from_voltage(const struct telemetry *t, double vbat);
// Inverse of the table: pack open circuit voltage at a SoC.
double tel_voltage_from_soc(const struct telemetry *t, double soc);
// The same without a telemetry, for the policy limits.
double tel_ocv_soc(const struct chargerd_config *cfg, double vbat);
double tel_ocv_voltage(const struct chargerd_config *cfg, double soc);
double tel_energy_remaining(const struct telemetry *t);
double tel_soh(const struct telemetry *t);
double tel_equivalent_cycles(const struct telemetry *t);
// Enough learning discharges (weight at least 2: two of 40 % or one of 50 %).
int tel_capacity_reliable(const struct telemetry *t);
// The capacity of the SoC counter and the estimates: the learned one, the
// rated one until the first learning discharge.
double tel_capacity_wh(const struct telemetry *t);
long long tel_runtime_s(const struct telemetry *t, int discharging);
long long tel_time_to_full_s(const struct telemetry *t, int charging);
unsigned long long tel_since(double since, double now);

void tel_save(const struct telemetry *t, struct tel_saved *s);
// vbat: present voltage, for the 15 % agreement check. Returns 1 when the
// saved counter was reused, 0 when it was dropped. A learning discharge is
// then evaluated with its last rest point (learned is then set).
int tel_restore(struct telemetry *t, const struct tel_saved *s, double vbat);

#endif
