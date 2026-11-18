// linht-chargerd: configuration file /etc/linht/chargerd.conf

#ifndef CHARGERD_CONFIG_H
#define CHARGERD_CONFIG_H

#define CHARGERD_CONFIG_PATH "/etc/linht/chargerd.conf"

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
    double quiet_min_vbat;          // V, used while the SoC is not trusted
    double quiet_end_vbat;          // V

    double low_soc;                 // % LowBattery("low")
    double critical_soc;            // % LowBattery("critical"), run time reference

    char battery[64];               // free text, for the log
    int cells;                      // series cells
    double capacity_rated_wh;
    double capacity_rated_mah;
};

void config_defaults(struct chargerd_config *c);

// Reads key = value lines. A missing file keeps the defaults and returns 1.
// Returns 0 on success, -1 on a syntax error or an unknown key (all lines
// are still read, the error is printed with the line number).
int config_load(struct chargerd_config *c, const char *path);

#endif
