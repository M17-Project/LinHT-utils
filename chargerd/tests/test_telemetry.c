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

static void setup(void)
{
    config_defaults(&cfg);
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

// Discharge from a full anchor with VBAT falling linearly, then charge.
static void discharge(int seconds, double v0, double v1, double power)
{
    in.full_event = 1;
    in.discharging = 0;
    in.charging = 0;
    run(1, v0, 0);
    in.discharging = 1;
    for (int k = 0; k < seconds; k++)
        run(1, v0 + (v1 - v0) * k / seconds, power);
    in.discharging = 0;
    in.charging = 1;
    run(5, 8.0, 2.0);
}

static void learning(void)
{
    // 10 h at 1.2 W = 12 Wh to a voltage SoC of 25 % (3.70 V per cell):
    // C_est = 12 / 0.75 = 16 Wh, C = 14.8 + 0.5 * 0.75 * 1.2 = 15.25 Wh.
    setup();
    discharge(36000, 8.3, 7.4, -1.2);
    CHECK(t.learned || t.n70 == 1);
    CHECK_NEAR(t.cap_wh, 15.25, 0.05);
    CHECK(t.n70 == 1 && t.n50 == 1);
    CHECK(tel_capacity_reliable(&t));
    CHECK_NEAR(tel_soh(&t), 100 * 15.25 / 14.8, 0.4);

    // The discharge was logged with its curve.
    CHECK(t.cycle.kind == TEL_DISCHARGE);
    CHECK_NEAR(t.cycle.energy_wh, 12.0, 0.01);
    CHECK_NEAR(t.cycle.duration, 36000, 2);
    CHECK_NEAR(t.cycle.curve[0], 8.3, 0.01);
    CHECK_NEAR(t.cycle.curve[10], 7.85, 0.02);
    CHECK_NEAR(t.cycle.curve[TEL_CURVE_PTS - 1], 7.4, 0.02);

    // Two discharges of 50 % make it reliable too.
    setup();
    discharge(20000, 8.3, 7.58, -1.0);         // 3.79 V per cell, 48 %
    CHECK(t.n50 == 1 && t.n70 == 0 && !tel_capacity_reliable(&t));
    discharge(20000, 8.3, 7.58, -1.0);
    CHECK(t.n50 == 2 && tel_capacity_reliable(&t));

    // Not deep enough: no update.
    setup();
    discharge(10000, 8.3, 7.9, -1.0);          // 3.95 V per cell = 70 %
    CHECK(t.n50 == 0 && t.cap_wh == 14.8);

    // Heavy load (mean 4 W): no update.
    setup();
    discharge(10000, 8.3, 7.4, -4.0);
    CHECK(t.n50 == 0 && t.cap_wh == 14.8);

    // Clamped to 110 % of rated.
    setup();
    for (int i = 0; i < 6; i++)
        discharge(36000, 8.3, 7.4, -2.5);      // 25 Wh to 25 %
    CHECK_NEAR(t.cap_wh, 1.1 * 14.8, 1e-6);
}

static void restore(void)
{
    struct tel_saved s;

    setup();
    in.full_event = 1;
    run(1, 8.3, 0);
    in.discharging = 1;
    run(3600 * 5, 7.6, -1.48);                  // 50 %
    tel_save(&t, &s);
    CHECK(s.anchored && s.learn_active);

    // The voltage agrees (7.6 V is 50 %): the counter continues.
    tel_init(&t, &cfg);
    CHECK(tel_restore(&t, &s, 7.62) == 1);
    CHECK(t.anchored && t.learn_active);
    CHECK_NEAR(t.soc, 50, 0.2);

    // It does not (8.3 V is about 93 %): back to the voltage table.
    tel_init(&t, &cfg);
    CHECK(tel_restore(&t, &s, 8.3) == 0);
    CHECK(!t.anchored && !t.soc_from_counter);
    CHECK(t.soc > 90);
}

// --- Partial cycles: a synthetic battery between 50 and 70 % ---

#define R_PACK 0.15             // ohm
#define I_HIZ  0.16             // A, load in quiet (HIZ)

static double bsoc;             // true SoC of the synthetic battery
static unsigned rng = 12345;

static double urand(void)       // -1 .. 1, the same sequence every run
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xffff) / 32767.5 - 1.0;
}

// One second with the given battery current. VBAT is the table voltage of
// the true SoC plus the table error, the IR drop, a relaxation term and
// 5 mV ADC noise. gain scales the reported current (a counting error).
static void bstep(double cap, double i, double verr, double relax, double gain)
{
    double v = tel_voltage_from_soc(&t, bsoc) + verr + i * R_PACK + relax + 0.005 * urand();
    bsoc += v * i / 3600.0 / cap * 100.0;
    in.now = now;
    in.wall = 1.7e9 + now;
    in.vbat = v;
    in.ibat = i * gain;
    tel_sample(&t, &in);
    in.full_event = 0;
    now += 1;
}

// Charge at 1 A to hi, then quiet (HIZ) at load amps down to lo. The table
// error moves linearly from e_hi at hi to e_lo at lo (V per pack).
static void partial_cycle(double cap, double lo, double hi, double load, double e_hi, double e_lo,
                          double gain)
{
    in.discharging = 0;
    in.charging = 1;
    while (bsoc < hi)
        bstep(cap, 1.0, e_hi, 0.05, gain);
    in.charging = 0;
    in.discharging = 1;
    for (int k = 0; bsoc > lo; k++)
    {
        double err = e_hi + (e_lo - e_hi) * (hi - bsoc) / (hi - lo);
        bstep(cap, -load, err, 0.06 * exp(-k / 120.0), gain);
    }
    // Charging resumes: the segment ends.
    in.discharging = 0;
    in.charging = 1;
    bstep(cap, 1.0, e_lo, 0.05, gain);
}

static void partial_setup(void)
{
    setup();
    bsoc = 60;
    rng = 12345;
}

static void partial_learning(void)
{
    const double cap = 12.0;    // 81 % of rated

    // Ten cycles 70 -> 50 % with a table error of up to 10 mV per pack at
    // each end (0.7 % SoC): within 10 % of the true capacity.
    partial_setup();
    CHECK(isnan(tel_capacity_partial_wh(&t)));
    CHECK(!strcmp(tel_capacity_source(&t), "none"));
    // Drawn in two statements, because C leaves the order of the arguments
    // of one call open (x86 gcc took the second first, aarch64 the first).
    // This order keeps the values the checks were tuned with.
    double e2 = 0.01 * urand();
    double e1 = 0.01 * urand();
    partial_cycle(cap, 50, 70, I_HIZ, e1, e2, 1.0);
    CHECK(t.part_n == 1);
    CHECK(t.learned_partial || t.part_n == 1);
    // The first cycle moves part of the way from the rated value.
    double first = t.part_wh;
    CHECK(first < 14.8 && first > cap);
    CHECK(14.8 - first < 0.6 * (14.8 - cap));
    CHECK_NEAR(t.part_est, cap, 0.1 * cap);
    CHECK(t.part_depth > 15 && t.part_depth < 20);  // the settle time is not counted
    for (int c = 1; c < 10; c++) {
        double f2 = 0.01 * urand();
        double f1 = 0.01 * urand();
        partial_cycle(cap, 50, 70, I_HIZ, f1, f2, 1.0);
    }
    CHECK(t.part_n == 10);
    CHECK_NEAR(t.part_wh, cap, 0.1 * cap);
    CHECK(!tel_capacity_reliable(&t) && t.cap_wh == 14.8);   // never full: no full learning
    CHECK(!strcmp(tel_capacity_source(&t), "partial"));
    CHECK_NEAR(tel_capacity_wh(&t), t.part_wh, 1e-12);
    CHECK_NEAR(tel_soh(&t), 100 * t.part_wh / 14.8, 1e-9);

    // One bad cycle (the current reads 40 % high) moves it by a bounded step.
    double before = t.part_wh;
    partial_cycle(cap, 50, 70, I_HIZ, 0, 0, 1.4);
    CHECK(t.part_n == 11);
    CHECK(t.part_est > 1.3 * cap);
    CHECK(fabs(t.part_wh - before) < 0.06 * cap);

    // A shallow segment (10 %) and a heavy load are not used.
    int n = t.part_n;
    partial_cycle(cap, 58, 70, I_HIZ, 0, 0, 1.0);
    CHECK(t.part_n == n);
    partial_cycle(cap, 40, 70, 1.0, 0, 0, 1.0);
    CHECK(t.part_n == n);

    // A short gap between two leases (USB, no charge current) does not end
    // the segment.
    partial_setup();
    in.charging = 1;
    while (bsoc < 70)
        bstep(cap, 1.0, 0, 0.05, 1.0);
    in.charging = 0;
    in.discharging = 1;
    for (int k = 0; k < 3600; k++)
        bstep(cap, -I_HIZ, 0, 0, 1.0);
    in.discharging = 0;
    in.charging = 1;
    bstep(cap, 0.0, 0, 0, 1.0);
    bstep(cap, 0.0, 0, 0, 1.0);
    CHECK(t.part_state == 2);
    partial_cycle(cap, 50, bsoc, I_HIZ, 0, 0, 1.0);
    CHECK(t.part_n == 1 && t.part_depth > 15);
    CHECK_NEAR(t.part_est, cap, 0.05 * cap);

    // Saved and restored with the state file.
    struct tel_saved sv;
    tel_save(&t, &sv);
    double pw = t.part_wh;
    tel_init(&t, &cfg);
    tel_restore(&t, &sv, 7.6);
    CHECK(t.part_n == 1 && t.part_wh == pw && t.part_w == sv.part_w);

    // A reliable full-anchored value wins, the partial one stays next to it.
    partial_setup();
    for (int c = 0; c < 3; c++)
        partial_cycle(cap, 50, 70, I_HIZ, 0, 0, 1.0);
    CHECK(!strcmp(tel_capacity_source(&t), "partial"));
    discharge(36000, 8.3, 7.4, -1.2);           // the learning() case, 15.25 Wh
    CHECK(tel_capacity_reliable(&t));
    CHECK(!strcmp(tel_capacity_source(&t), "full"));
    CHECK_NEAR(tel_soh(&t), 100 * 15.25 / 14.8, 0.4);
    CHECK(t.part_n >= 3 && !isnan(tel_capacity_partial_wh(&t)));
}

void test_telemetry(void)
{
    averages();
    counter_and_soc();
    learning();
    restore();
    partial_learning();
}
