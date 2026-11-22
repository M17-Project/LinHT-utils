// linht-gnssd tests: system clock and RTC from GNSS, with a fake clock

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fixtures.h"

static struct
{
    double now;             // monotonic test time
    double real;            // system clock minus monotonic time
    int sets, set_rc;
    int rtc, rtc_rc;
    double rtc_value;       // what the RTC got, as system time minus the offset
    int ntp;
} T;

static int t_open(void *ctx) { (void)ctx; return 0; }
static void t_close(void *ctx) { (void)ctx; }
static int t_write(void *ctx, const char *b, size_t n) { (void)ctx, (void)b, (void)n; return 0; }
static int t_reset(void *ctx) { (void)ctx; return -ENOTSUP; }

static int t_realtime(void *ctx, struct timespec *ts)
{
    (void)ctx;
    double r = T.now + T.real;
    ts->tv_sec = (time_t)floor(r);
    ts->tv_nsec = (long)((r - floor(r)) * 1e9);
    return 0;
}

static int t_set_realtime(void *ctx, const struct timespec *ts)
{
    (void)ctx;
    if (T.set_rc)
        return T.set_rc;
    T.sets++;
    T.real = ts->tv_sec + ts->tv_nsec * 1e-9 - T.now;
    return 0;
}

static int t_rtc_write(void *ctx, double offset, time_t *written)
{
    (void)ctx;
    if (T.rtc_rc)
        return T.rtc_rc;
    T.rtc++;
    T.rtc_value = T.now + T.real - offset;
    *written = (time_t)ceil(T.rtc_value);
    return 0;
}

static int t_ntp(void *ctx)
{
    (void)ctx;
    return T.ntp;
}

static const struct core_ops tops = {
    .open = t_open, .close = t_close, .write = t_write, .reset = t_reset,
    .realtime = t_realtime, .set_realtime = t_set_realtime, .rtc_write = t_rtc_write,
    .ntp_synced = t_ntp,
};

// GNSS time at monotonic 0, a date in the build year.
static double g0;

static void setup(struct core *c, double system_minus_gnss)
{
    memset(&T, 0, sizeof(T));
    memset(c, 0, sizeof(*c));
    config_defaults(&c->cfg);
    c->cfg.time_sync_delay_ms = 0;
    core_init(c, &tops, NULL);
    core_tick(c, 0);
    char d[8];
    snprintf(d, sizeof(d), "1503%02d", ts_build_year() % 100);
    g0 = ts_gnss_time(d, "120000.000", NULL);
    T.real = g0 + system_minus_gnss;
}

static void put(struct core *c, const char *body, double now)
{
    char buf[128];
    nmea_format(buf, sizeof(buf), body);
    core_feed(c, buf, strlen(buf), now);
}

// One epoch: RMC, GGA, GSA as the LG77L sends them. The RMC line arrives
// exactly at `now`, the receiver time is `gnss`.
static void epoch(struct core *c, double gnss, int valid, int fix, int used, double now)
{
    time_t t = (time_t)floor(gnss);
    struct tm tm;
    gmtime_r(&t, &tm);
    char hms[24], date[24], body[128];
    snprintf(hms, sizeof(hms), "%02d%02d%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)lround((gnss - t) * 1000) % 1000);
    snprintf(date, sizeof(date), "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);

    T.now = now;
    snprintf(body, sizeof(body), "GNRMC,%s,%s,3149.333190,N,11706.911552,E,0.00,5.78,%s,,,%s,V", hms,
             valid ? "A" : "V", date, valid ? "A" : "N");
    put(c, body, now);
    snprintf(body, sizeof(body), "GNGGA,%s,3149.333190,N,11706.911552,E,%d,%02d,0.74,53.489,M,-0.337,M,,",
             hms, valid ? 1 : 0, used);
    put(c, body, now);
    static const char *const ids[] = { "22", "29", "26", "25", "03", "32", "31", "16" };
    char *p = body + snprintf(body, sizeof(body), "GNGSA,A,%d", valid ? fix : 1);
    for (int i = 0; i < 12; i++)
        p += sprintf(p, ",%s", valid && i < used && i < 8 ? ids[i] : "");
    sprintf(p, ",1.35,0.74,1.13,1");
    put(c, body, now);
    core_tick(c, now);
    core_update(c, now);
}

// Epochs every second from t0 to t1, the receiver at g0 + t + skew.
static void run(struct core *c, double t0, double t1, int valid, int fix, int used, double skew)
{
    for (double t = t0; t <= t1 + 1e-9; t += 1)
        epoch(c, g0 + t + skew, valid, fix, used, t);
}

// The RMC above is 74 characters and CR LF, 9600 baud: 79 ms on the wire.
#define WIRE (76 * 10 / 9600.0)

static void test_gnss_time(void)
{
    int y;
    CHECK(ts_gnss_time("211126", "084814.250", &y) == 1795250894.25 && y == 2026);
    CHECK(ts_gnss_time("060180", "000000.000", &y) == 315964800.0 && y == 1980);
    CHECK(ts_gnss_time("010100", "000000", &y) == 946684800.0 && y == 2000);
    CHECK(ts_gnss_time("", "084814.000", &y) < 0);
    CHECK(ts_gnss_time("211126", "", &y) < 0);
    CHECK(ts_gnss_time("321126", "084814.000", &y) < 0);
    CHECK(ts_gnss_time("211126", "0848x4.000", &y) < 0);
    CHECK(ts_gnss_time("211126", "084814.2a", &y) < 0);
    CHECK(ts_build_year() >= 2026);
}

static void test_no_fix(void)
{
    struct core c;
    int stepped, wrote;

    // Rev C indoors: RMC V with a date from the receiver RTC.
    setup(&c, 100);
    for (double t = 1; t <= 30; t++)
    {
        T.now = t;
        for (const char *const *l = fx_nofix; *l; l++)
        {
            core_feed(&c, *l, strlen(*l), t);
            core_feed(&c, "\r\n", 2, t);
        }
        core_tick(&c, t);
        core_update(&c, t);
    }
    CHECK(T.sets == 0 && T.rtc == 0 && isnan(c.ts.offset));
    CHECK(ts_sync_now(&c, 30, &stepped, &wrote) == -ENODATA && !stepped && T.sets == 0);

    // RMC A, but only a 2D fix, or a 3D fix with 3 satellites.
    setup(&c, 100);
    run(&c, 1, 30, 1, 2, 8, 0);
    CHECK(c.snap.fix == 2 && T.sets == 0 && T.rtc == 0);
    setup(&c, 100);
    run(&c, 1, 30, 1, 3, 3, 0);
    CHECK(c.snap.fix == 3 && T.sets == 0 && T.rtc == 0);
    CHECK(ts_sync_now(&c, 30, &stepped, &wrote) == -ENODATA);
}

static void test_bad_dates(void)
{
    struct core c;
    int stepped, wrote;

    // A receiver that reports 1980 (GPS epoch, no time yet) with status A
    // and a 3D fix: never a step, the system clock would go back 46 years.
    setup(&c, 0);
    double back = g0 - 315964800.0;
    run(&c, 1, 30, 1, 3, 8, -back);
    CHECK(!strncmp(c.snap.utc, "1980-", 5));
    CHECK(T.sets == 0 && T.rtc == 0 && ts_sync_now(&c, 30, &stepped, &wrote) == -ENODATA);

    // GPS week rollover: 1024 weeks back.
    setup(&c, 0);
    run(&c, 1, 30, 1, 3, 8, -1024 * 7 * 86400.0);
    CHECK(T.sets == 0 && T.rtc == 0);

    // The 2021 examples of the specification are before the build year.
    setup(&c, 0);
    for (double t = 1; t <= 10; t++)
    {
        T.now = t;
        for (const char *const *l = fx_fix3d; *l; l++)
        {
            core_feed(&c, *l, strlen(*l), t);
            core_feed(&c, "\r\n", 2, t);
        }
        core_tick(&c, t);
        core_update(&c, t);
    }
    CHECK(c.snap.fix == 3 && T.sets == 0);

    // A receiver time that does not run with the clock never gets stable.
    setup(&c, 3600);
    for (double t = 1; t <= 30; t++)
        epoch(&c, g0 + t * 3, 1, 3, 8, t);
    CHECK(T.sets == 0 && T.rtc == 0);
}

static void test_step(void)
{
    struct core c;
    int stepped, wrote;

    // System clock one hour behind: no step before 5 agreeing epochs.
    setup(&c, -3600);
    run(&c, 1, 1, 1, 3, 8, 0);    // the first RMC still sees the old snapshot
    run(&c, 2, 5, 1, 3, 8, 0);
    CHECK(T.sets == 0 && c.ts.n == 4);
    run(&c, 6, 6, 1, 3, 8, 0);
    CHECK(T.sets == 1 && T.rtc == 1);
    CHECK_NEAR(c.ts.offset, -3600 - WIRE, 0.01);
    // The clock now follows GNSS: the RMC that starts a second arrived WIRE
    // later than the second.
    CHECK_NEAR(T.real, g0 + WIRE, 0.01);
    CHECK_NEAR(T.rtc_value, g0 + 6 + WIRE, 0.01);
    CHECK(c.ts.last_sync[0] && c.ts.steps == 1);

    // After the step: small offset, the RTC was just written, nothing more.
    run(&c, 7, 600, 1, 3, 8, 0);
    CHECK(T.sets == 1 && T.rtc == 1);
    CHECK_NEAR(c.ts.offset, 0, 0.01);

    // Backwards by a day with a stable fix: also stepped.
    setup(&c, 86400);
    run(&c, 1, 10, 1, 3, 8, 0);
    CHECK(T.sets == 1 && T.rtc == 1);
    CHECK_NEAR(T.real, g0 + WIRE, 0.01);

    // The receiver output delay is added: the clock lands on the second.
    setup(&c, 50);
    c.cfg.time_sync_delay_ms = 400;
    run(&c, 1, 10, 1, 3, 8, -0.4);   // the RMC of second g0 + t arrives at t + 0.4
    CHECK(T.sets == 1);
    CHECK_NEAR(T.real, g0 + WIRE, 0.01);

    // A failed step (no CAP_SYS_TIME) is tried again after 10 minutes.
    setup(&c, -3600);
    T.set_rc = -EPERM;
    run(&c, 1, 10, 1, 3, 8, 0);
    CHECK(T.sets == 0 && c.ts.next_step > 10);
    T.set_rc = 0;
    run(&c, 11, 600, 1, 3, 8, 0);
    CHECK(T.sets == 0);
    run(&c, 601, 620, 1, 3, 8, 0);
    CHECK(T.sets == 1);

    // Fix lost: SyncTimeNow refuses at once.
    setup(&c, -3600);
    T.set_rc = -EPERM;
    run(&c, 1, 10, 1, 3, 8, 0);
    T.set_rc = 0;
    CHECK(ts_stable(&c, 10));
    run(&c, 11, 11, 0, 1, 0, 0);
    CHECK(!ts_stable(&c, 11) && ts_sync_now(&c, 11, &stepped, &wrote) == -ENODATA);
    // NMEA stops: stale after 5 s.
    setup(&c, 0.5);
    run(&c, 1, 10, 1, 3, 8, 0);
    CHECK(ts_stable(&c, 15) && !ts_stable(&c, 15.1));
}

static void test_small_offset(void)
{
    struct core c;

    // 0.5 s: no step, the RTC once at the start and then once per hour.
    setup(&c, 0.5);
    run(&c, 1, 10, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 1);
    CHECK_NEAR(c.ts.offset, 0.5 - WIRE, 0.01);
    CHECK_NEAR(T.rtc_value, g0 + 6 + WIRE, 0.01);    // GNSS time, not the system clock
    run(&c, 11, 3605, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 1);
    run(&c, 3606, 3606, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 2);

    // Exactly at the threshold: no step.
    setup(&c, 2.0 + WIRE - 0.01);
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 1);

    // No RTC configured: still no step, nothing written.
    setup(&c, 0.5);
    c.cfg.rtc_device[0] = 0;
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 0 && !c.ts.last_sync[0]);
}

static void test_ntp(void)
{
    struct core c;
    int stepped, wrote;

    // NTP keeps the clock: the RTC is written, the clock is not touched.
    setup(&c, 0.3);
    T.ntp = 1;
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 1);
    CHECK(ts_sync_now(&c, 20, &stepped, &wrote) == 0 && !stepped && T.rtc == 2);

    // NTP and GNSS disagree: neither a step nor an RTC write.
    setup(&c, -3600);
    T.ntp = 1;
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 0);
    CHECK_NEAR(c.ts.offset, -3600 - WIRE, 0.01);
    CHECK(ts_sync_now(&c, 20, &stepped, &wrote) == -EALREADY && T.sets == 0);

    // system_clock = always steps also against NTP.
    setup(&c, -3600);
    T.ntp = 1;
    c.cfg.system_clock = SC_ALWAYS;
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 1 && T.rtc == 1);
}

// Every combination of system_clock, rtc, NTP and the offset: what the
// daemon does by itself in 20 s of a stable fix, then what SyncTimeNow does.
static void test_modes(void)
{
    enum { SMALL, BIG };
    enum { NONE = -99 };            // SyncTimeNow not checked (after a step)
    static const struct
    {
        int mode, rtc, ntp, offset;
        int sets, rtcs;             // after 20 s
        int rc, stepped, wrote;     // SyncTimeNow
    } cases[] = {
        // mode     rtc ntp offset  sets rtcs  rc          stepped wrote
        { SC_AUTO,   1, 0, SMALL,   0, 1,     0,          1, 1 },
        { SC_AUTO,   0, 0, SMALL,   0, 0,     0,          1, 0 },
        { SC_AUTO,   1, 0, BIG,     1, 1,     NONE,       0, 0 },
        { SC_AUTO,   0, 0, BIG,     1, 0,     NONE,       0, 0 },
        { SC_AUTO,   1, 1, SMALL,   0, 1,     0,          0, 1 },
        { SC_AUTO,   0, 1, SMALL,   0, 0,     0,          0, 0 },
        { SC_AUTO,   1, 1, BIG,     0, 0,     -EALREADY,  0, 0 },
        { SC_AUTO,   0, 1, BIG,     0, 0,     -EALREADY,  0, 0 },
        { SC_ALWAYS, 1, 0, SMALL,   0, 1,     0,          1, 1 },
        { SC_ALWAYS, 0, 0, SMALL,   0, 0,     0,          1, 0 },
        { SC_ALWAYS, 1, 0, BIG,     1, 1,     NONE,       0, 0 },
        { SC_ALWAYS, 0, 0, BIG,     1, 0,     NONE,       0, 0 },
        { SC_ALWAYS, 1, 1, SMALL,   0, 1,     0,          1, 1 },
        { SC_ALWAYS, 0, 1, SMALL,   0, 0,     0,          1, 0 },
        { SC_ALWAYS, 1, 1, BIG,     1, 1,     NONE,       0, 0 },
        { SC_ALWAYS, 0, 1, BIG,     1, 0,     NONE,       0, 0 },
        { SC_OFF,    1, 0, SMALL,   0, 1,     0,          0, 1 },
        { SC_OFF,    0, 0, SMALL,   0, 0,     -EPERM,     0, 0 },
        { SC_OFF,    1, 0, BIG,     0, 1,     0,          0, 1 },
        { SC_OFF,    0, 0, BIG,     0, 0,     -EPERM,     0, 0 },
        { SC_OFF,    1, 1, SMALL,   0, 1,     0,          0, 1 },
        { SC_OFF,    0, 1, SMALL,   0, 0,     -EPERM,     0, 0 },
        { SC_OFF,    1, 1, BIG,     0, 0,     -EALREADY,  0, 0 },
        { SC_OFF,    0, 1, BIG,     0, 0,     -EPERM,     0, 0 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct core c;
        int stepped, wrote;
        setup(&c, cases[i].offset == BIG ? -3600 : 0.5);
        T.ntp = cases[i].ntp;
        c.cfg.system_clock = cases[i].mode;
        c.cfg.rtc = cases[i].rtc;
        run(&c, 1, 20, 1, 3, 8, 0);
        int ok = T.sets == cases[i].sets && T.rtc == cases[i].rtcs && !isnan(c.ts.offset);
        // The RTC gets the GNSS time, also when the system clock is not set.
        if (ok && T.rtc)
            ok = fabs(T.rtc_value - (g0 + 6 + WIRE)) < 0.01;
        if (ok && cases[i].rc != NONE)
        {
            int sets = T.sets, rtcs = T.rtc;
            int rc = ts_sync_now(&c, 20, &stepped, &wrote);
            ok = rc == cases[i].rc && stepped == cases[i].stepped && wrote == cases[i].wrote &&
                 T.sets - sets == cases[i].stepped && T.rtc - rtcs == cases[i].wrote;
        }
        if (!ok)
            fprintf(stderr, "  case %zu: %s rtc %d ntp %d %s: %d sets, %d RTC writes\n", i,
                    system_clock_mode_name(cases[i].mode), cases[i].rtc, cases[i].ntp,
                    cases[i].offset == BIG ? "big" : "small", T.sets, T.rtc);
        CHECK(ok);
    }

    // rtc = on without rtc_device: nothing written, SyncTimeNow says so.
    struct core c;
    int stepped, wrote;
    setup(&c, 0.5);
    c.cfg.system_clock = SC_OFF;
    c.cfg.rtc_device[0] = 0;
    run(&c, 1, 20, 1, 3, 8, 0);
    CHECK(T.sets == 0 && T.rtc == 0);
    CHECK(ts_sync_now(&c, 20, &stepped, &wrote) == 0 && !stepped && !wrote);

    // Both off: SyncTimeNow refuses also without a fix.
    setup(&c, 0.5);
    c.cfg.system_clock = SC_OFF;
    c.cfg.rtc = 0;
    CHECK(ts_sync_now(&c, 0, &stepped, &wrote) == -EPERM);
}

static int load(struct gnssd_config *c, const char *text)
{
    char path[] = "/tmp/gnssd-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    config_defaults(c);
    int rc = config_load(c, path);
    unlink(path);
    return rc;
}

static void test_ts_config(void)
{
    struct gnssd_config c;
    config_defaults(&c);
    CHECK(c.system_clock == SC_AUTO && c.rtc == 1 && c.time_sync_threshold_s == 2 &&
          c.time_sync_delay_ms == 125 && !strcmp(c.rtc_device, "/dev/rtc1"));

    CHECK(load(&c, "system_clock = always\nrtc = off\ntime_sync_threshold_s = 5\nrtc_device =\n") == 0);
    CHECK(c.system_clock == SC_ALWAYS && c.rtc == 0 && c.time_sync_threshold_s == 5 &&
          !c.rtc_device[0]);
    CHECK(!strcmp(system_clock_mode_name(c.system_clock), "always"));
    CHECK(load(&c, "system_clock = off\n") == 0 && c.system_clock == SC_OFF && c.rtc == 1);
    CHECK(load(&c, "rtc = off\n") == 0 && c.system_clock == SC_AUTO && c.rtc == 0);

    CHECK(load(&c, "system_clock = always\nsystem_clock = off\n") == 0 && c.system_clock == SC_OFF);

    core_log_quiet = 1;
    CHECK(load(&c, "system_clock = on\n") == -1);
    CHECK(load(&c, "rtc = /dev/rtc1\n") == -1);
}

void test_timesync(void)
{
    test_gnss_time();
    test_ts_config();
    test_no_fix();
    test_bad_dates();
    test_step();
    test_small_offset();
    test_ntp();
    test_modes();
}
