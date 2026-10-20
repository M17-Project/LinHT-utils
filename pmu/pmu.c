#include "pmu.h"

static bool elapsed(uint32_t now, uint32_t since, uint32_t duration)
{
    return (uint32_t)(now - since) >= duration;
}

static pmu_debounce_t debounce_init(bool sample, uint32_t now, bool valid)
{
    return (pmu_debounce_t){
        .candidate = sample, .state = sample, .valid = valid, .since = now,
    };
}

/* Require consecutive stable time; earlier bounce never contributes to it. */
static void debounce_update(pmu_debounce_t *db, bool sample, uint32_t now)
{
    if (db->candidate != sample) {
        db->candidate = sample;
        db->since = now;
    }
    if (elapsed(now, db->since, PMU_DEBOUNCE_MS)) {
        db->state = sample;
        db->valid = true;
    }
}

static void power_off(pmu_t *pmu, uint32_t now)
{
    pmu->out = (pmu_outputs_t){0};
    pmu->state = PMU_OFF;
    pmu->state_since = now;
    pmu->switch_armed = !pmu->on_switch.state;
    pmu->off_req_seen_high = false;
}

void pmu_init(pmu_t *pmu, uint32_t now, bool switch_on)
{
    *pmu = (pmu_t){0};
    pmu->on_switch = debounce_init(switch_on, now, true);
    power_off(pmu, now);
}

void pmu_step(pmu_t *pmu, uint32_t now, pmu_inputs_t in)
{
    debounce_update(&pmu->on_switch, in.switch_on, now);

    /* Timed outputs must also expire during shutdown. */
    if (pmu->out.usb_boot &&
        elapsed(now, pmu->powered_since, PMU_USB_BOOT_HOLD_MS)) {
        pmu->out.usb_boot = false;
    }
    if (pmu->out.on_out_low &&
        elapsed(now, pmu->state_since, PMU_ON_OUT_PULSE_MS)) {
        pmu->out.on_out_low = false;
    }

    if (pmu->out.rail_5v) {
        debounce_update(&pmu->off_req, in.off_req_n, now);
    }

    if (pmu->state == PMU_RUNNING || pmu->state == PMU_SHUTDOWN) {
        if (pmu->off_req.valid && pmu->off_req.state) {
            pmu->off_req_seen_high = true;
        }
        if (pmu->off_req_seen_high && pmu->off_req.valid &&
            !pmu->off_req.state) {
            power_off(pmu, now);
            return;
        }
    }

    switch (pmu->state) {
    case PMU_OFF:
        if (!pmu->on_switch.state) {
            pmu->switch_armed = true;
        } else if (pmu->switch_armed) {
            pmu->switch_armed = false;
            pmu->out.side_pullup = true;
            pmu->boot_sample_started = false;
            pmu->state_since = now;
            pmu->state = PMU_SAMPLE_BOOT_BUTTON;
        }
        break;

    case PMU_SAMPLE_BOOT_BUTTON:
        if (!pmu->on_switch.state) {
            power_off(pmu, now);
            break;
        }
        if (!elapsed(now, pmu->state_since, PMU_BUTTON_SETTLE_MS)) {
            break;
        }
        if (!pmu->boot_sample_started) {
            pmu->boot_button = debounce_init(in.side_pressed, now, false);
            pmu->boot_sample_started = true;
        }
        debounce_update(&pmu->boot_button, in.side_pressed, now);
        if (pmu->boot_button.valid ||
            elapsed(now, pmu->state_since, PMU_BUTTON_TIMEOUT_MS)) {
            /* An unstable button falls back to normal boot, never USB boot. */
            pmu->out.usb_boot = pmu->boot_button.valid && pmu->boot_button.state;
            pmu->out.rail_5v = true;
            pmu->out.side_forward = true;
            pmu->powered_since = now;
            pmu->off_req = debounce_init(in.off_req_n, now, false);
            pmu->off_req_seen_high = false;
            pmu->state = PMU_STARTUP;
        }
        break;

    case PMU_STARTUP:
        if (!pmu->on_switch.state) {
            power_off(pmu, now);
        } else if (elapsed(now, pmu->powered_since, STARTUP_3V3_DELAY_MS)) {
            pmu->out.rail_3v3 = true;
            pmu->state = PMU_RUNNING;
        }
        break;

    case PMU_RUNNING:
        if (!pmu->on_switch.state) {
            pmu->out.on_out_low = true;
            pmu->state_since = now;
            pmu->state = PMU_SHUTDOWN;
        }
        break;

    case PMU_SHUTDOWN:
        if (elapsed(now, pmu->state_since, PMU_SHUTDOWN_MS)) {
            power_off(pmu, now);
        }
        break;

    default:
        power_off(pmu, now);
        break;
    }
}
