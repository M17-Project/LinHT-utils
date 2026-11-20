// linht-gnssd: the receiver UART and the RESET_N line

#ifndef GNSSD_TTY_H
#define GNSSD_TTY_H

// Opens the tty raw 8N1 at `baud`, non-blocking, with flock(LOCK_EX) like
// gnss_status.py and the FART field logger. Returns the fd or -errno,
// -EWOULDBLOCK when another reader holds the lock.
int tty_open(const char *path, unsigned baud);

// Drives the line low for `ms` milliseconds, then makes it an input again
// (RESET_N has its own pull-up in the module). Blocks for `ms`. Returns 0
// or -errno, -ENOTSUP when no line is configured.
int gpio_pulse_low(const char *chip, int line, unsigned ms);

#endif
