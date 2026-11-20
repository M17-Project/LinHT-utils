// linht-gnssd tests: core with a fake receiver

#include <errno.h>
#include <math.h>
#include <string.h>

#include "check.h"
#include "core.h"
#include "fixtures.h"

static struct
{
    int open_rc;        // what the next open returns
    int opens, closes, resets, reset_rc;
    int writes;
    char last[128];     // last write
    char all[2048];     // all writes
    int forwarded;
} F;

static int f_open(void *ctx)
{
    (void)ctx;
    F.opens++;
    return F.open_rc;
}

static void f_close(void *ctx)
{
    (void)ctx;
    F.closes++;
}

static int f_write(void *ctx, const char *buf, size_t n)
{
    (void)ctx;
    F.writes++;
    snprintf(F.last, sizeof(F.last), "%.*s", (int)n, buf);
    size_t used = strlen(F.all);
    snprintf(F.all + used, sizeof(F.all) - used, "%s", F.last);
    return 0;
}

static int f_reset(void *ctx)
{
    (void)ctx;
    F.resets++;
    return F.reset_rc;
}

static void f_forward(void *ctx, const char *line, size_t n)
{
    (void)ctx, (void)line, (void)n;
    F.forwarded++;
}

static const struct core_ops fops = { f_open, f_close, f_write, f_reset, f_forward };

static void setup(struct core *c)
{
    memset(&F, 0, sizeof(F));
    memset(c, 0, sizeof(*c));
    config_defaults(&c->cfg);
    core_init(c, &fops, NULL);
}

static void feed_lines(struct core *c, const char *const *lines, double now)
{
    for (; *lines; lines++)
    {
        core_feed(c, *lines, strlen(*lines), now);
        core_feed(c, "\r\n", 2, now);
    }
}

// One epoch every second from t0 to t1.
static void run(struct core *c, const char *const *epoch, double t0, double t1)
{
    for (double t = t0; t <= t1 + 1e-9; t += 0.5)
    {
        if (fmod(t, 1.0) < 1e-9)
            feed_lines(c, epoch, t);
        core_tick(c, t);
        core_update(c, t);
    }
}

static void test_open_and_feed(void)
{
    struct core c;
    setup(&c);

    // tty locked by field-log: retry every 5 s.
    F.open_rc = -EWOULDBLOCK;
    core_tick(&c, 1);
    CHECK(F.opens == 1 && !c.open && c.health == H_TTY_BUSY);
    core_tick(&c, 3);
    CHECK(F.opens == 1);
    F.open_rc = 0;
    core_tick(&c, 6);
    CHECK(F.opens == 2 && c.open && c.health == H_STARTING);
    // Every open asks for the version, which also ends a standby.
    CHECK(!strcmp(F.all, "$PQVERNO,R*3F\r\n$PMTK605*31\r\n"));

    // Bytes in odd pieces, with noise before the first '$'.
    const char *junk = "\x8f\x12r\xff,N*60\r\n$GNRMC,065403.132,V,,,,,0.00,0.00,201126,,,N,V*2B\r";
    core_feed(&c, junk, strlen(junk), 6.5);
    core_feed(&c, "\n$GPTXT,01,01,02,ANT", 20, 6.6);
    const char *rest = "STATUS=OPEN*2B\r\n$PQVERNO,R,LG77LICNR11A01S_GLN,2023/01/16,01:33*75\r\n";
    core_feed(&c, rest, strlen(rest), 6.7);
    CHECK(c.st.sentences == 3 && c.st.bad == 0);
    CHECK(F.forwarded == 3);
    CHECK(c.health == H_OK);
    CHECK(!strcmp(c.st.antenna, "OPEN"));
    CHECK(!strcmp(c.st.version, "LG77LICNR11A01S_GLN 2023/01/16 01:33"));

    // A sentence broken by a new '$' is dropped, the new one is kept.
    const char *broken = "$GPTXT,01,01,0$GPTXT,01,01,02,ANTSTATUS=OK*3B\r\n";
    core_feed(&c, broken, strlen(broken), 7);
    CHECK(!strcmp(c.st.antenna, "OK") && c.st.sentences == 4);
    // Too long: dropped until the line feed.
    char longline[300];
    memset(longline, 'A', sizeof(longline));
    longline[0] = '$';
    core_feed(&c, longline, sizeof(longline), 7);
    core_feed(&c, "\r\n", 2, 7);
    CHECK(c.st.sentences == 4 && c.st.bad == 1);
    // Wrong checksum: counted, not forwarded.
    core_feed(&c, "$GPTXT,01,01,02,ANTSTATUS=OK*3C\r\n", 33, 7);
    CHECK(c.st.bad == 2 && F.forwarded == 4);
}

static void test_watchdog(void)
{
    struct core c;
    setup(&c);
    core_tick(&c, 0);
    run(&c, fx_nofix, 1, 20);
    CHECK(c.health == H_OK && F.opens == 1);

    // Silence: reopen after watchdog_s (10 s).
    run(&c, (const char *const[]){ NULL }, 20.5, 30);
    CHECK(F.opens == 1 && c.health == H_OK);
    core_tick(&c, 30.5);
    CHECK(F.opens == 2 && F.closes == 1 && c.reopens == 1 && c.health == H_NO_DATA);
    CHECK(F.resets == 0);
    core_tick(&c, 40.4);
    CHECK(F.opens == 2);
    core_tick(&c, 41);
    core_tick(&c, 51.5);
    CHECK(F.opens == 4 && c.stalls == 3);
    // The third reopen without NMEA also pulses RESET_N.
    CHECK(F.resets == 1 && c.resets == 1);
    core_tick(&c, 62);
    core_tick(&c, 73);
    CHECK(F.opens == 6 && F.resets == 1);   // at most one reset in 10 min

    // Data again: health ok, counters start over.
    run(&c, fx_nofix, 74, 76);
    CHECK(c.health == H_OK && c.stalls == 0);

    // Without a reset line the reopens go on.
    setup(&c);
    F.reset_rc = -ENOTSUP;
    core_tick(&c, 0);
    for (double t = 0.5; t < 60; t += 0.5)
        core_tick(&c, t);
    CHECK(c.resets == 0 && c.reopens >= 4);

    // A read error closes the tty, the next tick opens it again.
    setup(&c);
    core_tick(&c, 0);
    core_link_lost(&c, 1, -EIO);
    CHECK(!c.open && F.closes == 1);
    core_tick(&c, 1.5);
    CHECK(!c.open);
    core_tick(&c, 2);
    CHECK(c.open && F.opens == 2);
}

static void test_standby_wake(void)
{
    struct core c;
    setup(&c);
    core_tick(&c, 0);
    run(&c, fx_nofix, 1, 3);

    CHECK(core_standby(&c, 3.2) == 0);
    CHECK(!strcmp(F.last, "$PMTK161,0*28\r\n"));
    CHECK(core_standby(&c, 3.2) == -EBUSY);
    CHECK(core_set_rate(&c, 2000, 3.2) == -EBUSY);
    // The epoch that was already on the way does not change anything.
    feed_lines(&c, fx_nofix, 3.3);
    CHECK(c.power == P_ON && !c.pend.done);
    feed_lines(&c, (const char *const[]){ "$PMTK001,161,3*36", NULL }, 3.4);
    CHECK(c.pend.done && c.pend.result == 0);
    CHECK(c.power == P_STANDBY && c.health == H_STANDBY);
    core_pend_clear(&c);

    // No NMEA in standby, no watchdog.
    for (double t = 3.5; t < 60; t += 0.5)
        core_tick(&c, t);
    CHECK(F.opens == 1 && c.health == H_STANDBY);

    // Wake: any byte, then NMEA ends the call.
    CHECK(core_wake(&c, 60) == 0);
    CHECK(!strcmp(F.last, "$PQVERNO,R*3F\r\n"));
    CHECK(c.power == P_ON && c.health == H_STARTING);
    feed_lines(&c, fx_nofix, 61);
    CHECK(c.pend.done && c.pend.result == 0 && c.health == H_OK);
    core_pend_clear(&c);

    // Wake without NMEA times out, then the watchdog runs again.
    CHECK(core_standby(&c, 62) == 0);
    feed_lines(&c, (const char *const[]){ "$PMTK001,161,3*36", NULL }, 62.1);
    core_pend_clear(&c);
    CHECK(core_wake(&c, 70) == 0);
    core_tick(&c, 72.9);
    CHECK(!c.pend.done);
    core_tick(&c, 73);
    CHECK(c.pend.done && c.pend.result == -ETIMEDOUT);
    core_pend_clear(&c);
    core_tick(&c, 80.6);
    CHECK(c.health == H_NO_DATA && F.opens == 2);

    // Standby without an ACK.
    setup(&c);
    core_tick(&c, 0);
    CHECK(core_standby(&c, 1) == 0);
    core_tick(&c, 3);
    CHECK(c.pend.done && c.pend.result == -ETIMEDOUT && c.power == P_ON);
    core_pend_clear(&c);

    // NMEA well after the standby ACK: the module woke by itself.
    setup(&c);
    core_tick(&c, 0);
    core_standby(&c, 1);
    feed_lines(&c, (const char *const[]){ "$PMTK001,161,3*36", NULL }, 1.1);
    core_pend_clear(&c);
    feed_lines(&c, fx_nofix, 5);
    CHECK(c.power == P_ON && c.health == H_OK);

    // Commands need an open tty.
    setup(&c);
    CHECK(core_standby(&c, 0) == -ENOTCONN);
}

static void test_commands(void)
{
    struct core c;
    setup(&c);
    core_tick(&c, 0);

    CHECK(core_restart(&c, "hot", 1) == 0 && !strcmp(F.last, "$PMTK101*32\r\n"));
    CHECK(core_restart(&c, "warm", 1) == 0 && !strcmp(F.last, "$PMTK102*31\r\n"));
    CHECK(core_restart(&c, "cold", 1) == 0 && !strcmp(F.last, "$PMTK103*30\r\n"));
    CHECK(core_restart(&c, "full-cold", 1) == 0 && !strcmp(F.last, "$PMTK104*37\r\n"));
    CHECK(core_restart(&c, "lukewarm", 1) == -EINVAL);
    CHECK(core_restart(&c, "reset", 1) == 0 && F.resets == 1 && c.resets == 1);
    F.reset_rc = -ENOTSUP;
    CHECK(core_restart(&c, "reset", 1) == -ENOTSUP && c.resets == 1);

    CHECK(core_set_rate(&c, 500, 1) == -EINVAL);
    CHECK(core_set_rate(&c, 1500, 1) == -EINVAL);
    CHECK(core_set_rate(&c, 20000, 1) == -EINVAL);
    int w = F.writes;
    CHECK(core_set_rate(&c, 2000, 1) == 0 && !strcmp(F.last, "$PMTK220,2000*1C\r\n"));
    CHECK(F.writes == w + 1);
    feed_lines(&c, (const char *const[]){ "$PMTK001,220,2*31", NULL }, 1.2);
    CHECK(c.pend.done && c.pend.result == -EIO);
    core_pend_clear(&c);
    CHECK(core_set_rate(&c, 1000, 2) == 0);
    // An ACK of another command does not end it.
    feed_lines(&c, (const char *const[]){ "$PMTK001,161,3*36", NULL }, 2.1);
    CHECK(!c.pend.done);
    feed_lines(&c, (const char *const[]){ "$PMTK001,220,3,1000*1D", NULL }, 2.2);
    CHECK(c.pend.done && c.pend.result == 0);
    core_pend_clear(&c);

    // Raw command: the $P replies of the next 1.5 s.
    CHECK(core_raw(&c, "PMTK414", 3) == 0 && !strcmp(F.last, "$PMTK414*33\r\n"));
    feed_lines(&c, fx_nofix, 3.5);
    feed_lines(&c, fx_replies, 3.6);
    core_tick(&c, 4.4);
    CHECK(!c.pend.done);
    core_tick(&c, 4.5);
    CHECK(c.pend.done && c.pend.result == 0 && c.pend.n == 3);
    CHECK(!strcmp(c.pend.lines[1], "$PMTK514,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*32"));
    core_pend_clear(&c);
    CHECK(core_raw(&c, "PMTK\x01", 5) == -EINVAL);
}

static void test_events(void)
{
    struct core c;
    setup(&c);
    core_tick(&c, 0);
    core_update(&c, 0);
    CHECK(c.fix_event == 1 && c.snap.fix == 0);
    c.fix_event = 0;

    run(&c, fx_nofix, 1, 3);
    CHECK(!c.fix_event && !c.pos_event);

    // Fix: one FixChanged, PositionChanged at most every 5 s.
    int pos = 0, fixes = 0;
    for (double t = 4; t <= 20; t += 0.5)
    {
        if (fmod(t, 1.0) < 1e-9)
            feed_lines(&c, fx_fix3d, t);
        core_tick(&c, t);
        core_update(&c, t);
        pos += c.pos_event;
        fixes += c.fix_event;
        c.pos_event = c.fix_event = 0;
    }
    CHECK(fixes == 1 && c.snap.fix == 3 && c.snap.valid);
    CHECK(pos == 4);   // t = 4, 9, 14, 19

    // Fix lost.
    run(&c, fx_nofix, 21, 22);
    CHECK(c.fix_event && c.snap.fix == 0);
    // The interval follows the RMC times (the fixtures jump, so it is not
    // checked here); stale data after silence keeps the fix at none.
    c.fix_event = 0;
    run(&c, (const char *const[]){ NULL }, 22.5, 29);
    CHECK(!c.fix_event && c.snap.view == 0);
}

static void test_interval(void)
{
    struct core c;
    char buf[128];
    setup(&c);
    core_tick(&c, 0);
    for (int i = 0; i < 4; i++)
    {
        char t[16];
        snprintf(t, sizeof(t), "%02d%02d%02d.000", 23, 59, 50 + 5 * i >= 60 ? 50 + 5 * i - 60 : 50 + 5 * i);
        if (50 + 5 * i >= 60)
            t[0] = '0', t[1] = '0', t[2] = '0', t[3] = '0';
        char body[96];
        snprintf(body, sizeof(body), "GNRMC,%s,V,,,,,0.00,0.00,201126,,,N,V", t);
        nmea_format(buf, sizeof(buf), body);
        core_feed(&c, buf, strlen(buf), 1 + 5 * i);
    }
    // 23:59:50, 23:59:55, 00:00:00, 00:00:05: 5 s also over midnight.
    CHECK(c.interval_ms == 5000);
    CHECK_NEAR(core_stale(&c), 12.5, 1e-9);

    // A gap of 9 s (standby) and then 1 s steps: 1 s, the gap never counts.
    const char *t2[] = { "000014.000", "000015.000", "000016.000", NULL };
    for (int i = 0; t2[i]; i++)
    {
        char body[96];
        snprintf(body, sizeof(body), "GNRMC,%s,V,,,,,0.00,0.00,201126,,,N,V", t2[i]);
        nmea_format(buf, sizeof(buf), body);
        core_feed(&c, buf, strlen(buf), 30 + i);
        core_feed(&c, "\r\n", 2, 30 + i);
        CHECK(c.interval_ms == (i < 2 ? 5000 : 1000));
    }
}

static void test_config(void)
{
    // The shipped file holds the built-in defaults.
    struct gnssd_config a, b;
    config_defaults(&a);
    config_defaults(&b);
    CHECK(config_load(&b, "gnssd.conf") == 0);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK(config_load(&b, "tests/no-such-file.conf") == 1);
}

void test_core(void)
{
    test_config();
    test_open_and_feed();
    test_watchdog();
    test_standby_wake();
    test_commands();
    test_events();
    test_interval();
}
