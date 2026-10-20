#ifndef LINHT_PMU_H
#define LINHT_PMU_H

#include <stdbool.h>
#include <stdint.h>

#define PMU_DEBOUNCE_MS          40UL
#define PMU_BUTTON_SETTLE_MS      1UL
#define PMU_BUTTON_TIMEOUT_MS   500UL
#ifndef STARTUP_3V3_DELAY_MS
#define STARTUP_3V3_DELAY_MS    3000UL
#endif
#define PMU_USB_BOOT_HOLD_MS   5000UL
#define PMU_SHUTDOWN_MS       20000UL
#define PMU_ON_OUT_PULSE_MS     200UL

typedef enum {
    PMU_OFF,
    PMU_SAMPLE_BOOT_BUTTON,
    PMU_STARTUP,
    PMU_RUNNING,
    PMU_SHUTDOWN,
} pmu_state_t;

typedef struct {
    bool candidate;
    bool state;
    bool valid;
    uint32_t since;
} pmu_debounce_t;

typedef struct {
    bool switch_on;
    bool side_pressed;
    bool off_req_n;
} pmu_inputs_t;

typedef struct {
    bool rail_5v;
    bool rail_3v3;
    bool usb_boot;
    bool on_out_low;
    bool side_pullup;
    bool side_forward;
} pmu_outputs_t;

typedef struct {
    pmu_state_t state;
    pmu_outputs_t out;
    pmu_debounce_t on_switch;
    pmu_debounce_t off_req;
    pmu_debounce_t boot_button;
    bool switch_armed;
    bool off_req_seen_high;
    bool boot_sample_started;
    uint32_t state_since;
    uint32_t powered_since;
} pmu_t;

void pmu_init(pmu_t *pmu, uint32_t now, bool switch_on);
void pmu_step(pmu_t *pmu, uint32_t now, pmu_inputs_t in);

#endif
