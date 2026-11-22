// linht-gnssd: system clock and battery-backed RTC from GNSS
//
// Without the 1PPS line the time comes from the NMEA second boundary. The
// RMC time is the start of the epoch, the sentence arrives later: the
// receiver output delay (time_sync_delay_ms) and the time on the wire are
// added. The tty read time is taken when the line is complete.

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"

int ts_build_year(void)
{
    // __DATE__ is "Mmm dd yyyy".
    return atoi(__DATE__ + 7);
}

static int digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + s[i] - '0';
    }
    return v;
}

static int days_from_civil(int y, int m, int d)
{
    // Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant).
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double ts_gnss_time(const char *date, const char *time, int *year)
{
    if (strlen(date) != 6 || strlen(time) < 6)
        return -1;
    int dd = digits(date, 2), mm = digits(date + 2, 2), yy = digits(date + 4, 2);
    int h = digits(time, 2), mi = digits(time + 2, 2), sec = digits(time + 4, 2);
    if (dd < 1 || dd > 31 || mm < 1 || mm > 12 || yy < 0 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || sec < 0 || sec > 60)
        return -1;
    double frac = 0;
    if (time[6] == '.')
    {
        char *end;
        frac = strtod(time + 6, &end);
        if (*end || frac < 0 || frac >= 1)
            return -1;
    }
    else if (time[6])
        return -1;
    int y = yy < 80 ? 2000 + yy : 1900 + yy;
    if (year)
        *year = y;
    return days_from_civil(y, mm, dd) * 86400.0 + h * 3600 + mi * 60 + sec + frac;
}

void ts_init(struct timesync *t)
{
    memset(t, 0, sizeof(*t));
    t->offset = NAN;
    t->last_rtc = -1;
    t->last_warn = -1;
}

static double realtime(struct core *c)
{
    struct timespec ts;
    if (c->ops->realtime(c->ctx, &ts) < 0)
        return NAN;
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void utc_string(time_t t, char *out, size_t size)
{
    struct tm tm;
    if (!gmtime_r(&t, &tm) || !strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &tm))
        snprintf(out, size, "?");
}

static int enabled(const struct core *c)
{
    return c->ops->realtime && c->ops->set_realtime;
}

int ts_stable(const struct core *c, double now)
{
    return enabled(c) && c->ts.n >= TS_SAMPLES && now - c->ts.last_sample <= core_stale(c);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(const struct timesync *t)
{
    double v[TS_SAMPLES];
    memcpy(v, t->offs, sizeof(v));
    qsort(v, TS_SAMPLES, sizeof(v[0]), cmp_double);
    return v[TS_SAMPLES / 2];
}

static void rtc_write(struct core *c, double offset, double now, const char *why)
{
    struct timesync *t = &c->ts;
    t->last_rtc = now;
    if (!c->ops->rtc_write || !c->cfg.rtc_device[0])
        return;
    time_t written = 0;
    int r = c->ops->rtc_write(c->ctx, offset, &written);
    if (r < 0)
    {
        core_log(3, "%s: write from GNSS failed: %s", c->cfg.rtc_device, strerror(-r));
        return;
    }
    t->rtc_writes++;
    utc_string(written, t->last_sync, sizeof(t->last_sync));
    core_log(6, "%s set to %s from GNSS (%s)", c->cfg.rtc_device, t->last_sync, why);
}

static int step(struct core *c, double offset)
{
    struct timesync *t = &c->ts;
    double now_rt = realtime(c);
    if (isnan(now_rt))
        return -EIO;
    double target = now_rt - offset;
    struct timespec ts;
    ts.tv_sec = (time_t)floor(target);
    ts.tv_nsec = (long)((target - floor(target)) * 1e9);
    if (ts.tv_nsec >= 1000000000L)
        ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    int r = c->ops->set_realtime(c->ctx, &ts);
    if (r < 0)
        return r;
    char was[32];
    utc_string((time_t)floor(now_rt), was, sizeof(was));
    utc_string(ts.tv_sec, t->last_sync, sizeof(t->last_sync));
    t->steps++;
    core_log(5, "system clock stepped by %+.3f s from GNSS, %s -> %s", -offset, was, t->last_sync);
    // The samples were taken against the old clock.
    t->n = 0;
    return 0;
}

// What to do with a stable offset. `manual` is SyncTimeNow: it skips the
// threshold and the RTC period, but system_clock = off and rtc = off still
// keep the daemon away from the clock and the RTC.
static int act(struct core *c, double now, int manual, int *stepped)
{
    struct timesync *t = &c->ts;
    int mode = c->cfg.system_clock, rtc = c->cfg.rtc;
    double off = t->offset;
    *stepped = 0;

    if (mode == SC_OFF && !rtc)
        return 0;
    // A clock before the build year is not valid (systemd starts it at its
    // own build time when the RTC has nothing). NTP cannot keep such a
    // clock, a claim of sync (ntpd with only the local clock) is ignored.
    double rt = realtime(c);
    int invalid = !isnan(rt) && rt < days_from_civil(ts_build_year(), 1, 1) * 86400.0;
    int ntp = mode != SC_ALWAYS && !invalid && c->ops->ntp_synced && c->ops->ntp_synced(c->ctx) == 1;
    int big = invalid || fabs(off) > c->cfg.time_sync_threshold_s;

    // NTP and GNSS disagree: neither is trusted, nothing is written.
    if (ntp && big)
    {
        if (manual || t->last_warn < 0 || now - t->last_warn >= TS_WARN_PERIOD_S)
        {
            t->last_warn = now;
            core_log(4, "system clock is NTP synchronised and %+.3f s off GNSS, no step and no "
                        "RTC write", off);
        }
        return manual ? -EALREADY : 0;
    }
    if (mode != SC_OFF && !ntp && (big || (manual && fabs(off) >= 0.001)))
    {
        if (!manual && now < t->next_step)
            return 0;
        int r = step(c, off);
        if (r < 0)
        {
            core_log(3, "system clock step by %+.3f s failed: %s", -off, strerror(-r));
            t->next_step = now + TS_RETRY_S;
            return r;
        }
        *stepped = 1;
        if (rtc)
            rtc_write(c, 0, now, "after the step");
        return 0;
    }
    // The RTC gets the GNSS time, also when the system clock stays off it.
    if (rtc && (manual || t->last_rtc < 0 || now - t->last_rtc >= TS_RTC_PERIOD_S))
    {
        char why[64];
        snprintf(why, sizeof(why), "system clock %+.3f s%s", off, ntp ? ", NTP synchronised" : "");
        rtc_write(c, off, now, why);
    }
    return 0;
}

void ts_rmc(struct core *c, size_t len, double now)
{
    struct timesync *t = &c->ts;
    if (!enabled(c))
        return;

    int year = 0;
    double gnss = ts_gnss_time(c->st.rmc_date, c->st.rmc_time, &year);
    // The snapshot is from the previous epoch (GGA and GSA follow the RMC).
    const struct gnss_snap *s = &c->snap;
    if (!c->st.rmc_valid || gnss < 0 || year < ts_build_year() || s->fix != 3 || !s->valid ||
        s->used < TS_MIN_SATS)
    {
        t->n = 0;
        return;
    }
    double rt = realtime(c);
    if (isnan(rt))
        return;

    // The line ends `len` + 2 characters of 10 bits after its first byte.
    double wire = c->cfg.baud ? (len + 2) * 10.0 / c->cfg.baud : 0;
    double arrival = gnss + c->cfg.time_sync_delay_ms / 1000.0 + wire;

    // The receiver time must run with the monotonic clock.
    double base = gnss - now;
    if (t->n > 0 && fabs(base - t->base) > TS_AGREE_S)
        t->n = 0;
    t->base = base;
    t->offs[t->head] = rt - arrival;
    t->head = (t->head + 1) % TS_SAMPLES;
    t->n++;
    t->last_sample = now;
    if (t->n < TS_SAMPLES)
        return;

    t->offset = median(t);
    int stepped;
    act(c, now, 0, &stepped);
}

int ts_sync_now(struct core *c, double now, int *stepped, int *rtc_written)
{
    *stepped = *rtc_written = 0;
    if (c->cfg.system_clock == SC_OFF && !c->cfg.rtc)
        return -EPERM;
    if (!ts_stable(c, now))
        return -ENODATA;
    unsigned writes = c->ts.rtc_writes;
    int r = act(c, now, 1, stepped);
    *rtc_written = c->ts.rtc_writes != writes;
    return r;
}
