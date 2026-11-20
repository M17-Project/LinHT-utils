// linht-gnssd tests: NMEA parser and snapshot

#include <math.h>
#include <string.h>

#include "check.h"
#include "fixtures.h"
#include "nmea.h"

static void feed(struct nmea_state *s, const char *const *lines, double now)
{
    for (; *lines; lines++)
        nmea_parse(s, *lines, now);
}

static void test_checksum(void)
{
    char buf[128];
    CHECK(nmea_checksum_ok("$PMTK161,0*28"));
    CHECK(nmea_checksum_ok("$PMTK161,0*28\r\n"));
    CHECK(!nmea_checksum_ok("$PMTK161,0*29"));
    CHECK(!nmea_checksum_ok("$PMTK161,0"));
    CHECK(!nmea_checksum_ok("PMTK161,0*28"));
    CHECK(!nmea_checksum_ok("$GPTXT,01,01,02,ANTSTA$US=OPEN*2B"));

    // Examples of the specification.
    CHECK(nmea_format(buf, sizeof(buf), "PMTK161,0") == 15 && !strcmp(buf, "$PMTK161,0*28\r\n"));
    CHECK(nmea_format(buf, sizeof(buf), "$PMTK220,1000") > 0 && !strcmp(buf, "$PMTK220,1000*1F\r\n"));
    CHECK(nmea_format(buf, sizeof(buf), "$PMTK103*30") > 0 && !strcmp(buf, "$PMTK103*30\r\n"));
    CHECK(nmea_format(buf, sizeof(buf), "PQVERNO,R") > 0 && !strcmp(buf, "$PQVERNO,R*3F\r\n"));
    CHECK(nmea_format(buf, sizeof(buf), "PMTK353,1,1,0,0,0") > 0 &&
          !strcmp(buf, "$PMTK353,1,1,0,0,0*2B\r\n"));
    CHECK(nmea_format(buf, sizeof(buf), "") < 0);
    CHECK(nmea_format(buf, sizeof(buf), "PMTK\n161") < 0);
    CHECK(nmea_format(buf, sizeof(buf), "PMTK$161") < 0);
    CHECK(nmea_format(buf, 8, "PMTK161,0") < 0);
}

static void test_nofix(void)
{
    struct nmea_state s;
    struct gnss_snap o;
    nmea_init(&s);
    feed(&s, fx_nofix, 10);
    CHECK(s.sentences == 9 && s.bad == 0 && s.malformed == 0);
    nmea_snapshot(&s, 10.5, 5, &o);
    CHECK(o.fix == 0 && !o.valid);
    CHECK(isnan(o.lat) && isnan(o.lon) && isnan(o.alt) && isnan(o.speed));
    CHECK(o.used == 0 && o.view == 0 && o.tracked == 0);
    CHECK(isnan(o.cn0_best) && isnan(o.cn0_mean));
    CHECK(isnan(o.hdop) && isnan(o.pdop));
    CHECK(!strcmp(o.utc, "2026-11-20T06:54:03.132Z"));
    CHECK(!o.time_valid);
    CHECK(!strcmp(s.antenna, "OPEN"));

    nmea_init(&s);
    feed(&s, fx_nofix_notime, 10);
    nmea_snapshot(&s, 10, 5, &o);
    CHECK(!strcmp(o.utc, "1980-01-06T17:21:29.870Z"));
}

static void test_fix3d(void)
{
    struct nmea_state s;
    struct gnss_snap o;
    nmea_init(&s);
    feed(&s, fx_nofix, 9);
    feed(&s, fx_fix3d, 10);
    CHECK(s.bad == 0 && s.malformed == 0);
    nmea_snapshot(&s, 10.2, 5, &o);
    CHECK(o.fix == 3 && o.valid);
    CHECK_NEAR(o.lat, 31 + 49.333190 / 60, 1e-9);
    CHECK_NEAR(o.lon, 117 + 6.911552 / 60, 1e-9);
    CHECK_NEAR(o.alt, 53.489, 1e-9);
    CHECK_NEAR(o.speed, 0, 1e-9);
    CHECK_NEAR(o.course, 5.78, 1e-9);
    CHECK(!strcmp(o.utc, "2021-11-22T08:02:37.000Z") && o.time_valid);
    CHECK(o.used == 15);
    CHECK(o.used_sys[SYS_GPS] == 8 && o.used_sys[SYS_QZSS] == 2 && o.used_sys[SYS_GLONASS] == 5);
    CHECK(o.view == 19);
    CHECK(o.view_sys[SYS_GPS] == 9 && o.view_sys[SYS_QZSS] == 3 && o.view_sys[SYS_SBAS] == 1 &&
          o.view_sys[SYS_GLONASS] == 6);
    CHECK(o.tracked == 17);
    CHECK_NEAR(o.cn0_best, 47, 1e-9);
    CHECK_NEAR(o.cn0_mean, 686.0 / 17, 1e-9);
    CHECK_NEAR(o.hdop, 0.74, 1e-9);
    CHECK_NEAR(o.pdop, 1.35, 1e-9);
    CHECK_NEAR(o.vdop, 1.13, 1e-9);
    CHECK(!strcmp(s.antenna, "OK"));

    // A satellite without C/N0 stays in view and is not tracked.
    int found = 0;
    for (int i = 0; i < o.nsats; i++)
        if (o.sats[i].sys == SYS_GPS && o.sats[i].id == 10)
            found = isnan(o.sats[i].cn0) && o.sats[i].elev == 9 && o.sats[i].azim == 175;
    CHECK(found);

    // Old data does not count.
    nmea_snapshot(&s, 16, 5, &o);
    CHECK(o.fix == 0 && !o.valid && o.view == 0 && o.used == 0 && isnan(o.lat));
    CHECK(!strcmp(o.utc, "2021-11-22T08:02:37.000Z") && !o.time_valid);

    // A fresh "no fix" wins over an older fix.
    feed(&s, fx_fix3d, 20);
    nmea_parse(&s, "$GNRMC,065403.132,V,,,,,0.00,0.00,201126,,,N,V*2B", 20.5);
    nmea_snapshot(&s, 20.6, 5, &o);
    CHECK(!o.valid && o.fix == 0);
}

static void test_fix2d(void)
{
    struct nmea_state s;
    struct gnss_snap o;
    char buf[128];
    nmea_init(&s);
    nmea_parse(&s, "$GPRMC,080325.000,A,3149.332234,N,11706.911645,E,0.07,33.95,221121,,,A,V*27", 1);
    nmea_format(buf, sizeof(buf), "GNGSA,A,2,22,29,26,,,,,,,,,,2.5,2.3,0.9,1");
    nmea_parse(&s, buf, 1);
    nmea_snapshot(&s, 1, 5, &o);
    CHECK(o.fix == 2 && o.valid);
    CHECK(isnan(o.alt));
    CHECK_NEAR(o.speed, 0.07 * 1852 / 3600, 1e-9);
    CHECK_NEAR(o.course, 33.95, 1e-9);
    CHECK(o.used == 3 && o.used_sys[SYS_GPS] == 3);
    CHECK_NEAR(o.lat, 31 + 49.332234 / 60, 1e-9);

    // Southern and western hemispheres.
    nmea_format(buf, sizeof(buf), "GNRMC,120000.000,A,3349.5000,S,07040.2500,W,10.0,180.0,010125,,,A,V");
    nmea_parse(&s, buf, 2);
    nmea_snapshot(&s, 2, 5, &o);
    CHECK_NEAR(o.lat, -(33 + 49.5 / 60), 1e-9);
    CHECK_NEAR(o.lon, -(70 + 40.25 / 60), 1e-9);
    CHECK_NEAR(o.speed, 10 * 1852.0 / 3600, 1e-9);
}

static void test_gsv(void)
{
    struct nmea_state s;
    struct gnss_snap o;
    nmea_init(&s);
    // A group without its first sentence is dropped.
    nmea_parse(&s, "$GPGSV,4,2,13,50,51,161,39,193,49,150,41,22,30,288,41,29,26,082,41,1*5E", 1);
    nmea_snapshot(&s, 1, 5, &o);
    CHECK(o.view == 0);
    // A group with a missing sentence is dropped, the previous one stays.
    feed(&s, fx_fix3d, 2);
    nmea_parse(&s, "$GPGSV,4,1,13,194,72,074,43,26,61,222,45,31,61,352,43,32,60,116,47,1*5A", 3);
    nmea_parse(&s, "$GPGSV,4,3,13,25,23,042,38,03,20,311,38,16,19,215,38,10,09,175,,1*6A", 3);
    nmea_parse(&s, "$GPGSV,4,4,13,195,,,,1*5B", 3);
    nmea_snapshot(&s, 3, 5, &o);
    CHECK(o.view == 19);
    // An empty group replaces the old one: the satellites are gone.
    nmea_parse(&s, "$GPGSV,1,1,00,1*64", 4);
    nmea_snapshot(&s, 4, 5, &o);
    CHECK(o.view == 6 && o.view_sys[SYS_GPS] == 0 && o.view_sys[SYS_GLONASS] == 6);

    // NMEA 4.10 without the signal ID field.
    char buf[128];
    nmea_init(&s);
    nmea_format(buf, sizeof(buf), "GPGSV,1,1,02,05,40,100,31,12,10,200,");
    nmea_parse(&s, buf, 1);
    nmea_snapshot(&s, 1, 5, &o);
    CHECK(o.view == 2 && o.tracked == 1);
    CHECK_NEAR(o.cn0_best, 31, 1e-9);
    // Broken field counts.
    nmea_format(buf, sizeof(buf), "GPGSV,1,1,02,05,40,100");
    nmea_parse(&s, buf, 1);
    CHECK(s.malformed == 1);
}

static void test_proprietary(void)
{
    struct nmea_state s;
    nmea_init(&s);
    feed(&s, fx_replies, 1);
    CHECK(!strcmp(s.version, "LG77LICNR11A01S_GLN 2023/01/16 01:33"));
    CHECK(nmea_parse(&s, "$PMTK001,161,3*36", 1) == NMEA_ACK);
    CHECK(s.ack_cmd == 161 && s.ack_flag == 3 && s.ack_seq == 1);
    CHECK(nmea_parse(&s, "$PMTK001,220,3,1000*1D", 1) == NMEA_ACK);
    CHECK(s.ack_cmd == 220 && s.ack_seq == 2);
    nmea_parse(&s, "$PMTK705,MT3333_AXN5.1.9_MODULE_STD_F9_P1,000C,Quectel-LG77LIC,1.0*38", 1);
    CHECK(!strcmp(s.release, "MT3333_AXN5.1.9_MODULE_STD_F9_P1 Quectel-LG77LIC"));
    CHECK(!strcmp(s.version, "LG77LICNR11A01S_GLN 2023/01/16 01:33"));
    char buf[64];
    nmea_format(buf, sizeof(buf), "PMTK010,001");
    nmea_parse(&s, buf, 1);
    CHECK(s.startup_seq == 1);

    char zda[64];
    nmea_format(zda, sizeof(zda), "GNZDA,080237.00,22,11,2021,00,00");
    CHECK(nmea_parse(&s, zda, 5) == NMEA_ZDA);
    struct gnss_snap o;
    nmea_snapshot(&s, 5, 5, &o);
    CHECK(!strcmp(o.utc, "2021-11-22T08:02:37.00Z"));
}

void test_nmea(void)
{
    test_checksum();
    test_nofix();
    test_fix3d();
    test_fix2d();
    test_gsv();
    test_proprietary();
}
