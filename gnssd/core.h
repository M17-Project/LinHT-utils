// linht-gnssd: receiver state, commands and the NMEA watchdog. No D-Bus
// and no file descriptors here: the daemon passes the I/O in core_ops, the
// unit tests pass a fake receiver.

#ifndef GNSSD_CORE_H
#define GNSSD_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "config.h"
#include "nmea.h"

#define CORE_RETRY_S     5.0    // tty open retry
#define CORE_ACK_S       2.0    // PMTK001 wait
#define CORE_WAKE_S      3.0    // NMEA wait after Wake
#define CORE_COLLECT_S   1.5    // replies collected by SendCommand
#define CORE_COLLECT_MAX 8
#define CORE_STANDBY_S   2.0    // NMEA this long after standby wakes the state

struct core_ops
{
    int (*open)(void *ctx);                         // 0 or -errno, -EWOULDBLOCK = locked
    void (*close)(void *ctx);
    int (*write)(void *ctx, const char *buf, size_t n);
    int (*reset)(void *ctx);                        // RESET_N pulse, -ENOTSUP without a line
    void (*forward)(void *ctx, const char *line, size_t n);  // valid sentence with CR LF

    // System clock and RTC, NULL = no time sync. The tests pass fakes.
    int (*realtime)(void *ctx, struct timespec *ts);            // CLOCK_REALTIME
    int (*set_realtime)(void *ctx, const struct timespec *ts);  // 0 or -errno
    // Writes the system time minus `offset` to the RTC at its next whole
    // second and returns that second in *written. 0 or -errno.
    int (*rtc_write)(void *ctx, double offset, time_t *written);
    int (*ntp_synced)(void *ctx);                               // 1 = NTP keeps the clock
};

#define TS_SAMPLES       5      // RMC epochs in a row that must agree
#define TS_AGREE_S       0.2    // GNSS time minus monotonic time, allowed change
#define TS_MIN_SATS      4
#define TS_RTC_PERIOD_S  3600.0 // RTC write without a step at most this often
#define TS_RETRY_S       600.0  // after a failed step
#define TS_WARN_PERIOD_S 3600.0 // GNSS against NTP warning

struct timesync
{
    int n;                          // valid samples in a row that agree
    double offs[TS_SAMPLES];        // system clock minus GNSS, ring
    int head;
    double base;                    // GNSS time minus monotonic time, last sample
    double last_sample;             // monotonic, 0 = never
    double offset;                  // median of the last TS_SAMPLES, NaN = none
    double last_rtc;                // monotonic, -1 = never
    double next_step;               // no automatic step before this (after a failure)
    double last_warn;               // -1 = never
    unsigned steps, rtc_writes;
    char last_sync[32];             // UTC of the last step or RTC write, empty = never
};

enum core_health { H_STARTING, H_OK, H_NO_DATA, H_TTY_BUSY, H_TTY_ERROR, H_STANDBY };
enum core_power { P_ON, P_STANDBY };
enum core_pend { PEND_NONE, PEND_ACK, PEND_DATA, PEND_COLLECT };

struct core
{
    struct gnssd_config cfg;
    const struct core_ops *ops;
    void *ctx;
    struct nmea_state st;

    char line[NMEA_MAX + 3];
    size_t len;
    int skipping;                   // line too long, wait for LF

    int open;
    double link_since;              // tty opened or the module restarted
    double last_rx;                 // last valid sentence, 0 = never
    double next_open;
    int open_err;
    enum core_health health;
    enum core_power power;
    double standby_at;
    unsigned reopens, resets, stalls;
    double last_reset;
    unsigned startup_seen;

    struct
    {
        enum core_pend kind;
        int cmd;                    // PMTK packet type for PEND_ACK
        unsigned ack_seq;
        double deadline;
        int done;                   // 1: result is set
        int result;                 // 0 or -errno
        int n;
        char lines[CORE_COLLECT_MAX][NMEA_MAX + 3];
    } pend;

    double last_utc_s;              // RMC time of day, -1 = none
    uint32_t interval_ms;           // between RMC times, 0 = unknown
    uint32_t last_step_ms;          // last RMC time step

    struct gnss_snap snap;
    int fix_prev, valid_prev;
    double last_pos;
    int fix_event, pos_event;       // set by core_update, cleared by the D-Bus layer

    struct timesync ts;
};

extern int core_log_quiet;
void core_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
double core_now(void);

void core_init(struct core *c, const struct core_ops *ops, void *ctx);

// Bytes from the tty.
void core_feed(struct core *c, const char *data, size_t n, double now);
// Read error or hangup: the tty is closed and opened again by core_tick.
void core_link_lost(struct core *c, double now, int err);
// Opens the tty, runs the watchdog and the command timeouts. Call it at
// least twice a second.
void core_tick(struct core *c, double now);
// Refreshes the snapshot and sets fix_event and pos_event.
void core_update(struct core *c, double now);

// Commands. 0 when sent, -EBUSY while another command waits, -EINVAL for a
// bad argument, -ENOTSUP without a reset line, -ENOTCONN when the tty is
// not open or the write fails. PEND_* commands finish later with
// pend.done and pend.result: 0, -ETIMEDOUT, or from the PMTK001 flag
// -EBADMSG (0, invalid packet), -EPROTONOSUPPORT (1, unsupported packet),
// -EIO (2, action failed).
int core_standby(struct core *c, double now);           // PMTK161,0, waits for the ACK
int core_wake(struct core *c, double now);              // any byte, waits for NMEA
int core_restart(struct core *c, const char *mode, double now);  // hot warm cold full-cold reset
int core_set_rate(struct core *c, unsigned ms, double now);      // PMTK220, waits for the ACK
int core_raw(struct core *c, const char *cmd, double now);       // collects $P replies
void core_pend_clear(struct core *c);

double core_stale(const struct core *c);

// --- Time sync (timesync.c) ---

// Seconds since 1970 from RMC ddmmyy and hhmmss.sss, -1 when malformed.
// *year gets the four digit year.
double ts_gnss_time(const char *date, const char *time, int *year);
// Year of the build, from __DATE__. Earlier GNSS dates are rejected (a
// receiver without time counts from 1980, a GPS week rollover goes back
// 19.6 years).
int ts_build_year(void);
void ts_init(struct timesync *t);
// Called for each RMC sentence (`len` without CR LF), after the snapshot of
// the previous epoch.
void ts_rmc(struct core *c, size_t len, double now);
// 1 when TS_SAMPLES valid epochs in a row agree and the last one is fresh.
int ts_stable(const struct core *c, double now);
// SyncTimeNow: does at once what system_clock and rtc allow, without the
// threshold and the hourly RTC period. Steps the clock (system_clock auto
// or always, in auto not against NTP) and writes the RTC (rtc = on).
// 0 with *stepped and *rtc_written, -EPERM with system_clock = off and
// rtc = off, -ENODATA without a stable fix, -EALREADY when NTP keeps the
// clock (not in always mode) and it differs from GNSS by more than the
// threshold, or the -errno of the step.
int ts_sync_now(struct core *c, double now, int *stepped, int *rtc_written);
const char *core_health_name(enum core_health h);
const char *core_power_name(enum core_power p);

#endif
