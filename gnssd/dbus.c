// linht-gnssd: D-Bus interface org.m17project.linht.Gnss1

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"

// Snapshot of the scalar properties, refreshed by dbus_update. The getters
// read it through the sd-bus property offset.
struct props
{
    const char *fix, *health, *power, *antenna, *version, *utc, *device, *pty, *sock;
    const char *ts_mode, *last_sync;
    double ts_offset;
    int valid, time_valid;
    double lat, lon, alt, speed, course, hdop, pdop, vdop, cn0_best, cn0_mean, age;
    uint32_t used, view, tracked, interval, reopens, resets, bad, dropped;
    uint64_t sentences;
    char antenna_buf[24], version_buf[136], utc_buf[32], last_sync_buf[32];
};

static struct
{
    sd_bus *bus;
    struct core *c;
    struct feed *f;
    struct props p, old;
    sd_bus_message *waiting;        // method call that waits for the receiver
    enum core_pend waiting_kind;
} S;

static void refresh(struct props *p, double now)
{
    struct core *c = S.c;
    const struct gnss_snap *s = &c->snap;

    memset(p, 0, sizeof(*p));
    p->fix = nmea_fix_name(s->fix);
    p->valid = s->valid;
    p->health = core_health_name(c->health);
    p->power = core_power_name(c->power);
    snprintf(p->antenna_buf, sizeof(p->antenna_buf), "%s", c->st.antenna);
    snprintf(p->version_buf, sizeof(p->version_buf), "%s%s%s", c->st.version,
             c->st.version[0] && c->st.release[0] ? ", " : "", c->st.release);
    snprintf(p->utc_buf, sizeof(p->utc_buf), "%s", s->utc);
    p->antenna = p->antenna_buf;
    p->version = p->version_buf;
    p->utc = p->utc_buf;
    p->time_valid = s->time_valid;
    p->device = c->cfg.device;
    p->pty = S.f->link;
    p->sock = S.f->sock;

    p->lat = s->lat;
    p->lon = s->lon;
    p->alt = s->alt;
    p->speed = s->speed;
    p->course = s->course;
    p->hdop = s->hdop;
    p->pdop = s->pdop;
    p->vdop = s->vdop;
    p->cn0_best = s->cn0_best;
    p->cn0_mean = s->cn0_mean;
    p->age = c->last_rx > 0 ? now - c->last_rx : NAN;
    p->used = s->used;
    p->view = s->view;
    p->tracked = s->tracked;
    p->interval = c->interval_ms;
    p->reopens = c->reopens;
    p->resets = c->resets;
    p->bad = c->st.bad;
    p->dropped = S.f->dropped;
    p->sentences = c->st.sentences;
    p->ts_mode = time_sync_mode_name(c->cfg.time_sync);
    snprintf(p->last_sync_buf, sizeof(p->last_sync_buf), "%s", c->ts.last_sync);
    p->last_sync = p->last_sync_buf;
    p->ts_offset = c->ts.offset;
}

// --- Getters, userdata points at the field ---

static int get_s(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "s", *(const char **)userdata);
}

static int get_b(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "b", *(int *)userdata);
}

static int get_u(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "u", *(uint32_t *)userdata);
}

static int get_d(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "d", *(double *)userdata);
}

static int get_t(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "t", *(uint64_t *)userdata);
}

static int append_per_system(sd_bus_message *reply, const uint32_t *n)
{
    int r = sd_bus_message_open_container(reply, 'a', "{su}");
    for (int i = 0; r >= 0 && i < SYS_N; i++)
        r = sd_bus_message_append(reply, "{su}", gnss_sys_name[i], n[i]);
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int get_used_sys(sd_bus *b, const char *path, const char *iface, const char *prop,
                        sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err, (void)userdata;
    return append_per_system(reply, S.c->snap.used_sys);
}

static int get_view_sys(sd_bus *b, const char *path, const char *iface, const char *prop,
                        sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err, (void)userdata;
    return append_per_system(reply, S.c->snap.view_sys);
}

static int get_sats(sd_bus *b, const char *path, const char *iface, const char *prop,
                    sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err, (void)userdata;
    const struct gnss_snap *s = &S.c->snap;
    int r = sd_bus_message_open_container(reply, 'a', "(sqnnd)");
    for (int i = 0; r >= 0 && i < s->nsats; i++)
        r = sd_bus_message_append(reply, "(sqnnd)", gnss_sys_name[s->sats[i].sys], s->sats[i].id,
                                  s->sats[i].elev, s->sats[i].azim, s->sats[i].cn0);
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

// --- Methods ---

static int reply_error(sd_bus_message *m, int r)
{
    switch (r)
    {
    case -EBUSY:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "Busy", "another command is waiting");
    case -EINVAL:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "InvalidArgument", "invalid argument");
    case -ENOTSUP:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "NotSupported", "no reset line configured");
    case -ETIMEDOUT:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "Timeout", "no answer from the receiver");
    case -EBADMSG:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "Rejected", "PMTK001 flag 0, invalid packet");
    case -EPROTONOSUPPORT:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "Rejected", "PMTK001 flag 1, unsupported packet");
    case -EIO:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "Failed", "PMTK001 flag 2, action failed");
    case -ENOTCONN:
        return sd_bus_reply_method_errorf(m, GNSS_ERROR "NotOpen", "receiver tty not open");
    }
    return sd_bus_reply_method_errnof(m, -r, "%s", strerror(-r));
}

// Keeps the call until core finishes the command (dbus_update).
static int wait_for(sd_bus_message *m, int r)
{
    if (r < 0)
        return reply_error(m, r);
    S.waiting = sd_bus_message_ref(m);
    S.waiting_kind = S.c->pend.kind;
    return 1;
}

static int root_only(sd_bus_message *m, sd_bus_error *err, const char *what);

static int method_standby(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    return wait_for(m, core_standby(S.c, core_now()));
}

static int method_wake(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    return wait_for(m, core_wake(S.c, core_now()));
}

static int method_restart(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    const char *mode;
    int r = sd_bus_message_read(m, "s", &mode);
    if (r < 0)
        return r;
    r = core_restart(S.c, mode, core_now());
    if (r < 0)
        return reply_error(m, r);
    dbus_update();
    return sd_bus_reply_method_return(m, "");
}

static int method_set_rate(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    uint32_t ms;
    int r = sd_bus_message_read(m, "u", &ms);
    if (r < 0)
        return r;
    return wait_for(m, core_set_rate(S.c, ms, core_now()));
}

static int method_send(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    const char *cmd;
    int r = sd_bus_message_read(m, "s", &cmd);
    if (r < 0)
        return r;
    if (!S.c->cfg.send_command)
        return sd_bus_error_set(err, GNSS_ERROR "Disabled", "send_command = off in gnssd.conf");
    if ((r = root_only(m, err, "SendCommand")) < 0)
        return r;
    return wait_for(m, core_raw(S.c, cmd, core_now()));
}

static int root_only(sd_bus_message *m, sd_bus_error *err, const char *what)
{
    sd_bus_creds *creds = NULL;
    uid_t uid = (uid_t)-1;
    int r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID | SD_BUS_CREDS_AUGMENT, &creds);
    if (r >= 0)
        r = sd_bus_creds_get_euid(creds, &uid);
    sd_bus_creds_unref(creds);
    if (r < 0 || uid != 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_ACCESS_DENIED, "%s is for root only", what);
    return 0;
}

static int method_sync_time(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    int r = root_only(m, err, "SyncTimeNow");
    if (r < 0)
        return r;
    int stepped = 0;
    double now = core_now();
    core_update(S.c, now);
    r = ts_sync_now(S.c, now, &stepped);
    if (r == -ENODATA)
        return sd_bus_error_set(err, GNSS_ERROR "NoFix",
                                "no stable GNSS time (3D fix, 4 satellites, 5 epochs that agree)");
    if (r == -EALREADY)
        return sd_bus_error_setf(err, GNSS_ERROR "NtpSynchronized",
                                 "NTP keeps the system clock and it is %+.3f s off GNSS",
                                 S.c->ts.offset);
    if (r < 0)
        return sd_bus_error_setf(err, GNSS_ERROR "Failed", "clock step: %s", strerror(-r));
    dbus_update();
    return sd_bus_reply_method_return(m, "db", S.c->ts.offset, stepped);
}

static void finish_waiting(void)
{
    struct core *c = S.c;
    if (!S.waiting || !c->pend.done)
        return;
    sd_bus_message *m = S.waiting;
    S.waiting = NULL;
    if (c->pend.result < 0)
    {
        reply_error(m, c->pend.result);
    }
    else if (S.waiting_kind == PEND_COLLECT)
    {
        sd_bus_message *reply = NULL;
        int r = sd_bus_message_new_method_return(m, &reply);
        if (r >= 0)
            r = sd_bus_message_open_container(reply, 'a', "s");
        for (int i = 0; r >= 0 && i < c->pend.n; i++)
            r = sd_bus_message_append(reply, "s", c->pend.lines[i]);
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
        if (r >= 0)
            sd_bus_send(NULL, reply, NULL);
        sd_bus_message_unref(reply);
    }
    else
    {
        sd_bus_reply_method_return(m, "");
    }
    sd_bus_message_unref(m);
    core_pend_clear(c);
}

#define EMITS SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE
#define P(name, sig, get, field, flags) SD_BUS_PROPERTY(name, sig, get, offsetof(struct props, field), flags)

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Standby", "", "", method_standby, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Wake", "", "", method_wake, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("Restart", "s", SD_BUS_PARAM(mode), "", , method_restart,
                             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("SetUpdateRate", "u", SD_BUS_PARAM(milliseconds), "", , method_set_rate,
                             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("SendCommand", "s", SD_BUS_PARAM(command), "as", SD_BUS_PARAM(replies),
                             method_send, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("SyncTimeNow", "", , "db", SD_BUS_PARAM(offset) SD_BUS_PARAM(stepped),
                             method_sync_time, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL_WITH_NAMES("FixChanged", "sb", SD_BUS_PARAM(fix) SD_BUS_PARAM(valid), 0),
    SD_BUS_SIGNAL_WITH_NAMES("PositionChanged", "ddd",
                             SD_BUS_PARAM(latitude) SD_BUS_PARAM(longitude) SD_BUS_PARAM(altitude), 0),

    P("Fix", "s", get_s, fix, EMITS),
    P("Valid", "b", get_b, valid, EMITS),
    P("Health", "s", get_s, health, EMITS),
    P("PowerState", "s", get_s, power, EMITS),
    P("AntennaStatus", "s", get_s, antenna, EMITS),
    P("ModuleVersion", "s", get_s, version, EMITS),
    P("TimeValid", "b", get_b, time_valid, EMITS),

    P("Latitude", "d", get_d, lat, 0),
    P("Longitude", "d", get_d, lon, 0),
    P("Altitude", "d", get_d, alt, 0),
    P("Speed", "d", get_d, speed, 0),
    P("Course", "d", get_d, course, 0),
    P("UtcTime", "s", get_s, utc, 0),
    P("SatellitesUsed", "u", get_u, used, 0),
    SD_BUS_PROPERTY("SatellitesUsedPerSystem", "a{su}", get_used_sys, 0, 0),
    P("SatellitesInView", "u", get_u, view, 0),
    SD_BUS_PROPERTY("SatellitesInViewPerSystem", "a{su}", get_view_sys, 0, 0),
    P("SatellitesTracked", "u", get_u, tracked, 0),
    SD_BUS_PROPERTY("Satellites", "a(sqnnd)", get_sats, 0, 0),
    P("Hdop", "d", get_d, hdop, 0),
    P("Pdop", "d", get_d, pdop, 0),
    P("Vdop", "d", get_d, vdop, 0),
    P("CnoBest", "d", get_d, cn0_best, 0),
    P("CnoMean", "d", get_d, cn0_mean, 0),
    P("DataAgeSec", "d", get_d, age, 0),
    P("UpdateIntervalMs", "u", get_u, interval, 0),
    P("Sentences", "t", get_t, sentences, 0),
    P("ChecksumErrors", "u", get_u, bad, 0),
    P("Reopens", "u", get_u, reopens, 0),
    P("Resets", "u", get_u, resets, 0),
    P("Device", "s", get_s, device, 0),
    P("NmeaPty", "s", get_s, pty, 0),
    P("NmeaSocket", "s", get_s, sock, 0),
    P("NmeaDropped", "u", get_u, dropped, 0),
    P("TimeSyncMode", "s", get_s, ts_mode, SD_BUS_VTABLE_PROPERTY_CONST),
    P("LastTimeSync", "s", get_s, last_sync, EMITS),
    P("TimeSyncOffsetSec", "d", get_d, ts_offset, 0),
    SD_BUS_VTABLE_END,
};

void dbus_update(void)
{
    const char *changed[12];
    int n = 0;

    if (!S.bus)
        return;
    struct core *c = S.c;
    double now = core_now();
    core_update(c, now);
    finish_waiting();
    S.old = S.p;
    refresh(&S.p, now);

#define CH(field, name) \
    if (S.old.field != S.p.field) \
        changed[n++] = name
#define CHS(field, name) \
    if (strcmp(S.old.field, S.p.field)) \
        changed[n++] = name
    CH(fix, "Fix");
    CH(valid, "Valid");
    CH(health, "Health");
    CH(power, "PowerState");
    CHS(antenna_buf, "AntennaStatus");
    CHS(version_buf, "ModuleVersion");
    CH(time_valid, "TimeValid");
    CHS(last_sync_buf, "LastTimeSync");
#undef CH
#undef CHS
    changed[n] = NULL;
    if (n)
        sd_bus_emit_properties_changed_strv(S.bus, GNSS_PATH, GNSS_IFACE, (char **)changed);

    if (c->fix_event)
    {
        sd_bus_emit_signal(S.bus, GNSS_PATH, GNSS_IFACE, "FixChanged", "sb", S.p.fix, S.p.valid);
        c->fix_event = 0;
    }
    if (c->pos_event)
    {
        sd_bus_emit_signal(S.bus, GNSS_PATH, GNSS_IFACE, "PositionChanged", "ddd", S.p.lat, S.p.lon,
                           S.p.alt);
        c->pos_event = 0;
    }
}

int dbus_start(sd_bus *bus, struct core *c, struct feed *f)
{
    S.bus = bus;
    S.c = c;
    S.f = f;
    core_update(c, core_now());
    c->fix_event = c->pos_event = 0;
    refresh(&S.p, core_now());
    int r = sd_bus_add_object_vtable(bus, NULL, GNSS_PATH, GNSS_IFACE, vtable, &S.p);
    if (r < 0)
        return r;
    return sd_bus_request_name(bus, GNSS_BUS_NAME, 0);
}

void dbus_stop(void)
{
    if (S.waiting)
    {
        sd_bus_reply_method_errorf(S.waiting, GNSS_ERROR "Failed", "daemon stops");
        sd_bus_message_unref(S.waiting);
        S.waiting = NULL;
    }
    if (S.bus)
        sd_bus_release_name(S.bus, GNSS_BUS_NAME);
    S.bus = NULL;
}
