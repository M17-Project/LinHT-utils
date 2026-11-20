// linht-chargerd: configuration file /etc/linht/chargerd.conf

#ifndef CHARGERD_CONFIG_H
#define CHARGERD_CONFIG_H

#define CHARGERD_CONFIG_PATH "/etc/linht/chargerd.conf"

#define CONFIG_VCELL_MIN 4.00       // V per cell, allowed charge voltage
#define CONFIG_VCELL_MAX 4.20

struct chargerd_config
{
    char i2c_dev[64];               // /dev/i2c-2
    int i2c_addr;                   // 0x6b
    char state_dir[128];            // /var/lib/linht-chargerd

    int pfm;                        // 1 = PFM on in forward mode, 0 = forced PWM
    int full_hold;                  // 1 = HIZ while full on USB
    double full_hold_resume_soc;    // %

    unsigned quiet_max_s;           // longest lease
    double quiet_min_soc;           // % new leases refused below (on USB)
    double quiet_end_soc;           // % all leases end below (on USB)
    double quiet_resume_soc;        // % leases allowed again after a low battery end
    double quiet_min_vbat;          // V, used while the SoC is not trusted,
    double quiet_end_vbat;          //   NaN (auto): from the voltage table

    double low_soc;                 // % LowBattery("low")
    double critical_soc;            // % LowBattery("critical"), run time reference

    char battery[64];               // free text, for the log
    int cells;                      // series cells
    double charge_voltage_v;        // VREG written to the charger, V per pack
    double capacity_rated_wh;
    double capacity_rated_mah;
};

void config_defaults(struct chargerd_config *c);

// Reads key = value lines. A missing file keeps the defaults and returns 1.
// Returns 0 on success, -1 on a syntax error, an unknown key or a charge
// voltage outside CONFIG_VCELL_MIN to CONFIG_VCELL_MAX per cell (all lines
// are still read, the error is printed with the line number). A double
// may be "auto", which is NaN.
int config_load(struct chargerd_config *c, const char *path);

#endif
