#define F_CPU ((uint32_t)(20e6 / 48) + 1)

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#define I2C_ADDR 0x73

typedef enum
{
    DEV_OFF,
    DEV_ON
} dev_state_t;

typedef enum
{
    PIN_INPUT,
    PIN_OUTPUT
} pin_dir_t;

enum
{
    _5V_OFF_REQ,
    _5V_ON_OUT,
    USB_BOOT,
    _5V_ON_REQ, 
    _5V0_ON_REQ,
    _3V3_ON_REQ,
    SIDE_BTN
};

typedef struct gpio_pin
{
    PORT_t *port;    // port address
    uint8_t pin;     // pin value
    pin_dir_t dir;   // direction
    uint8_t def;     // default value
} gpio_pin_t;

gpio_pin_t gpio[7] = 
{
    {&PORTA, 1, PIN_INPUT,  0},    // PMIC_STBY_REQ
    {&PORTA, 2, PIN_OUTPUT, 1},    // ON/OFF SoM, active low
    {&PORTA, 3, PIN_OUTPUT, 0},    // enable USB bootloader
    {&PORTA, 5, PIN_INPUT,  0},    // ON/OFF switch
    {&PORTA, 6, PIN_OUTPUT, 0},    // 5V DC/DC enable signal
    {&PORTA, 7, PIN_OUTPUT, 0},    // 3V3 DC/DC enable signal
    {&PORTB, 5, PIN_INPUT,  0}     // Side button (below PTT)
};

volatile uint8_t rep_cnt;
volatile uint8_t rxb;
uint8_t txb[4] = {0xDE, 0xAD, 0xBE, 0xEF};
dev_state_t dev_state = DEV_OFF;

void set_direction(gpio_pin_t gpio)
{
    uint8_t mask = (uint8_t)1 << gpio.pin;

    if (gpio.dir == PIN_OUTPUT)
        gpio.port->DIR |= mask;
    else
        gpio.port->DIR &= (uint8_t)~mask;
}

void set_value(gpio_pin_t gpio, uint8_t value)
{
    uint8_t mask = (uint8_t)1 << gpio.pin;

    if (value)
        gpio.port->OUT |= mask;
    else
        gpio.port->OUT &= (uint8_t)~mask;
}

uint8_t get_value(gpio_pin_t gpio)
{
    return (gpio.port->IN & ((uint8_t)1 << gpio.pin)) >> gpio.pin;
}

void init_pin(gpio_pin_t gpio)
{
    set_direction(gpio);
    set_value(gpio, gpio.def);
}

// configs
void config_clk(void)
{
    CCP = CCP_IOREG_gc;
    CLKCTRL.MCLKCTRLB = CLKCTRL_PDIV_48X_gc | CLKCTRL_PEN_bm;
}

void config_gpios(void)
{
    PORTA.DIR = PORTA.OUT = 0;
    PORTB.DIR = PORTB.OUT = 0;

    for(uint8_t i=0; i<7; i++)
        init_pin(gpio[i]);
}

void config_i2c(void)
{
    // set slave address
    TWI0.SADDR = (I2C_ADDR << 1);

    // enable address and data interrupts
    TWI0.SCTRLA = TWI_DIEN_bm | TWI_APIEN_bm | TWI_ENABLE_bm;

    // clear status flags
    TWI0.SSTATUS = TWI_APIF_bm | TWI_DIF_bm | TWI_COLL_bm | TWI_BUSERR_bm;
}

void init_device(void)
{
    config_clk();
    config_gpios();
    config_i2c();
    sei();
}

// interrupt handlers
ISR(TWI0_TWIS_vect)
{
    uint8_t status = TWI0.SSTATUS;

    // address match (read or write)
    if (status & TWI_APIF_bm)
    {
        if (status & TWI_AP_bm)
        {
            // master wants to READ (slave transmit)
            rep_cnt = 0;
        }
        // clear flag
        TWI0.SSTATUS |= TWI_APIF_bm;
    }

    // data stage
    if (status & TWI_DIF_bm)
    {
        if (status & TWI_DIR_bm)
        {
            // master is reading, send next byte
            if (rep_cnt < sizeof(txb))
            {
                TWI0.SDATA = txb[rep_cnt++];
                TWI0.SCTRLB = TWI_SCMD_RESPONSE_gc; // send data + ACK
            }
            else
            {
                // no more data, send NACK
                TWI0.SCTRLB = TWI_ACKACT_NACK_gc | TWI_SCMD_COMPTRANS_gc;
            }
        }
        else
        {
            // master writing, receive byte
            rxb = TWI0.SDATA;
            TWI0.SCTRLB = TWI_ACKACT_ACK_gc | TWI_SCMD_RESPONSE_gc;
        }

        // clear DIF
        TWI0.SSTATUS |= TWI_DIF_bm;
    }

    // stop or collision
    if (status & (TWI_COLL_bm | TWI_BUSERR_bm))
    {
        TWI0.SSTATUS |= (TWI_COLL_bm | TWI_BUSERR_bm);
    }
}

int main(void)
{
    init_device();

    while (1)
    {
        // read the ON/OFF knob's position - "ON" position
        if(dev_state == DEV_OFF && get_value(gpio[_5V_ON_REQ]) == 0)
        {
            set_value(gpio[_5V0_ON_REQ], 1);
            _delay_ms(5000); //TODO: replace with a timer interrupt call?
            set_value(gpio[_3V3_ON_REQ], 1);
            dev_state = DEV_ON;
        }

        // read the ON/OFF knob's position - "OFF" position
        if (dev_state == DEV_ON && get_value(gpio[_5V_ON_REQ]) == 1)
        {
            set_value(gpio[_5V_ON_OUT], 0);
            _delay_ms(5000); //TODO: replace with a timer interrupt call?
            set_value(gpio[_5V_ON_OUT], 1);
            dev_state = DEV_OFF;
        }

        // react to a positive pulse at PMIC_STBY_REQ
        // TODO: check if it is indeed a positive pulse
        // and if this is a valid shutdown procedure
        if(dev_state == DEV_ON && get_value(gpio[_5V_OFF_REQ]) == 1)
        {
            set_value(gpio[_5V0_ON_REQ], 0);
            set_value(gpio[_3V3_ON_REQ], 0);
            dev_state = DEV_OFF;
        }

        // simple inversion of the SIDE_BTN signal
        set_value(gpio[SIDE_BTN], !get_value(gpio[USB_BOOT]));

        // more logic, as required
        ;
    }

    return 0;
}
