// linht-gnssd: NMEA 0183 parser for the Quectel LG77L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmea.h"

#define MAX_FIELDS 48
#define GSV_GAP_S  3.0          // longest time between the sentences of one GSV group

const char *const gnss_sys_name[SYS_N] = { "GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "SBAS" };

void nmea_init(struct nmea_state *s)
{
    memset(s, 0, sizeof(*s));
    s->rmc_lat = s->rmc_lon = s->sog_kn = s->cog_deg = NAN;
    s->gga_lat = s->gga_lon = s->gga_alt = s->gga_hdop = NAN;
    s->gga_used = -1;
    for (int i = 0; i < NMEA_GSA_SYS; i++)
        s->gsa[i].pdop = s->gsa[i].hdop = s->gsa[i].vdop = NAN;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// Copies the body (between '$' and '*') to buf. Returns its length or -1.
static int checked_body(const char *line, char *buf, size_t size)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        n--;
    if (n < 4 || line[0] != '$' || line[n - 3] != '*' || n - 4 >= size)
        return -1;
    int hi = hexval(line[n - 2]), lo = hexval(line[n - 1]);
    if (hi < 0 || lo < 0)
        return -1;
    unsigned sum = 0;
    for (size_t i = 1; i < n - 3; i++)
    {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c > 0x7e || c == '$' || c == '*')
            return -1;
        sum ^= c;
    }
    if (sum != (unsigned)(hi * 16 + lo))
        return -1;
    memcpy(buf, line + 1, n - 4);
    buf[n - 4] = 0;
    return (int)(n - 4);
}

int nmea_checksum_ok(const char *line)
{
    char buf[NMEA_MAX + 1];
    return checked_body(line, buf, sizeof(buf)) >= 0;
}

int nmea_format(char *out, size_t size, const char *cmd)
{
    if (*cmd == '$')
        cmd++;
    size_t n = strcspn(cmd, "*");
    if (n == 0 || n > NMEA_MAX - 4)
        return -1;
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)cmd[i];
        if (c < 0x20 || c > 0x7e || c == '$')
            return -1;
        sum ^= c;
    }
    int len = snprintf(out, size, "$%.*s*%02X\r\n", (int)n, cmd, sum);
    return len < 0 || (size_t)len >= size ? -1 : len;
}

static int split(char *body, char **f)
{
    int n = 0;
    f[n++] = body;
    for (char *p = body; *p && n < MAX_FIELDS; p++)
        if (*p == ',')
        {
            *p = 0;
            f[n++] = p + 1;
        }
    return n;
}

static double num(const char *s)
{
    if (!*s)
        return NAN;
    char *end;
    double v = strtod(s, &end);
    return *end || !isfinite(v) ? NAN : v;
}

static int inum(const char *s)
{
    if (!*s)
        return -1;
    char *end;
    long v = strtol(s, &end, 10);
    return *end || v < 0 || v > 100000 ? -1 : (int)v;
}

// ddmm.mmmm and N/S, or dddmm.mmmm and E/W.
static double coord(const char *v, const char *hemi, int lat)
{
    double raw = num(v);
    if (isnan(raw) || raw < 0 || strlen(hemi) != 1)
        return NAN;
    char h = hemi[0];
    if (lat ? (h != 'N' && h != 'S') : (h != 'E' && h != 'W'))
        return NAN;
    double deg = floor(raw / 100);
    double min = raw - deg * 100;
    double d = deg + min / 60;
    if (min >= 60 || d > (lat ? 90 : 180))
        return NAN;
    return h == 'S' || h == 'W' ? -d : d;
}

static void copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

enum gnss_sys nmea_sat_sys(const char *talker, int system_id, int sat)
{
    switch (system_id)
    {
    case 2:
        return SYS_GLONASS;
    case 3:
        return SYS_GALILEO;
    case 4:
        return SYS_BEIDOU;
    case 5:
        return SYS_QZSS;
    }
    if (system_id != 1)
    {
        if (!strcmp(talker, "GL"))
            return SYS_GLONASS;
        if (!strcmp(talker, "GA"))
            return SYS_GALILEO;
        if (!strcmp(talker, "GB") || !strcmp(talker, "BD"))
            return SYS_BEIDOU;
        if (!strcmp(talker, "GQ") || !strcmp(talker, "QZ"))
            return SYS_QZSS;
        if (!strcmp(talker, "GN") && sat >= 65 && sat <= 99)
            return SYS_GLONASS;
    }
    // GP, GN or system ID 1: GPS with SBAS and QZSS by number.
    if (sat >= 33 && sat <= 64)
        return SYS_SBAS;
    if (sat >= 193 && sat <= 202)
        return SYS_QZSS;
    return SYS_GPS;
}

static struct nmea_gsv *slot(struct nmea_gsv *tab, const char *talker, const char *signal, int create)
{
    struct nmea_gsv *free_slot = NULL, *oldest = &tab[0];
    for (int i = 0; i < NMEA_GROUPS; i++)
    {
        struct nmea_gsv *g = &tab[i];
        if (!g->talker[0])
        {
            if (!free_slot)
                free_slot = g;
            continue;
        }
        if (!strcmp(g->talker, talker) && !strcmp(g->signal, signal))
            return g;
        if (g->t < oldest->t)
            oldest = g;
    }
    if (!create)
        return NULL;
    return free_slot ? free_slot : oldest;
}

static enum nmea_kind parse_gsv(struct nmea_state *s, const char *talker, char **f, int nf, double now)
{
    if (nf < 4)
        return NMEA_BAD;
    int total = inum(f[1]), idx = inum(f[2]), count = inum(f[3]);
    if (total < 1 || total > 9 || idx < 1 || idx > total || count < 0)
        return NMEA_BAD;
    int nb = nf - 4;
    const char *signal = "";
    if (nb % 4 == 1)
        signal = f[nf - 1], nb--;
    else if (nb % 4)
        return NMEA_BAD;

    struct nmea_gsv *p = slot(s->pending, talker, signal, idx == 1);
    if (!p)
        return NMEA_GSV;        // middle of a group we did not see start
    if (idx == 1)
    {
        memset(p, 0, sizeof(*p));
        copy(p->talker, sizeof(p->talker), talker);
        copy(p->signal, sizeof(p->signal), signal);
        p->total = total;
        p->count = count;
        p->next = 1;
        p->t = now;
    }
    if (p->next != idx || p->total != total || p->count != count || now - p->t > GSV_GAP_S)
    {
        p->talker[0] = 0;
        return NMEA_GSV;
    }
    for (int i = 0; i < nb; i += 4)
    {
        char **q = f + 4 + i;
        int id = inum(q[0]);
        if (id < 0 || p->n >= NMEA_MAX_SATS)
            continue;
        struct nmea_sat *sat = &p->sats[p->n++];
        sat->id = (uint16_t)id;
        sat->sys = (uint8_t)nmea_sat_sys(talker, 0, id);
        sat->elev = (int16_t)inum(q[1]);
        sat->azim = (int16_t)inum(q[2]);
        sat->cn0 = num(q[3]);
    }
    p->next++;
    if (idx == total)
    {
        struct nmea_gsv *d = slot(s->done, talker, signal, 1);
        *d = *p;
        p->talker[0] = 0;
    }
    return NMEA_GSV;
}

static enum nmea_kind parse_proprietary(struct nmea_state *s, char **f, int nf)
{
    if (!strcmp(f[0], "PMTK001") && nf >= 3)
    {
        s->ack_cmd = inum(f[1]);
        s->ack_flag = inum(f[2]);
        s->ack_seq++;
        return NMEA_ACK;
    }
    if (!strcmp(f[0], "PQVERNO") && nf >= 3 && !strcmp(f[1], "R"))
        snprintf(s->version, sizeof(s->version), "%s%s%s%s%s", f[2], nf > 3 ? " " : "",
                 nf > 3 ? f[3] : "", nf > 4 ? " " : "", nf > 4 ? f[4] : "");
    else if (!strcmp(f[0], "PMTK705") && nf >= 2)
        snprintf(s->release, sizeof(s->release), "%s%s%s", f[1], nf > 3 ? " " : "", nf > 3 ? f[3] : "");
    else if (!strcmp(f[0], "PMTK010") && nf >= 2 && !strcmp(f[1], "001"))
        s->startup_seq++;
    return NMEA_PROPRIETARY;
}

enum nmea_kind nmea_parse(struct nmea_state *s, const char *line, double now)
{
    char buf[NMEA_MAX + 1];
    char *f[MAX_FIELDS];

    if (checked_body(line, buf, sizeof(buf)) < 0)
    {
        s->bad++;
        return NMEA_BAD;
    }
    s->sentences++;
    int nf = split(buf, f);
    if (f[0][0] == 'P')
        return parse_proprietary(s, f, nf);
    if (strlen(f[0]) != 5)
        return NMEA_OTHER;

    char talker[3] = { f[0][0], f[0][1], 0 };
    const char *type = f[0] + 2;
    enum nmea_kind k = NMEA_OTHER;

    if (!strcmp(type, "RMC"))
    {
        if (nf < 10)
            goto malformed;
        s->t_rmc = now;
        copy(s->rmc_time, sizeof(s->rmc_time), f[1]);
        s->rmc_valid = !strcmp(f[2], "A");
        s->rmc_lat = coord(f[3], f[4], 1);
        s->rmc_lon = coord(f[5], f[6], 0);
        s->sog_kn = num(f[7]);
        s->cog_deg = num(f[8]);
        copy(s->rmc_date, sizeof(s->rmc_date), f[9]);
        k = NMEA_RMC;
    }
    else if (!strcmp(type, "GGA"))
    {
        if (nf < 10)
            goto malformed;
        s->t_gga = now;
        s->gga_lat = coord(f[2], f[3], 1);
        s->gga_lon = coord(f[4], f[5], 0);
        s->gga_quality = inum(f[6]) < 0 ? 0 : inum(f[6]);
        s->gga_used = inum(f[7]);
        s->gga_hdop = num(f[8]);
        s->gga_alt = num(f[9]);
        k = NMEA_GGA;
    }
    else if (!strcmp(type, "GSA"))
    {
        if (nf < 18)
            goto malformed;
        int sysid = nf > 18 ? inum(f[18]) : 0;
        if (sysid < 0 || sysid >= NMEA_GSA_SYS)
            sysid = 0;
        s->gsa[sysid].t = now;
        s->gsa[sysid].fix_mode = inum(f[2]);
        copy(s->gsa[sysid].talker, sizeof(s->gsa[sysid].talker), talker);
        s->gsa[sysid].n = 0;
        for (int i = 3; i < 15; i++)
        {
            int id = inum(f[i]);
            if (id >= 0)
                s->gsa[sysid].ids[s->gsa[sysid].n++] = (uint16_t)id;
        }
        s->gsa[sysid].pdop = num(f[15]);
        s->gsa[sysid].hdop = num(f[16]);
        s->gsa[sysid].vdop = num(f[17]);
        k = NMEA_GSA;
    }
    else if (!strcmp(type, "GSV"))
    {
        k = parse_gsv(s, talker, f, nf, now);
        if (k == NMEA_BAD)
            goto malformed;
    }
    else if (!strcmp(type, "ZDA"))
    {
        if (nf < 5 || strlen(f[1]) < 6 || strlen(f[2]) != 2 || strlen(f[3]) != 2 || strlen(f[4]) != 4)
            goto malformed;
        s->t_zda = now;
        snprintf(s->zda_iso, sizeof(s->zda_iso), "%s-%s-%sT%.2s:%.2s:%sZ", f[4], f[3], f[2], f[1],
                 f[1] + 2, f[1] + 4);
        k = NMEA_ZDA;
    }
    else if (!strcmp(type, "TXT"))
    {
        if (nf < 5)
            goto malformed;
        if (!strncmp(f[4], "ANTSTATUS=", 10))
        {
            copy(s->antenna, sizeof(s->antenna), f[4] + 10);
            s->t_ant = now;
        }
        k = NMEA_TXT;
    }
    return k;

malformed:
    s->malformed++;
    return NMEA_OTHER;
}

static int fresh(double t, double now, double stale)
{
    return t > 0 && now - t <= stale;
}

const char *nmea_fix_name(int fix)
{
    return fix == 3 ? "3D" : fix == 2 ? "2D" : "none";
}

static void rmc_iso(const struct nmea_state *s, char *out, size_t size)
{
    const char *t = s->rmc_time, *d = s->rmc_date;
    if (strlen(t) < 6 || strlen(d) != 6)
    {
        out[0] = 0;
        return;
    }
    int yy = (d[4] - '0') * 10 + (d[5] - '0');
    snprintf(out, size, "%d-%.2s-%.2sT%.2s:%.2s:%sZ", yy < 80 ? 2000 + yy : 1900 + yy, d + 2, d, t,
             t + 2, t + 4);
}

void nmea_snapshot(const struct nmea_state *s, double now, double stale, struct gnss_snap *o)
{
    memset(o, 0, sizeof(*o));
    o->lat = o->lon = o->alt = o->speed = o->course = NAN;
    o->hdop = o->pdop = o->vdop = o->cn0_best = o->cn0_mean = NAN;

    int gga = fresh(s->t_gga, now, stale), rmc = fresh(s->t_rmc, now, stale);
    int valid = (gga && s->gga_quality > 0) || (rmc && s->rmc_valid);
    // A fresh report of "no fix" from either sentence wins.
    if ((gga && s->gga_quality == 0) || (rmc && !s->rmc_valid))
        valid = 0;

    int dim = 0, gsa_any = -1;
    for (int i = 0; i < NMEA_GSA_SYS; i++)
    {
        if (!fresh(s->gsa[i].t, now, stale))
            continue;
        if (s->gsa[i].fix_mode > dim)
            dim = s->gsa[i].fix_mode;
        if (gsa_any < 0 || s->gsa[i].t > s->gsa[gsa_any].t)
            gsa_any = i;
        for (int j = 0; j < s->gsa[i].n; j++)
        {
            int sys = nmea_sat_sys(s->gsa[i].talker, i, s->gsa[i].ids[j]);
            o->used_sys[sys]++;
        }
    }
    uint32_t used_sum = 0;
    for (int i = 0; i < SYS_N; i++)
        used_sum += o->used_sys[i];
    o->used = gga && s->gga_used >= 0 ? (uint32_t)s->gga_used : used_sum;

    if (valid)
    {
        o->fix = dim == 2 ? 2 : dim == 3 ? 3 : (gga && !isnan(s->gga_alt)) ? 3 : 2;
        if (gga && s->gga_quality > 0 && !isnan(s->gga_lat))
        {
            o->lat = s->gga_lat;
            o->lon = s->gga_lon;
        }
        else if (rmc)
        {
            o->lat = s->rmc_lat;
            o->lon = s->rmc_lon;
        }
        if (gga && o->fix == 3)
            o->alt = s->gga_alt;
        if (rmc)
        {
            o->speed = s->sog_kn * 1852.0 / 3600.0;
            o->course = s->cog_deg;
        }
        o->valid = !isnan(o->lat) && !isnan(o->lon);
        if (!o->valid)
            o->fix = 0;
    }

    if (gsa_any >= 0)
    {
        o->pdop = s->gsa[gsa_any].pdop;
        o->hdop = s->gsa[gsa_any].hdop;
        o->vdop = s->gsa[gsa_any].vdop;
    }
    if (isnan(o->hdop) && gga)
        o->hdop = s->gga_hdop;

    // Time: the newer of RMC and ZDA, kept after the data goes stale.
    if (s->t_zda > s->t_rmc)
        snprintf(o->utc, sizeof(o->utc), "%s", s->zda_iso);
    else
        rmc_iso(s, o->utc, sizeof(o->utc));
    o->time_valid = rmc && s->rmc_valid;

    // Satellites in view, one entry per system and ID (best C/N0 wins).
    double sum = 0;
    for (int g = 0; g < NMEA_GROUPS; g++)
    {
        const struct nmea_gsv *d = &s->done[g];
        if (!d->talker[0] || !fresh(d->t, now, stale))
            continue;
        for (int i = 0; i < d->n; i++)
        {
            const struct nmea_sat *sat = &d->sats[i];
            int j;
            for (j = 0; j < o->nsats; j++)
                if (o->sats[j].sys == sat->sys && o->sats[j].id == sat->id)
                    break;
            if (j == o->nsats)
            {
                if (o->nsats == NMEA_MAX_SATS)
                    continue;
                o->sats[o->nsats++] = *sat;
            }
            else if (!isnan(sat->cn0) && (isnan(o->sats[j].cn0) || sat->cn0 > o->sats[j].cn0))
            {
                o->sats[j].cn0 = sat->cn0;
            }
        }
    }
    for (int j = 0; j < o->nsats; j++)
    {
        const struct nmea_sat *sat = &o->sats[j];
        o->view_sys[sat->sys]++;
        if (!isnan(sat->cn0) && sat->cn0 > 0)
        {
            o->tracked++;
            sum += sat->cn0;
            if (isnan(o->cn0_best) || sat->cn0 > o->cn0_best)
                o->cn0_best = sat->cn0;
        }
    }
    o->view = (uint32_t)o->nsats;
    if (o->tracked)
        o->cn0_mean = sum / o->tracked;
}
