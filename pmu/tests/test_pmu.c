#include <assert.h>
#include <stdio.h>
#include "pmu.h"

static pmu_t p;
static pmu_inputs_t in;
static uint32_t now;

static void tick(unsigned count)
{
    while (count--) {
        pmu_step(&p, ++now, in);
        assert(!p.out.rail_3v3 || p.out.rail_5v);
        assert(!p.out.side_forward || (p.out.side_pullup && p.out.rail_5v));
        if (p.state == PMU_OFF) {
            assert(!p.out.rail_5v && !p.out.rail_3v3);
            assert(!p.out.side_pullup && !p.out.side_forward);
            assert(!p.out.usb_boot && !p.out.on_out_low);
        }
    }
}

static void reset(bool switch_on, uint32_t time)
{
    now = time;
    in = (pmu_inputs_t){.switch_on = switch_on};
    pmu_init(&p, now, switch_on);
}

static void select_boot(bool pressed)
{
    in.side_pressed = pressed;
    in.switch_on = true;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_SAMPLE_BOOT_BUTTON);
    assert(p.out.side_pullup && !p.out.rail_5v && !p.out.usb_boot);
    tick(PMU_BUTTON_SETTLE_MS + PMU_DEBOUNCE_MS);
    assert(p.state == PMU_STARTUP);
    assert(p.out.rail_5v && !p.out.rail_3v3 && p.out.side_forward);
    assert(p.out.usb_boot == pressed);
}

static void run(void)
{
    tick(STARTUP_3V3_DELAY_MS - 1);
    assert(!p.out.rail_3v3);
    tick(1);
    assert(p.state == PMU_RUNNING && p.out.rail_3v3);
}

static void normal_boot_and_off(void)
{
    reset(false, 0);
    /* Floating PB5 is ignored while OFF, even when read as pressed. */
    in.side_pressed = true;
    tick(100);
    assert(p.state == PMU_OFF);
    /* Switch bounce cannot power the SoM. */
    for (unsigned i = 0; i < 20; i++) {
        in.switch_on = !in.switch_on;
        tick(5);
    }
    assert(p.state == PMU_OFF);
    select_boot(false);
    run();
    /* Pressing the side key later never changes the boot selection. */
    in.side_pressed = true;
    tick(100);
    assert(!p.out.usb_boot);
    /* OFF_REQ initially LOW is ignored until a qualified HIGH was seen. */
    tick(100);
    assert(p.state == PMU_RUNNING);
    in.off_req_n = true;
    tick(PMU_DEBOUNCE_MS + 1);
    in.off_req_n = false;
    tick(PMU_DEBOUNCE_MS - 1);
    assert(p.state == PMU_RUNNING);
    tick(2);
    assert(p.state == PMU_OFF);
    tick(1000);
    assert(p.state == PMU_OFF); /* Knob still ON cannot restart the device. */
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    select_boot(false);
}

static void usb_boot_and_shutdown(void)
{
    reset(false, 0);
    select_boot(true);
    in.side_pressed = false;
    in.off_req_n = true;
    run();
    assert(p.out.usb_boot); /* Releasing the key does not release the strap. */
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_SHUTDOWN && p.out.on_out_low);
    tick(PMU_ON_OUT_PULSE_MS - 1);
    assert(p.out.on_out_low);
    tick(1);
    assert(!p.out.on_out_low && p.out.rail_5v);
    while ((uint32_t)(now - p.powered_since) < PMU_USB_BOOT_HOLD_MS - 1) tick(1);
    assert(p.out.usb_boot);
    tick(1);
    assert(!p.out.usb_boot && p.state == PMU_SHUTDOWN);
    /* Short feedback glitch must not cut power. */
    in.off_req_n = false;
    tick(10);
    in.off_req_n = true;
    tick(50);
    assert(p.state == PMU_SHUTDOWN);
    in.off_req_n = false;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_OFF);
}

static void unstable_boot_button(void)
{
    reset(false, 0);
    in.switch_on = true;
    tick(PMU_DEBOUNCE_MS + 1);
    for (unsigned i = 0; i < PMU_BUTTON_TIMEOUT_MS / 5; i++) {
        in.side_pressed = !in.side_pressed;
        tick(5);
    }
    assert(p.state == PMU_STARTUP && !p.out.usb_boot);

    reset(false, 0);
    in.switch_on = true;
    tick(PMU_DEBOUNCE_MS + 1);
    for (unsigned i = 0; i < 10; i++) {
        in.side_pressed = !in.side_pressed;
        tick(3);
    }
    in.side_pressed = true;
    tick(PMU_DEBOUNCE_MS);
    assert(!p.out.rail_5v);
    tick(1);
    assert(p.state == PMU_STARTUP && p.out.usb_boot);
}

static void cancel_startup(void)
{
    reset(false, 0);
    in.switch_on = true;
    tick(PMU_DEBOUNCE_MS + 1);
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_OFF);
    select_boot(true);
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_OFF);
}

static void reset_with_switch_on(void)
{
    reset(true, 0);
    in.side_pressed = true;
    tick(10000);
    assert(p.state == PMU_OFF);
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    select_boot(false);
}

static void shutdown_timeout_and_wraparound(void)
{
    reset(false, UINT32_MAX - 100);
    select_boot(true);
    run();
    in.switch_on = false;
    tick(PMU_DEBOUNCE_MS + 1);
    assert(p.state == PMU_SHUTDOWN);
    /* Turning back ON does not cancel an in-progress shutdown. */
    in.switch_on = true;
    tick(PMU_SHUTDOWN_MS - 1);
    assert(p.out.rail_5v);
    tick(1);
    assert(p.state == PMU_OFF);
    tick(1000);
    assert(p.state == PMU_OFF);
}

int main(void)
{
    normal_boot_and_off();
    usb_boot_and_shutdown();
    unstable_boot_button();
    cancel_startup();
    reset_with_switch_on();
    shutdown_timeout_and_wraparound();
    puts("PMU: 6 scenario groups passed");
    return 0;
}
