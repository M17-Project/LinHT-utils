// linht-gnssd: NMEA 0183 parser for the Quectel LG77L
//
// Sentences: RMC, GGA, GSA, GSV, ZDA, TXT (ANTSTATUS) and the proprietary
// replies PMTK001 (ACK), PMTK010 (system message), PMTK514 and PQVERNO.
// Reference: Quectel Lx0&Lx6&LC86L&LG77L GNSS Protocol Specification V2.0.
// No I/O here, the tests feed it recorded sentences.

#ifndef GNSSD_NMEA_H
#define GNSSD_NMEA_H

#include <stddef.h>
#include <stdint.h>

#define NMEA_MAX        100     // longest sentence we accept, without CR LF
#define NMEA_MAX_SATS   64
#define NMEA_GROUPS     12      // GSV groups (talker and signal ID)
#define NMEA_GSA_SYS    6       // GSA by system ID 1 to 5, 0 = no system ID

enum gnss_sys
{
    SYS_GPS,
    SYS_GLONASS,
    SYS_GALILEO,
    SYS_BEIDOU,
    SYS_QZSS,
    SYS_SBAS,
    SYS_N
};

extern const char *const gnss_sys_name[SYS_N];

struct nmea_sat
{
    uint8_t sys;                // enum gnss_sys
    uint16_t id;                // satellite ID as in the sentence
    int16_t elev, azim;         // degrees, -1 = empty field
    double cn0;                 // dB-Hz, NaN = empty field (not tracked)
};

struct nmea_gsv
{
    char talker[3];
    char signal[3];
    int total, next, count;     // sentences, next expected index, satellites
    double t;                   // time of the first sentence
    int n;
    struct nmea_sat sats[NMEA_MAX_SATS];
};

struct nmea_state
{
    // RMC
    double t_rmc;               // receive time, 0 = never
    int rmc_valid;              // status A
    double rmc_lat, rmc_lon;    // degrees, NaN = empty
    double sog_kn, cog_deg;     // NaN = empty
    char rmc_time[16];          // hhmmss.sss
    char rmc_date[8];           // ddmmyy

    // GGA
    double t_gga;
    int gga_quality;            // 0 = no fix
    int gga_used;               // -1 = empty
    double gga_lat, gga_lon, gga_alt, gga_hdop;

    // ZDA (not in the LG77L specification, parsed when present)
    double t_zda;
    char zda_iso[32];

    // GSA, index = NMEA system ID
    struct
    {
        double t;
        int fix_mode;           // 1 none, 2 = 2D, 3 = 3D
        char talker[3];
        int n;
        uint16_t ids[12];
        double pdop, hdop, vdop;
    } gsa[NMEA_GSA_SYS];

    // GSV: groups being received and complete ones
    struct nmea_gsv pending[NMEA_GROUPS];
    struct nmea_gsv done[NMEA_GROUPS];

    // GPTXT ANTSTATUS=...
    double t_ant;
    char antenna[24];

    // Proprietary replies
    char version[64];           // PQVERNO: Quectel firmware, date, time
    char release[64];           // PMTK705: chipset firmware, product model
    unsigned ack_seq;           // counts PMTK001
    int ack_cmd, ack_flag;      // last PMTK001
    unsigned startup_seq;       // counts PMTK010,001 (module start)

    uint64_t sentences;         // valid checksum
    uint32_t bad;               // bad checksum or no checksum
    uint32_t malformed;         // valid checksum, fields do not fit
};

// Snapshot of what the receiver reports, at a given time. Values older than
// `stale` seconds are not used.
struct gnss_snap
{
    int fix;                    // 0 none, 2 = 2D, 3 = 3D
    int valid;                  // fix and fresh data
    double lat, lon, alt;       // degrees, m MSL; NaN without a fix
    double speed, course;       // m/s, degrees true; NaN without a fix
    char utc[32];               // ISO 8601, empty = never received
    int time_valid;             // RMC status A
    uint32_t used, used_sys[SYS_N];
    uint32_t view, view_sys[SYS_N];
    uint32_t tracked;           // in view with a C/N0
    double hdop, pdop, vdop;    // NaN = empty
    double cn0_best, cn0_mean;  // over the tracked satellites, NaN = none
    int nsats;
    struct nmea_sat sats[NMEA_MAX_SATS];
};

enum nmea_kind
{
    NMEA_BAD = -1,              // no checksum or a wrong one
    NMEA_OTHER = 0,             // valid, not used (VTG, GLL, unknown)
    NMEA_RMC, NMEA_GGA, NMEA_GSA, NMEA_GSV, NMEA_ZDA, NMEA_TXT,
    NMEA_ACK,                   // PMTK001
    NMEA_PROPRIETARY,           // any other $P sentence
};

void nmea_init(struct nmea_state *s);

// One sentence, "$...*hh", trailing CR or LF allowed. `now` is a monotonic
// time in seconds and must be above 0.
enum nmea_kind nmea_parse(struct nmea_state *s, const char *line, double now);

// 1 when the line has a valid "*hh" checksum.
int nmea_checksum_ok(const char *line);

// "$<body>*hh\r\n" from "PMTK161,0", "$PMTK161,0" or "$PMTK161,0*28".
// Printable ASCII only, no '$' or '*' inside. Returns the length or -1.
int nmea_format(char *out, size_t size, const char *cmd);

void nmea_snapshot(const struct nmea_state *s, double now, double stale, struct gnss_snap *out);

const char *nmea_fix_name(int fix);

// System of a satellite ID from the talker of its GSV or GSA (Table 6 of
// the specification: GPS 1-32, SBAS 33-64, GLONASS 65-99, QZSS 193-202).
enum gnss_sys nmea_sat_sys(const char *talker, int system_id, int sat);

#endif
