// linht-gnssd: the receiver UART and the RESET_N line

#ifndef GNSSD_TTY_H
#define GNSSD_TTY_H

#include <time.h>

// Opens the tty raw 8N1 at `baud`, non-blocking, with flock(LOCK_EX) like
// gnss_status.py and the FART field logger. Returns the fd or -errno,
// -EWOULDBLOCK when another reader holds the lock.
int tty_open(const char *path, unsigned baud);

// Drives the line low for `ms` milliseconds, then makes it an input again
// (RESET_N has its own pull-up in the module). Blocks for `ms`. Returns 0
// or -errno, -ENOTSUP when no line is configured.
int gpio_pulse_low(const char *chip, int line, unsigned ms);

// Sets the RTC (RTC_SET_TIME) to the system time minus `offset` seconds. It
// waits for the next whole second of that time first (at most 1 s), because
// the RTC counts whole seconds and its driver (rtc-abx80x) clears the
// hundredths on a write. Returns 0 with the second in *written, or -errno.
int rtc_set_from_system(const char *dev, double offset, time_t *written);

// The test of timedatectl NTPSynchronized (systemd ntp_synced()): adjtimex
// reports a maximum error below 16 s. clock_settime sets it back to 16 s.
int clock_ntp_synced(void);

#endif
