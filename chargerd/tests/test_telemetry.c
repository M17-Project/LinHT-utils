// linht-chargerd tests: averages, energy counter, SoC and capacity learning

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "telemetry.h"

static struct chargerd_config cfg;
static struct telemetry t;
static struct tel_in in;
static double now;

// The checks with fixed numbers below use the plain table (100 % at
// 4.20 V per cell), so VREG 8.40 V. vreg_table() and field_learning() run
// at the default 8.30 V.
static void setup(void)
{
    config_defaults(&cfg);
    cfg.charge_voltage_v = 8.40;
    tel_init(&t, &cfg);
    memset(&in, 0, sizeof(in));
    now = 1000;
    in.valid = 1;
    in.tdie = 30;
}

// n samples of 1 s with a constant battery power.
static void run(int n, double vbat, double power)
{
    for (int k = 0; k < n; k++)
    {
        in.now = now;
        in.wall = 1.7e9 + now;
        in.vbat = vbat;
        in.ibat = power / vbat;
        tel_sample(&t, &in);
        in.full_event = 0;
        now += 1;
    }
}

static void averages(void)
{
    setup();
    in.discharging = 1;
    run(29, 8.0, -1.25);
    CHECK(isnan(tel_avg_power(&t, 60)));        // less than half a window
    run(1, 8.0, -1.25);
    CHECK_NEAR(tel_avg_power(&t, 60), -1.25, 1e-6);
    CHECK_NEAR(tel_avg_current(&t, 60), -1.25 / 8.0, 1e-6);
    CHECK(isnan(tel_avg_power(&t, 600)));
    run(60, 8.0, -2.5);
    CHECK_NEAR(tel_avg_power(&t, 60), -2.5, 1e-6);   // only the last minute
    run(300, 8.0, -1.0);
    CHECK_NEAR(tel_avg_power(&t, 600), (30 * -1.25 + 60 * -2.5 + 300 * -1.0) / 390.0, 1e-6);
    CHECK(isnan(tel_avg_power(&t, 3600)));

    // The ring keeps 60 min: older samples drop out.
    run(3600, 8.0, -0.5);
    CHECK_NEAR(tel_avg_power(&t, 3600), -0.5, 1e-6);
    CHECK(t.count == TEL_RING);
}

static void counter_and_soc(void)
{
    setup();
    // Without a full anchor the SoC comes from the voltage table.
    run(5, 7.6, -1.0);                          // 3.80 V per cell
    CHECK_NEAR(t.soc, 50.0, 0.5);
    CHECK(!t.soc_from_counter);

    // Full anchor, then 1 h at 1.48 W = 10 % of 14.8 Wh.
    in.full_event = 1;
    run(1, 8.3, 0);
    CHECK(t.anchored && t.soc_from_counter);
    CHECK_NEAR(t.soc, 100, 1e-9);
    in.discharging = 1;
    run(3600, 8.0, -1.48);
    CHECK_NEAR(t.e_wh, -1.48, 0.002);
    CHECK_NEAR(t.soc, 90.0, 0.02);
    CHECK_NEAR(tel_energy_remaining(&t), 13.32, 0.01);
    CHECK_NEAR(tel_equivalent_cycles(&t), 0.1, 0.001);
    CHECK(tel_since(t.discharge_since, now - 1) == 3599);

    // Run time: (90 - critical 5) % of 14.8 Wh at 1.48 W.
    CHECK(llabs(tel_runtime_s(&t, 1) - (long long)(0.85 * 14.8 / 1.48 * 3600)) <= 30);
    CHECK(tel_runtime_s(&t, 0) == -1);
    CHECK(tel_time_to_full_s(&t, 1) == -1);     // the power is negative

    // A stale measurement holds the last power.
    in.valid = 0;
    run(60, 0, 0);
    CHECK_NEAR(t.e_wh, -1.48 - 1.48 / 60, 0.003);
    in.valid = 1;

    // Charging brings the counter up, never above full.
    in.discharging = 0;
    in.charging = 1;
    run(7200, 8.2, 2.0);
    CHECK(t.e_wh <= 0 && t.soc == 100);
    CHECK(tel_since(t.charge_since, now - 1) == 7199);
    CHECK(t.discharge_since < 0);
}

// Discharge from a full anchor with VBAT falling linearly, then charge:
// logged with its curve.
static void cycle_curve(void)
{
    setup();
    in.full_event = 1;
    run(1, 8.3, 0);
    in.discharging = 1;
    for (int k = 0; k < 36000; k++)
        run(1, 8.3 + (7.4 - 8.3) * k / 36000, -1.2);
    in.discharging = 0;
    in.charging = 1;
    run(1, 8.0, 2.0);
    CHECK(t.cycle_ready && t.cycle.kind == TEL_DISCHARGE);
    CHECK_NEAR(t.cycle.energy_wh, 12.0, 0.01);
    CHECK_NEAR(t.cycle.duration, 36000, 2);
    CHECK_NEAR(t.cycle.curve[0], 8.3, 0.01);
    CHECK_NEAR(t.cycle.curve[10], 7.85, 0.02);
    CHECK_NEAR(t.cycle.curve[TEL_CURVE_PTS - 1], 7.4, 0.02);
}

static void restore(void)
{
    struct tel_saved s;

    setup();
    in.full_event = 1;
    run(1, 8.3, 0);
    in.discharging = 1;
    run(3600 * 5, 7.6, -1.48);                  // 7.4 Wh to 50 %, 0.195 A
    tel_save(&t, &s);
    CHECK(s.anchored && s.learn_active && s.learn_rest);
    CHECK_NEAR(s.learn_rest_e, 7.4, 0.01);
    CHECK_NEAR(s.learn_rest_socv, 50, 0.5);

    // The voltage agrees (7.6 V is 50 %): the counter and the learning
    // discharge continue.
    tel_init(&t, &cfg);
    CHECK(tel_restore(&t, &s, 7.62) == 1);
    CHECK(t.anchored && t.learn_active && t.learn_rest && !t.learned);
    CHECK_NEAR(t.soc, 50, 0.2);

    // It does not (8.3 V is about 93 %): back to the voltage table, and the
    // discharge is evaluated at its last rest point, 7.4 Wh for 50 %.
    tel_init(&t, &cfg);
    CHECK(tel_restore(&t, &s, 8.3) == 0);
    CHECK(!t.anchored && !t.soc_from_counter && !t.learn_active);
    CHECK(t.soc > 90);
    CHECK(t.learned && t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK_NEAR(t.learn_est, 14.8, 0.2);

    // A state file without the learning weight (older daemon): the saved
    // capacity with the weight of the prior, not reliable.
    memset(&s, 0, sizeof(s));
    s.cap_wh = 14.8;
    tel_init(&t, &cfg);
    tel_restore(&t, &s, 7.6);
    CHECK(t.cap_wh == 14.8 && t.learn_n == 0 && !tel_capacity_reliable(&t));
}

// --- Capacity learning with a synthetic battery ---

#define R_PACK  0.15            // ohm
#define R_POL   0.05            // ohm, polarization that relaxes slowly
#define TAU_POL 300.0           // s
#define I_RX    0.16            // A, the unit in RX (or quiet)

static double bsoc;             // true SoC of the synthetic battery
static double vpol;             // its polarization voltage
static int seen;                // a learning discharge ended since the last reset
static unsigned rng = 12345;

static double urand(void)       // -1 .. 1, the same sequence every run
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xffff) / 32767.5 - 1.0;
}

// One second at the battery current i (negative discharges). VBAT is the
// table voltage of the true SoC plus the table error verr (V per pack), the
// IR drop, the polarization and 5 mV of ADC noise. gain scales the reported
// current (a counting error).
static void bstep(double cap, double i, double verr, double gain)
{
    vpol += (i * R_POL - vpol) / TAU_POL;
    double v = tel_voltage_from_soc(&t, bsoc) + verr + i * R_PACK + vpol + 0.005 * urand();
    bsoc += v * i / 3600.0 / cap * 100.0;
    if (bsoc > 100)
        bsoc = 100;
    in.now = now;
    in.wall = 1.7e9 + now;
    in.vbat = v;
    in.ibat = i * gain;
    tel_sample(&t, &in);
    seen |= t.learned;
    in.full_event = 0;
    now += 1;
}

static void b_setup(double vreg)
{
    setup();
    cfg.charge_voltage_v = vreg;
    bsoc = 100;
    vpol = 0;
    seen = 0;
    rng = 12345;
}

// USB in: charge at 1 A for the given seconds.
static void b_charge(double cap, int seconds)
{
    in.discharging = 0;
    in.charging = 1;
    for (int k = 0; k < seconds; k++)
        bstep(cap, 1.0, 0, 1.0);
}

// Charge to full, the full anchor, 1 h on USB at full.
static void b_full(double cap)
{
    seen = 0;
    in.discharging = 0;
    in.charging = 1;
    while (bsoc < 100)
        bstep(cap, 1.0, 0, 1.0);
    in.charging = 0;
    in.full_event = 1;
    for (int k = 0; k < 3600; k++)
        bstep(cap, 0, 0, 1.0);
}

// On battery (or quiet) in RX for the given seconds.
static void b_idle(double cap, int seconds, double verr)
{
    in.discharging = 1;
    in.charging = 0;
    for (int k = 0; k < seconds; k++)
        bstep(cap, -I_RX, verr, 1.0);
}

// Use in the field: RX with a 20 s TX at 1.5 A every 2 min, mean 0.38 A.
static void b_use(double cap, int seconds)
{
    in.discharging = 1;
    in.charging = 0;
    for (int k = 0; k < seconds; k++)
        bstep(cap, k % 120 < 20 ? -1.5 : -I_RX, 0, 1.0);
}

// From full in RX down to the true SoC lo, then USB in.
static void b_cycle(double cap, double lo, double verr, double gain)
{
    b_full(cap);
    in.discharging = 1;
    while (bsoc > lo)
        bstep(cap, -I_RX, verr, gain);
    b_charge(cap, 5);
}

static void learning(void)
{
    const double cap = 11.0;    // 74 % of rated

    // 100 -> 60 % in RX, then USB: learned at the last rest point before
    // the charge. The IR drop and the polarization (32 mV per pack) read
    // the depth 2 % too large, so C_est is about 5 % low.
    b_setup(8.40);
    CHECK(!tel_capacity_reliable(&t) && t.cap_wh == 14.8 && t.learn_n == 0);
    b_cycle(cap, 60, 0, 1.0);
    CHECK(seen && t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK(!t.learn_active);
    CHECK_NEAR(t.learn_depth, 41, 2);
    CHECK_NEAR(t.learn_est, cap, 0.07 * cap);
    double w = (t.learn_depth / 40) * (t.learn_depth / 40);
    CHECK_NEAR(t.learn_weight, w, 1e-9);
    CHECK_NEAR(t.cap_wh, 14.8 + w / (0.5 * 0.9 + w) * (t.learn_est - 14.8), 1e-9);
    CHECK(!tel_capacity_reliable(&t));          // weight 1.5
    CHECK_NEAR(tel_capacity_wh(&t), t.cap_wh, 1e-12);
    CHECK_NEAR(tel_soh(&t), 100 * t.cap_wh / 14.8, 1e-9);

    // The second one makes it reliable, a few more settle it.
    b_cycle(cap, 60, 0, 1.0);
    CHECK(t.learn_n == 2 && tel_capacity_reliable(&t));
    for (int c = 0; c < 4; c++)
        b_cycle(cap, 60, 0.02 * urand(), 1.0);  // table error up to 20 mV per pack
    CHECK(t.learn_n == 6);
    CHECK_NEAR(t.cap_wh, cap, 0.07 * cap);

    // The counter uses it: 20 % of the true capacity is 20 % of the SoC.
    b_full(cap);
    b_idle(cap, (int)(0.2 * cap / (I_RX * 8.0) * 3600), 0);
    CHECK_NEAR(t.soc, 80, 2);
    CHECK_NEAR(t.soc, 100.0 * (1 + t.e_wh / t.cap_wh), 1e-9);

    // One bad discharge (the current reads 40 % high) moves the settled
    // value by a bounded step.
    double before = t.cap_wh;
    b_charge(cap, 5);
    b_cycle(cap, 60, 0, 1.4);
    CHECK(t.learn_n == 7 && t.learn_est > 1.3 * cap);
    CHECK(fabs(t.cap_wh - before) < 0.15 * cap);

    // One deep discharge (to 30 %) is enough for a reliable value. The
    // table is flat there (3.3 mV per % and cell), so the same load drop
    // reads 5 % of SoC and C_est comes out about 7 % low.
    b_setup(8.40);
    b_cycle(cap, 30, 0, 1.0);
    CHECK(t.learn_n == 1 && tel_capacity_reliable(&t));
    CHECK_NEAR(t.learn_est, 0.93 * cap, 0.03 * cap);
    CHECK_NEAR(t.cap_wh, cap, 0.1 * cap);

    // Too shallow (100 -> 80 %): not used.
    b_setup(8.40);
    b_cycle(cap, 80, 0, 1.0);
    CHECK(seen && t.learn_result == TEL_LEARN_SHALLOW);
    CHECK(t.learn_n == 0 && t.cap_wh == 14.8);

    // A current that reads 30 % of the true one: implausible, not used.
    b_setup(8.40);
    b_cycle(cap, 60, 0, 0.3);
    CHECK(t.learn_result == TEL_LEARN_IMPLAUSIBLE && t.learn_n == 0);

    // A steady 1 A load has no rest point: not used, the energy is known.
    b_setup(8.40);
    b_full(cap);
    in.discharging = 1;
    while (bsoc > 50)
        bstep(cap, -1.0, 0, 1.0);
    b_charge(cap, 5);
    CHECK(t.learn_result == TEL_LEARN_NO_REST && t.learn_n == 0);
    CHECK_NEAR(t.learn_e, 0.5 * cap, 0.05 * cap);

    // A full anchor in a poll that still has charge current starts the
    // learning discharge, the current of that poll does not end it.
    b_setup(8.40);
    in.charging = 1;
    in.full_event = 1;
    bstep(cap, 0.3, 0, 1.0);
    CHECK(t.anchored && t.learn_active && !seen);

    // A top-off charge after the anchor ends the learning discharge without
    // energy: nothing to learn.
    b_setup(8.40);
    b_full(cap);
    b_charge(cap, 5);
    CHECK(seen && t.learn_result == TEL_LEARN_NO_REST && t.learn_e < 0.01);

}

static void vreg_table(void)
{
    setup();
    cfg.charge_voltage_v = 8.30;                // the default
    // 4.15 V per cell is 95 % of the plain table and becomes 100 %.
    CHECK_NEAR(tel_soc_from_voltage(&t, 8.30), 100, 1e-9);
    CHECK_NEAR(tel_soc_from_voltage(&t, 8.40), 100, 1e-9);
    CHECK_NEAR(tel_soc_from_voltage(&t, 7.60), 50 / 0.95, 1e-6);
    CHECK_NEAR(tel_soc_from_voltage(&t, 6.60), 0, 1e-9);
    CHECK_NEAR(tel_voltage_from_soc(&t, 100), 8.30, 1e-9);
    for (double soc = 0; soc <= 100; soc += 7)
        CHECK_NEAR(tel_soc_from_voltage(&t, tel_voltage_from_soc(&t, soc)), soc, 1e-6);
    cfg.charge_voltage_v = 8.40;
    CHECK_NEAR(tel_soc_from_voltage(&t, 7.60), 50, 1e-9);
    CHECK_NEAR(tel_voltage_from_soc(&t, 100), 8.40, 1e-9);

    // The learning at 8.30 V: the depth is read on the scaled table, so the
    // capacity is the one of a charge to 8.30 V.
    b_setup(8.30);
    b_cycle(10.0, 60, 0, 1.0);
    CHECK(t.learn_result == TEL_LEARN_USED);
    CHECK_NEAR(t.learn_est, 10.0, 0.07 * 10.0);
}

static void field_learning(void)
{
    const double cap = 12.0;    // of the 8.30 V charge, 81 % of rated

    // Unplugged at full, 30 min idle, 2 h in the field (the TX steps never
    // let VBAT rest), 30 min idle, USB in, about 60 % deep: learned at the
    // rest point before the charge. The end is at 38 %, where the table is
    // flat, so C_est is up to 10 % low (see the deep case of learning()).
    b_setup(8.30);
    b_full(cap);
    b_idle(cap, 1800, 0);
    b_use(cap, 7200);
    b_idle(cap, 1800, 0);
    CHECK(t.learn_active && t.learn_rest && t.learn_n == 0);
    b_charge(cap, 5);
    CHECK(t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK(t.learn_depth > 55 && t.learn_depth < 70);
    CHECK_NEAR(t.learn_est, cap, 0.1 * cap);

    // No rest after the use: the last rest point is before it, too shallow.
    b_setup(8.30);
    b_full(cap);
    b_idle(cap, 1800, 0);
    b_use(cap, 7200);
    b_charge(cap, 5);
    CHECK(t.learn_result == TEL_LEARN_SHALLOW && t.learn_n == 0);

    // A charge in the middle ends the learning discharge. Without a new
    // full anchor the rest of the day is not learned.
    b_setup(8.30);
    b_full(cap);
    b_idle(cap, 1800, 0);
    b_use(cap, 3600);
    b_charge(cap, 300);
    CHECK(!t.learn_active && t.learn_n == 0);
    seen = 0;
    b_use(cap, 3600);
    b_idle(cap, 1800, 0);
    b_charge(cap, 5);
    CHECK(!seen && t.learn_n == 0);

    // A short gap without charge current (USB between two quiet leases)
    // does not end it.
    b_setup(8.30);
    b_full(cap);
    b_idle(cap, 3600, 0);
    in.discharging = 0;
    in.charging = 1;
    for (int k = 0; k < 60; k++)
        bstep(cap, 0, 0, 1.0);
    CHECK(t.learn_active);
    in.discharging = 1;
    in.charging = 0;
    while (bsoc > 55)
        bstep(cap, -I_RX, 0, 1.0);
    b_charge(cap, 5);
    CHECK(t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK_NEAR(t.learn_est, cap, 0.07 * cap);

    // Switched off after 1 h of field use, before any charge: the learning
    // discharge is saved. Charged elsewhere (the voltage disagrees at the
    // next start), it is evaluated at its last rest point.
    b_setup(8.30);
    b_full(cap);
    b_idle(cap, 1800, 0);
    b_use(cap, 3600);
    b_idle(cap, 1200, 0);
    struct tel_saved sv;
    tel_save(&t, &sv);
    CHECK(sv.learn_active && sv.learn_rest && sv.learn_rest_e > 0);
    tel_init(&t, &cfg);
    tel_restore(&t, &sv, 8.3);
    CHECK(t.learned && t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK_NEAR(t.learn_est, cap, 0.07 * cap);

    // Switched on again unchanged: it goes on and ends at the next charge.
    tel_init(&t, &cfg);
    CHECK(tel_restore(&t, &sv, in.vbat) == 1);
    CHECK(t.learn_active && !t.learned);
    b_idle(cap, 1800, 0);
    b_charge(cap, 5);
    CHECK(t.learn_result == TEL_LEARN_USED && t.learn_n == 1);
    CHECK_NEAR(t.learn_est, cap, 0.07 * cap);
}

// --- Discharges of unit B (Rev B, VREG 8.40 V), from its cycles.jsonl ---

struct b_cycle_log
{
    double duration_s, energy_wh;
    float curve[TEL_CURVE_PTS];
};

// The four discharges from a full anchor of 2026-11-21 to 25, in RX at
// 1.3 to 1.4 W. VBAT at every 5 % of the energy.
static const struct b_cycle_log b_log[] = {
    { 10596, 3.809, { 8.348, 8.238, 8.211, 8.186, 8.160, 8.134, 8.108, 8.087, 8.064, 8.050, 8.036,
                      8.019, 8.003, 7.982, 7.969, 7.947, 7.925, 7.908, 7.888, 7.863, 7.845 } },
    { 8596, 3.409, { 8.240, 8.198, 8.172, 8.137, 8.121, 8.097, 8.078, 8.056, 8.036, 8.032, 8.016,
                     8.005, 7.988, 7.974, 7.955, 7.938, 7.918, 7.896, 7.873, 7.867, 7.846 } },
    { 7004, 2.735, { 8.281, 8.231, 8.210, 8.190, 8.164, 8.146, 8.121, 8.105, 8.087, 8.070, 8.061,
                     8.042, 8.035, 8.024, 8.016, 8.003, 7.990, 7.970, 7.957, 7.948, 7.938 } },
    { 12607, 4.436, { 8.239, 8.192, 8.150, 8.119, 8.088, 8.056, 8.028, 8.016, 7.994, 7.967, 7.944,
                      7.920, 7.887, 7.865, 7.844, 7.819, 7.790, 7.770, 7.745, 7.723, 7.696 } },
};

// Full anchor, the logged discharge at its mean power, USB in.
static void b_replay(const struct b_cycle_log *c)
{
    in.discharging = 0;
    in.charging = 0;
    in.full_event = 1;
    run(1, c->curve[0], 0);
    in.discharging = 1;
    double p = c->energy_wh / (c->duration_s / 3600.0);
    for (int s = 0; s < (int)c->duration_s; s++)
    {
        double f = s / c->duration_s * (TEL_CURVE_PTS - 1);
        int k = (int)f;
        double v = c->curve[k] + (c->curve[k + 1] - c->curve[k]) * (f - k);
        run(1, v, -p);
    }
    in.discharging = 0;
    in.charging = 1;
    run(1, 7.9, 2.4);
}

static void unit_b(void)
{
    setup();
    // 2026-11-21: 3.81 Wh to 66.6 %, C_est 11.4 Wh.
    b_replay(&b_log[0]);
    CHECK(t.learn_result == TEL_LEARN_USED);
    CHECK_NEAR(t.learn_est, 11.41, 0.05);
    // 2026-11-23: 3.41 Wh to 66.7 %, C_est 10.2 Wh.
    b_replay(&b_log[1]);
    CHECK_NEAR(t.learn_est, 10.24, 0.05);
    CHECK(t.learn_n == 2 && !tel_capacity_reliable(&t));
    // 2026-11-24: to 72.4 %, too shallow.
    b_replay(&b_log[2]);
    CHECK(t.learn_result == TEL_LEARN_SHALLOW && t.learn_n == 2);
    // 2026-11-25: 4.44 Wh from 100 to 57 %, C_est 10.3 Wh, now reliable.
    b_replay(&b_log[3]);
    CHECK(t.learn_result == TEL_LEARN_USED);
    CHECK_NEAR(t.learn_depth, 43.0, 0.3);
    CHECK_NEAR(t.learn_est, 10.30, 0.05);
    CHECK(t.learn_n == 3 && tel_capacity_reliable(&t));
    CHECK_NEAR(t.cap_wh, 11.13, 0.05);
}

void test_telemetry(void)
{
    averages();
    counter_and_soc();
    cycle_curve();
    learning();
    restore();
    vreg_table();
    field_learning();
    unit_b();
}
