#ifndef F_CPU
#define F_CPU 1000000UL
#endif

#include <stdbool.h>
#include <stdint.h>

#include <avr/cpufunc.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <util/delay.h>

#define LOOP_TICK_MS              10UL
#define DEBOUNCE_TICKS            4U
#define STARTUP_3V3_DELAY_MS      3000UL
#define USB_BOOT_HOLD_MS          5000UL
#define SHUTDOWN_TIMEOUT_MS       20000UL
#define ON_OUT_PULSE_MS           200UL
#define WATCHDOG_PERIOD           WDT_PERIOD_2KCLK_gc

typedef struct {
    PORT_t *port;
    volatile uint8_t *vport_in;
    uint8_t bitmask;
    bool output;
    bool initial_high;
    bool pullup;
} gpio_pin_t;

typedef struct {
    uint8_t integrator;
    bool state;
} debounce_t;

typedef enum {
    DEV_OFF = 0,
    DEV_STARTUP_WAIT_3V3,
    DEV_RUNNING,
    DEV_SHUTDOWN_WAIT,
} dev_state_t;

static const gpio_pin_t PIN_ON_SWITCH = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN2_bm,
    .output = false, .initial_high = false, .pullup = true,
};

static const gpio_pin_t PIN_OFF_REQ_N = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN1_bm,
    .output = false, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_SIDE_BTN = {
    .port = &PORTB, .vport_in = &VPORTB.IN, .bitmask = PIN5_bm,
    .output = false, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_USB_BOOT = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN3_bm,
    .output = false, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_5V_ON_REQ = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN6_bm,
    .output = true, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_3V3_ON_REQ = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN7_bm,
    .output = true, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_5V_ON_OUT_N = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN5_bm,
    .output = false, .initial_high = false, .pullup = false,
};

static const gpio_pin_t PIN_SOM_RST = {
    .port = &PORTA, .vport_in = &VPORTA.IN, .bitmask = PIN4_bm,
    .output = false, .initial_high = false, .pullup = false,
};

static volatile uint8_t *pin_ctrl_reg(const gpio_pin_t *pin)
{
    switch (pin->bitmask) {
    case PIN0_bm: return &pin->port->PIN0CTRL;
    case PIN1_bm: return &pin->port->PIN1CTRL;
    case PIN2_bm: return &pin->port->PIN2CTRL;
    case PIN3_bm: return &pin->port->PIN3CTRL;
    case PIN4_bm: return &pin->port->PIN4CTRL;
    case PIN5_bm: return &pin->port->PIN5CTRL;
    case PIN6_bm: return &pin->port->PIN6CTRL;
    default:      return &pin->port->PIN7CTRL;
    }
}

static void set_pin_pullup(const gpio_pin_t *pin, bool enabled)
{
    volatile uint8_t *ctrl = pin_ctrl_reg(pin);

    if (enabled) {
        *ctrl = (*ctrl & ~PORT_PULLUPEN_bm) | PORT_PULLUPEN_bm;
    } else {
        *ctrl &= (uint8_t)~PORT_PULLUPEN_bm;
    }
}

static void init_pin(const gpio_pin_t *pin)
{
    set_pin_pullup(pin, pin->pullup);

    if (pin->output) {
        if (pin->initial_high) {
            pin->port->OUTSET = pin->bitmask;
        } else {
            pin->port->OUTCLR = pin->bitmask;
        }
        pin->port->DIRSET = pin->bitmask;
    } else {
        pin->port->DIRCLR = pin->bitmask;
    }
}

static inline bool read_pin(const gpio_pin_t *pin)
{
    return ((*pin->vport_in) & pin->bitmask) != 0U;
}

static inline void drive_high(const gpio_pin_t *pin)
{
    pin->port->OUTSET = pin->bitmask;
    pin->port->DIRSET = pin->bitmask;
}

static inline void drive_low(const gpio_pin_t *pin)
{
    pin->port->OUTCLR = pin->bitmask;
    pin->port->DIRSET = pin->bitmask;
}

static inline void float_input(const gpio_pin_t *pin)
{
    pin->port->DIRCLR = pin->bitmask;
}

static inline bool debounce_update(debounce_t *db, bool sample)
{
    bool changed = false;

    if (sample) {
        if (db->integrator < DEBOUNCE_TICKS) {
            db->integrator++;
        }
    } else {
        if (db->integrator > 0U) {
            db->integrator--;
        }
    }

    if ((db->integrator == 0U) && db->state) {
        db->state = false;
        changed = true;
    } else if ((db->integrator >= DEBOUNCE_TICKS) && !db->state) {
        db->state = true;
        changed = true;
    }

    return changed;
}

static inline bool timer_expired(uint32_t now_ms, uint32_t deadline_ms)
{
    return ((int32_t)(now_ms - deadline_ms)) >= 0;
}

static inline void enable_5v(bool on)
{
    if (on) {
        drive_high(&PIN_5V_ON_REQ);
    } else {
        drive_low(&PIN_5V_ON_REQ);
    }
}

static inline void enable_3v3(bool on)
{
    if (on) {
        drive_high(&PIN_3V3_ON_REQ);
    } else {
        drive_low(&PIN_3V3_ON_REQ);
    }
}

static inline void usb_boot_assert(bool active)
{
    if (active) {
        drive_high(&PIN_USB_BOOT);
    } else {
        float_input(&PIN_USB_BOOT);
    }
}

static inline void on_out_assert(bool active)
{
    if (active) {
        drive_low(&PIN_5V_ON_OUT_N);
    } else {
        float_input(&PIN_5V_ON_OUT_N);
    }
}

static void all_outputs_safe_off(void)
{
    enable_3v3(false);
    enable_5v(false);
    usb_boot_assert(false);
    on_out_assert(false);
}

static inline bool on_switch_is_high(void)
{
    return read_pin(&PIN_ON_SWITCH);
}

static inline bool side_btn_pressed(void)
{
    return !read_pin(&PIN_SIDE_BTN);
}

static void watchdog_enable_runtime(void)
{
    CCP = CCP_IOREG_gc;
    WDT.CTRLA = WATCHDOG_PERIOD;
}

static void clock_init_1mhz(void)
{
    CCP = CCP_IOREG_gc;
    CLKCTRL.MCLKCTRLB = CLKCTRL_PDIV_16X_gc | CLKCTRL_PEN_bm;
}

int main(void)
{
    uint32_t now_ms = 0UL;
    uint32_t startup_3v3_deadline = 0UL;
    uint32_t usb_boot_release_deadline = 0UL;
    uint32_t shutdown_deadline = 0UL;
    uint32_t on_out_release_deadline = 0UL;

    bool usb_boot_latched = false;
    bool on_out_asserted_flag = false;
    bool on_switch_fall_armed;
    bool off_req_seen_high = false;

    debounce_t on_sw_db;
    debounce_t side_btn_db;
    debounce_t off_req_db;
    dev_state_t state = DEV_OFF;

    init_pin(&PIN_ON_SWITCH);
    init_pin(&PIN_OFF_REQ_N);
    init_pin(&PIN_SIDE_BTN);
    init_pin(&PIN_USB_BOOT);
    init_pin(&PIN_5V_ON_REQ);
    init_pin(&PIN_3V3_ON_REQ);
    init_pin(&PIN_5V_ON_OUT_N);
    init_pin(&PIN_SOM_RST);

    all_outputs_safe_off();

    on_sw_db.state = on_switch_is_high();
    on_sw_db.integrator = on_sw_db.state ? DEBOUNCE_TICKS : 0U;
    side_btn_db.state = read_pin(&PIN_SIDE_BTN);
    side_btn_db.integrator = side_btn_db.state ? DEBOUNCE_TICKS : 0U;
    off_req_db.state = read_pin(&PIN_OFF_REQ_N);
    off_req_db.integrator = off_req_db.state ? DEBOUNCE_TICKS : 0U;

    on_switch_fall_armed = on_sw_db.state;

    clock_init_1mhz();
    watchdog_enable_runtime();

    while (1) {
        bool on_sw_changed;
        bool side_changed;
        bool off_req_changed;
        bool on_sw_rising = false;
        bool on_sw_falling = false;
        bool off_req_falling = false;

        _delay_ms(LOOP_TICK_MS);
        now_ms += LOOP_TICK_MS;

        on_sw_changed = debounce_update(&on_sw_db, on_switch_is_high());
        side_changed = debounce_update(&side_btn_db, read_pin(&PIN_SIDE_BTN));
        off_req_changed = debounce_update(&off_req_db, read_pin(&PIN_OFF_REQ_N));
        (void)side_changed;

        if (on_sw_changed) {
            on_sw_rising = on_sw_db.state;
            on_sw_falling = !on_sw_db.state;
        }

        if (off_req_changed) {
            off_req_falling = !off_req_db.state;
        }

        switch (state) {
        case DEV_OFF:
            set_pin_pullup(&PIN_SIDE_BTN, true);
            usb_boot_assert(false);
            on_out_assert(false);
            usb_boot_latched = false;
            on_out_asserted_flag = false;
            off_req_seen_high = false;

            if (on_sw_db.state) {
                on_switch_fall_armed = true;
            }

            if (on_sw_falling && on_switch_fall_armed) {
                const bool usb_boot_request = !side_btn_db.state || side_btn_pressed();

                on_switch_fall_armed = false;
                enable_5v(true);
                enable_3v3(false);

                if (usb_boot_request) {
                    usb_boot_assert(true);
                    usb_boot_latched = true;
                    usb_boot_release_deadline = now_ms + USB_BOOT_HOLD_MS;
                }

                startup_3v3_deadline = now_ms + STARTUP_3V3_DELAY_MS;
                state = DEV_STARTUP_WAIT_3V3;
            }
            break;

        case DEV_STARTUP_WAIT_3V3:
            set_pin_pullup(&PIN_SIDE_BTN, false);
            if (usb_boot_latched && timer_expired(now_ms, usb_boot_release_deadline)) {
                usb_boot_assert(false);
                usb_boot_latched = false;
            }

            if (on_sw_rising) {
                all_outputs_safe_off();
                state = DEV_OFF;
                on_switch_fall_armed = true;
                break;
            }

            if (timer_expired(now_ms, startup_3v3_deadline)) {
                enable_3v3(true);
                state = DEV_RUNNING;
            }
            break;

        case DEV_RUNNING:
            set_pin_pullup(&PIN_SIDE_BTN, false);
            if (off_req_db.state) {
                off_req_seen_high = true;
            }
            if (usb_boot_latched && timer_expired(now_ms, usb_boot_release_deadline)) {
                usb_boot_assert(false);
                usb_boot_latched = false;
            }

            if (off_req_seen_high && off_req_falling) {
                all_outputs_safe_off();
                state = DEV_OFF;
                on_switch_fall_armed = on_sw_db.state;
                usb_boot_latched = false;
                on_out_asserted_flag = false;
                break;
            }

            if (on_sw_rising) {
                on_out_assert(true);
                on_out_asserted_flag = true;
                on_out_release_deadline = now_ms + ON_OUT_PULSE_MS;
                shutdown_deadline = now_ms + SHUTDOWN_TIMEOUT_MS;
                state = DEV_SHUTDOWN_WAIT;
            }
            break;

        case DEV_SHUTDOWN_WAIT:
            set_pin_pullup(&PIN_SIDE_BTN, false);
            if (off_req_db.state) {
                off_req_seen_high = true;
            }
            if (on_out_asserted_flag && timer_expired(now_ms, on_out_release_deadline)) {
                on_out_assert(false);
                on_out_asserted_flag = false;
            }

            if (off_req_seen_high && off_req_falling) {
                all_outputs_safe_off();
                state = DEV_OFF;
                on_switch_fall_armed = on_sw_db.state;
                usb_boot_latched = false;
                on_out_asserted_flag = false;
                break;
            }

            if (timer_expired(now_ms, shutdown_deadline)) {
                all_outputs_safe_off();
                state = DEV_OFF;
                on_switch_fall_armed = on_sw_db.state;
                usb_boot_latched = false;
                on_out_asserted_flag = false;
            }
            break;

        default:
            all_outputs_safe_off();
            state = DEV_OFF;
            on_switch_fall_armed = on_sw_db.state;
            usb_boot_latched = false;
            on_out_asserted_flag = false;
            break;
        }

        wdt_reset();
    }
}
