// linht-chargerd: replay of logged battery data through the telemetry.
//
//   tests/replay-stream.py cycles cycles.jsonl | tests/replay [VREG]
//
// Reads one poll per line, "now wall vbat ibat valid discharging charging
// full_event" (s, Unix s, V, A, 0/1 flags), and prints every learning
// discharge and the state at the end. VREG (default 8.40 V) is the charge
// voltage the data was taken with, it scales the voltage table.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "telemetry.h"

static const char *result_name[] = { "-", "used", "no rest point", "too shallow", "implausible" };

int main(int argc, char **argv)
{
    struct chargerd_config cfg;
    config_defaults(&cfg);
    cfg.charge_voltage_v = argc > 1 ? atof(argv[1]) : 8.40;

    struct telemetry t;
    tel_init(&t, &cfg);
    struct tel_in in = { 0 };
    char line[256];
    while (fgets(line, sizeof(line), stdin))
    {
        if (sscanf(line, "%lf %lf %lf %lf %d %d %d %d", &in.now, &in.wall, &in.vbat, &in.ibat,
                   &in.valid, &in.discharging, &in.charging, &in.full_event) != 8)
            continue;
        tel_sample(&t, &in);
        if (!t.learned || (t.learn_result == TEL_LEARN_NO_REST && t.learn_e < 0.5))
            continue;
        time_t w = (time_t)in.wall;
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", gmtime(&w));
        printf("%s %-12s %5.2f Wh to %5.1f %%, %5.1f %% deep, C_est %5.2f Wh, weight %4.2f, "
               "capacity %5.2f Wh from %d (weight %.2f%s)\n", ts, result_name[t.learn_result],
               t.learn_e, t.learn_socv, t.learn_depth, t.learn_est, t.learn_weight, t.cap_wh,
               t.learn_n, t.learn_w, tel_capacity_reliable(&t) ? ", reliable" : "");
    }
    printf("end: capacity %.2f Wh from %d discharges, weight %.2f, %s, SoC %.1f %% (%s), "
           "voltage SoC %.1f %%\n", t.cap_wh, t.learn_n, t.learn_w,
           tel_capacity_reliable(&t) ? "reliable" : "not reliable", t.soc,
           t.soc_from_counter ? "counter" : "voltage", t.soc_voltage);
    return 0;
}
