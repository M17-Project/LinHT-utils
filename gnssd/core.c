// linht-gnssd: receiver state, commands and the NMEA watchdog

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"

int core_log_quiet;

// stderr with a priority prefix, which journald understands.
void core_log(int prio, const char *fmt, ...)
{
    if (core_log_quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "<%d>", prio);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

double core_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

const char *core_health_name(enum core_health h)
{
    static const char *const n[] = { "starting", "ok", "no-data", "tty-busy", "tty-error", "standby" };
    return n[h];
}

const char *core_power_name(enum core_power p)
{
    return p == P_STANDBY ? "standby" : "on";
}

void core_init(struct core *c, const struct core_ops *ops, void *ctx)
{
    struct gnssd_config cfg = c->cfg;
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    c->ops = ops;
    c->ctx = ctx;
    nmea_init(&c->st);
    c->last_utc_s = -1;
    c->fix_prev = -1;
    c->valid_prev = -1;
    c->last_pos = -1e9;
    nmea_snapshot(&c->st, 1, c->cfg.stale_s, &c->snap);
}

double core_stale(const struct core *c)
{
    double s = 2.5 * c->interval_ms / 1000.0;
    return s > c->cfg.stale_s ? s : c->cfg.stale_s;
}

static void set_health(struct core *c, enum core_health h)
{
    c->health = h;
}

static int send_cmd(struct core *c, const char *cmd)
{
    char buf[NMEA_MAX + 8];
    int n = nmea_format(buf, sizeof(buf), cmd);
    if (n < 0)
        return -EINVAL;
    if (!c->open)
        return -ENOTCONN;
    int r = c->ops->write(c->ctx, buf, (size_t)n);
    if (r < 0)
    {
        core_log(4, "write %.*s: %s", n - 2, buf, strerror(-r));
        return -ENOTCONN;
    }
    return 0;
}

// --- Link ---

static void try_open(struct core *c, double now)
{
    int r = c->ops->open(c->ctx);
    if (r < 0)
    {
        if (r != c->open_err)
        {
            if (r == -EWOULDBLOCK)
                core_log(4, "%s is locked by another reader (gnss-status, field-log), retrying",
                         c->cfg.device);
            else
                core_log(3, "%s: %s, retrying", c->cfg.device, strerror(-r));
        }
        c->open_err = r;
        set_health(c, r == -EWOULDBLOCK ? H_TTY_BUSY : H_TTY_ERROR);
        c->next_open = now + CORE_RETRY_S;
        return;
    }
    if (c->open_err)
        core_log(6, "%s open", c->cfg.device);
    c->open_err = 0;
    c->open = 1;
    c->link_since = now;
    c->len = 0;
    c->skipping = 0;
    if (c->health != H_NO_DATA)
        set_health(c, H_STARTING);
    // Any byte also ends the standby of the module, so a new start always
    // finds it running.
    c->power = P_ON;
    send_cmd(c, "PQVERNO,R");
    send_cmd(c, "PMTK605");
}

static void close_link(struct core *c)
{
    if (c->open)
        c->ops->close(c->ctx);
    c->open = 0;
}

void core_link_lost(struct core *c, double now, int err)
{
    core_log(3, "%s: %s, opening again", c->cfg.device, err ? strerror(-err) : "hangup");
    close_link(c);
    c->reopens++;
    c->next_open = now + 1;
}

// --- Commands ---

static int pend_start(struct core *c, enum core_pend kind, int cmd, double timeout, double now)
{
    c->pend.kind = kind;
    c->pend.cmd = cmd;
    c->pend.ack_seq = c->st.ack_seq;
    c->pend.deadline = now + timeout;
    c->pend.done = 0;
    c->pend.result = 0;
    c->pend.n = 0;
    return 0;
}

static void pend_finish(struct core *c, int result)
{
    c->pend.done = 1;
    c->pend.result = result;
}

void core_pend_clear(struct core *c)
{
    c->pend.kind = PEND_NONE;
    c->pend.done = 0;
}

static int busy(const struct core *c)
{
    return c->pend.kind != PEND_NONE;
}

int core_standby(struct core *c, double now)
{
    if (busy(c))
        return -EBUSY;
    int r = send_cmd(c, "PMTK161,0");
    if (r < 0)
        return r;
    return pend_start(c, PEND_ACK, 161, CORE_ACK_S, now);
}

int core_wake(struct core *c, double now)
{
    if (busy(c))
        return -EBUSY;
    // The specification: "Sending any data will make the modules exit
    // Standby mode". The version query also refreshes ModuleVersion.
    int r = send_cmd(c, "PQVERNO,R");
    if (r < 0)
        return r;
    if (c->power == P_STANDBY)
        core_log(6, "wake");
    c->power = P_ON;
    c->link_since = now;
    if (c->health == H_STANDBY)
        set_health(c, H_STARTING);
    return pend_start(c, PEND_DATA, 0, CORE_WAKE_S, now);
}

int core_restart(struct core *c, const char *mode, double now)
{
    static const struct { const char *name, *cmd; } modes[] = {
        { "hot", "PMTK101" }, { "warm", "PMTK102" }, { "cold", "PMTK103" }, { "full-cold", "PMTK104" },
    };
    if (busy(c))
        return -EBUSY;
    int r = -EINVAL;
    if (!strcmp(mode, "reset"))
    {
        r = c->ops->reset(c->ctx);
        if (r == 0)
            c->resets++;
    }
    else
    {
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
            if (!strcmp(mode, modes[i].name))
                r = send_cmd(c, modes[i].cmd);
    }
    if (r < 0)
        return r;
    core_log(6, "restart %s", mode);
    // No ACK for restarts. The module runs again after a restart.
    c->power = P_ON;
    c->link_since = now;
    c->last_utc_s = -1;
    if (c->health == H_STANDBY)
        set_health(c, H_STARTING);
    return 0;
}

int core_set_rate(struct core *c, unsigned ms, double now)
{
    // PMTK220 takes 100 to 10000 ms, above 1000 only whole seconds. At
    // 9600 baud one epoch of the default sentences takes up to about 1 s on
    // the wire, so faster rates are refused.
    if (ms < 1000 || ms > 10000 || ms % 1000)
        return -EINVAL;
    if (busy(c))
        return -EBUSY;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "PMTK220,%u", ms);
    int r = send_cmd(c, cmd);
    if (r < 0)
        return r;
    return pend_start(c, PEND_ACK, 220, CORE_ACK_S, now);
}

int core_raw(struct core *c, const char *cmd, double now)
{
    if (busy(c))
        return -EBUSY;
    int r = send_cmd(c, cmd);
    if (r < 0)
        return r;
    core_log(5, "raw command %s", cmd);
    return pend_start(c, PEND_COLLECT, 0, CORE_COLLECT_S, now);
}

static int ack_errno(int flag)
{
    switch (flag)
    {
    case 3:
        return 0;
    case 1:
        return -EPROTONOSUPPORT;    // unsupported packet type
    case 2:
        return -EIO;                // valid packet, action failed
    default:
        return -EBADMSG;            // invalid packet
    }
}

// --- Input ---

static double utc_seconds(const char *t)
{
    if (strlen(t) < 6)
        return -1;
    for (int i = 0; i < 6; i++)
        if (t[i] < '0' || t[i] > '9')
            return -1;
    double frac = t[6] == '.' ? atof(t + 6) : 0;
    return ((t[0] - '0') * 10 + t[1] - '0') * 3600.0 + ((t[2] - '0') * 10 + t[3] - '0') * 60.0 +
           (t[4] - '0') * 10 + t[5] - '0' + frac;
}

static void on_line(struct core *c, const char *line, double now)
{
    enum nmea_kind k = nmea_parse(&c->st, line, now);
    if (k == NMEA_BAD)
        return;

    if (c->ops->forward)
    {
        char out[NMEA_MAX + 4];
        int n = snprintf(out, sizeof(out), "%s\r\n", line);
        if (n > 0 && (size_t)n < sizeof(out))
            c->ops->forward(c->ctx, out, (size_t)n);
    }

    if (c->health == H_NO_DATA)
        core_log(5, "NMEA again after %.0f s", now - (c->last_rx > 0 ? c->last_rx : c->link_since));
    else if (c->health == H_STARTING && c->last_rx == 0)
        core_log(6, "receiving NMEA");
    c->last_rx = now;
    c->stalls = 0;

    int standard = k >= NMEA_RMC && k <= NMEA_TXT;
    if (c->power == P_STANDBY && standard && now - c->standby_at > CORE_STANDBY_S)
    {
        core_log(5, "NMEA in standby, the module woke up");
        c->power = P_ON;
    }
    set_health(c, c->power == P_STANDBY ? H_STANDBY : H_OK);

    if (k == NMEA_RMC)
    {
        double t = utc_seconds(c->st.rmc_time);
        if (t >= 0 && c->last_utc_s >= 0)
        {
            double d = t - c->last_utc_s;
            if (d < 0)
                d += 86400;
            // A new interval counts after two equal steps in a row, so a
            // gap in the output (standby, restart) does not change it.
            uint32_t ms = d > 0.05 && d <= 60 ? (uint32_t)lround(d * 1000) : 0;
            if (ms && ms + 50 >= c->last_step_ms && ms <= c->last_step_ms + 50)
                c->interval_ms = ms;
            c->last_step_ms = ms;
        }
        c->last_utc_s = t;
    }
    if (k == NMEA_PROPRIETARY && c->st.startup_seq != c->startup_seen)
    {
        c->startup_seen = c->st.startup_seq;
        core_log(6, "module start-up message");
    }

    if (c->pend.kind == PEND_NONE || c->pend.done)
        return;
    switch (c->pend.kind)
    {
    case PEND_ACK:
        if (k == NMEA_ACK && c->st.ack_cmd == c->pend.cmd)
        {
            int r = ack_errno(c->st.ack_flag);
            if (r == 0 && c->pend.cmd == 161)
            {
                c->power = P_STANDBY;
                c->standby_at = now;
                set_health(c, H_STANDBY);
                core_log(6, "standby");
            }
            if (r == 0 && c->pend.cmd == 220)
                c->last_utc_s = -1;
            pend_finish(c, r);
        }
        break;
    case PEND_DATA:
        if (k == NMEA_RMC || k == NMEA_GGA)
            pend_finish(c, 0);
        break;
    case PEND_COLLECT:
        if ((k == NMEA_ACK || k == NMEA_PROPRIETARY) && c->pend.n < CORE_COLLECT_MAX)
            snprintf(c->pend.lines[c->pend.n++], sizeof(c->pend.lines[0]), "%s", line);
        break;
    case PEND_NONE:
        break;
    }
}

void core_feed(struct core *c, const char *data, size_t n, double now)
{
    for (size_t i = 0; i < n; i++)
    {
        char ch = data[i];
        if (ch == '$')
        {
            // A new sentence, also in the middle of a broken one.
            c->len = 0;
            c->skipping = 0;
        }
        if (ch == '\n' || ch == '\r')
        {
            if (c->len && !c->skipping)
            {
                c->line[c->len] = 0;
                if (c->line[0] == '$')
                    on_line(c, c->line, now);
            }
            c->len = 0;
            c->skipping = 0;
            continue;
        }
        if (c->skipping)
            continue;
        if (c->len >= NMEA_MAX)
        {
            c->skipping = 1;
            c->st.bad++;
            continue;
        }
        c->line[c->len++] = ch;
    }
}

// --- Timers ---

void core_tick(struct core *c, double now)
{
    if (c->pend.kind != PEND_NONE && !c->pend.done && now >= c->pend.deadline)
        pend_finish(c, c->pend.kind == PEND_COLLECT ? 0 : -ETIMEDOUT);

    if (!c->open)
    {
        if (now >= c->next_open)
            try_open(c, now);
        return;
    }
    if (c->power == P_STANDBY)
        return;

    double ref = c->last_rx > c->link_since ? c->last_rx : c->link_since;
    double limit = c->cfg.watchdog_s;
    if (c->interval_ms && 2.5 * c->interval_ms / 1000.0 > limit)
        limit = 2.5 * c->interval_ms / 1000.0;
    if (now - ref <= limit)
        return;

    c->stalls++;
    set_health(c, H_NO_DATA);
    core_log(4, "no NMEA for %.0f s, opening %s again (%u)", now - ref, c->cfg.device, c->stalls);
    if (c->cfg.watchdog_reset && c->stalls >= c->cfg.reset_after_reopens &&
        (c->resets == 0 || now - c->last_reset >= c->cfg.reset_min_interval_s))
    {
        int r = c->ops->reset(c->ctx);
        if (r == 0)
        {
            c->resets++;
            c->last_reset = now;
            core_log(4, "RESET_N pulse after %u reopens without NMEA", c->stalls);
        }
        else if (r != -ENOTSUP)
        {
            core_log(3, "RESET_N pulse: %s", strerror(-r));
        }
    }
    close_link(c);
    c->reopens++;
    try_open(c, now);
}

void core_update(struct core *c, double now)
{
    nmea_snapshot(&c->st, now, core_stale(c), &c->snap);
    if (c->snap.fix != c->fix_prev || c->snap.valid != c->valid_prev)
    {
        if (c->fix_prev >= 0)
            core_log(6, "fix %s (%u used, %u in view)", nmea_fix_name(c->snap.fix), c->snap.used,
                     c->snap.view);
        c->fix_prev = c->snap.fix;
        c->valid_prev = c->snap.valid;
        c->fix_event = 1;
    }
    if (c->snap.valid && now - c->last_pos >= c->cfg.position_interval_s)
    {
        c->last_pos = now;
        c->pos_event = 1;
    }
}
