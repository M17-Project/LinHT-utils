#ifndef F_CPU
#define F_CPU 1000000UL
#endif
#if F_CPU != 1000000UL
#error "PMU requires a 1 MHz CPU clock (16 MHz oscillator, /16 prescaler)"
#endif

#include <avr/cpufunc.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <util/atomic.h>

#include "pmu.h"

/* PORTA: PA0 UPDI; PA1 OFF_REQ_N; PA2 switch; PA3 USB_BOOT;
 * PA4 SoM reset (Hi-Z); PA5 ON_OUT_N; PA6 5V EN; PA7 3V3 EN.
 * PB5 physical button; PC0 open-drain button output to the 1.8 V SoM.
 */
static volatile uint32_t uptime_ms;
static volatile bool side_forwarding;
static bool side_pullup_enabled;

static inline void side_release(void)
{
    PORTC.DIRCLR = PIN0_bm;
}

static inline void side_forward(void)
{
    /* PC0 OUT is always zero; never enable its pull-up or drive it HIGH. */
    if (VPORTB.IN & PIN5_bm) {
        side_release();
    } else {
        PORTC.DIRSET = PIN0_bm;
    }
}

ISR(PORTB_PORT_vect)
{
    PORTB.INTFLAGS = PIN5_bm;
    if (side_forwarding) {
        side_forward();
    }
}

ISR(TCB0_INT_vect)
{
    TCB0.INTFLAGS = TCB_CAPT_bm;
    uptime_ms++;
}

static void init_hardware(void)
{
    /* Set output latches before directions. Hardware EN pulldowns keep rails off
     * during reset and programming. Keep every connection to the SoM Hi-Z.
     */
    PORTA.OUTCLR = PIN3_bm | PIN5_bm | PIN6_bm | PIN7_bm;
    PORTA.DIRCLR = PIN1_bm | PIN2_bm | PIN3_bm | PIN4_bm | PIN5_bm;
    PORTA.DIRSET = PIN6_bm | PIN7_bm;
    PORTA.PIN2CTRL = PORT_PULLUPEN_bm;
    PORTB.DIRCLR = PIN5_bm;
    PORTB.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
    PORTC.OUTCLR = PIN0_bm;
    PORTC.DIRCLR = PIN0_bm;
    PORTC.PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc;

    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PDIV_16X_gc | CLKCTRL_PEN_bm);

    /* Fuses enable and lock the 8K watchdog at reset. Use the same period if
     * running on an MCU whose watchdog has not yet been enabled by fuses.
     */
    while (WDT.STATUS & WDT_SYNCBUSY_bm) {}
    if (!(WDT.STATUS & WDT_LOCK_bm)) {
        _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8KCLK_gc);
    }
    wdt_reset();

    /* Hardware timebase: 1 MHz / 1000 = 1 kHz. */
    TCB0.CTRLB = TCB_CNTMODE_INT_gc;
    TCB0.CCMP = (F_CPU / 1000UL) - 1UL;
    TCB0.INTFLAGS = TCB_CAPT_bm;
    TCB0.INTCTRL = TCB_CAPT_bm;
    TCB0.CTRLA = TCB_CLKSEL_DIV1_gc | TCB_ENABLE_bm;
}

static void apply_outputs(const pmu_outputs_t *out)
{
    /* Disable consumers before removing the SoM supply. */
    if (!out->rail_3v3) PORTA.OUTCLR = PIN7_bm;
    if (!out->rail_5v) PORTA.OUTCLR = PIN6_bm;

    /* The boot strap must be valid BEFORE the 5 V enable edge. */
    if (out->usb_boot) {
        PORTA.OUTSET = PIN3_bm;
        PORTA.DIRSET = PIN3_bm;
    } else {
        PORTA.DIRCLR = PIN3_bm;
    }

    if (out->on_out_low) {
        PORTA.DIRSET = PIN5_bm;
    } else {
        PORTA.DIRCLR = PIN5_bm;
    }

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (out->side_pullup != side_pullup_enabled) {
            side_pullup_enabled = out->side_pullup;
            PORTB.PIN5CTRL = out->side_pullup
                ? PORT_PULLUPEN_bm : PORT_ISC_INPUT_DISABLE_gc;
        }
        if (out->side_forward != side_forwarding) {
            side_forwarding = out->side_forward;
            PORTB.INTFLAGS = PIN5_bm;
            PORTB.PIN5CTRL = out->side_forward
                ? PORT_PULLUPEN_bm | PORT_ISC_BOTHEDGES_gc
                : (out->side_pullup ? PORT_PULLUPEN_bm : PORT_ISC_INPUT_DISABLE_gc);
            if (out->side_forward) side_forward();
            else side_release();
        }
    }

    if (out->rail_5v) PORTA.OUTSET = PIN6_bm;
    if (out->rail_3v3) PORTA.OUTSET = PIN7_bm;
}

int main(void)
{
    pmu_t pmu;
    uint32_t previous_ms = 0;

    init_hardware();
    pmu_init(&pmu, 0, !(VPORTA.IN & PIN2_bm));
    apply_outputs(&pmu.out);
    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();

    for (;;) {
        uint32_t now;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            now = uptime_ms;
        }
        if (now != previous_ms) {
            pmu_inputs_t in = {
                .switch_on = !(VPORTA.IN & PIN2_bm),
                .side_pressed = side_pullup_enabled && !(VPORTB.IN & PIN5_bm),
                .off_req_n = (VPORTA.IN & PIN1_bm) != 0,
            };
            previous_ms = now;
            pmu_step(&pmu, now, in);
            apply_outputs(&pmu.out);
            wdt_reset();
        }
        /* Timer and button interrupts continue in idle sleep. */
        sleep_mode();
    }
}
