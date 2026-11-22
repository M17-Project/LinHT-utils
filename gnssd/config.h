// linht-gnssd: configuration file /etc/linht/gnssd.conf

#ifndef GNSSD_CONFIG_H
#define GNSSD_CONFIG_H

#define GNSSD_CONFIG_PATH "/etc/linht/gnssd.conf"

struct gnssd_config
{
    char device[64];                // /dev/ttyLP2
    unsigned baud;                  // 9600

    double stale_s;                 // sentences older than this are not used
    double watchdog_s;              // no NMEA this long: reopen the tty
    int watchdog_reset;             // 1 = RESET_N pulse after reset_after_reopens
    unsigned reset_after_reopens;
    double reset_min_interval_s;    // at most one watchdog reset per this time

    char reset_chip[32];            // /dev/gpiochip2, empty = no reset line
    int reset_line;                 // GPIO4_IO03 = 3 on gpiochip2, -1 = none
    unsigned reset_ms;              // RESET_N low time, at least 100 ms

    double position_interval_s;     // PositionChanged at most this often

    char pty_link[96];              // NMEA pass-through pty, empty = off
    char socket_path[96];           // NMEA pass-through socket, empty = off
    char feed_group[32];            // group of the pty and the socket

    int send_command;               // 1 = SendCommand allowed (root only)

    int system_clock;               // enum system_clock_mode
    int rtc;                        // 1 = write rtc_device from GNSS
    double time_sync_threshold_s;   // step the system clock above this offset
    double time_sync_delay_ms;      // receiver output delay, start of the second to the first RMC byte
    char rtc_device[32];            // battery-backed RTC, empty = no RTC writes
};

enum system_clock_mode { SC_AUTO, SC_OFF, SC_ALWAYS };
const char *system_clock_mode_name(int mode);

void config_defaults(struct gnssd_config *c);

// Reads key = value lines. A missing file keeps the defaults and returns 1.
// Returns 0 on success, -1 on a syntax error or an unknown key (all lines
// are still read, the error is printed with the line number).
int config_load(struct gnssd_config *c, const char *path);

#endif
