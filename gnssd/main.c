// linht-gnssd: Quectel LG77L GNSS daemon for LinHT
//
//   linht-gnssd [-c FILE]            the daemon (systemd unit)
//   linht-gnssd status [--json]      one GetAll, as text or one JSON line
//   linht-gnssd standby | wake       receiver standby (PMTK161) and wake up
//   linht-gnssd restart MODE         hot, warm, cold, full-cold or reset
//   linht-gnssd rate MS              fix interval, 1000 to 10000 ms
//   linht-gnssd send COMMAND         raw command, root and send_command = on

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "core.h"
#include "dbus.h"
#include "feed.h"
#include "tty.h"

#define TICK_US 500000ULL

static struct core core;
static struct feed feed;
static sd_event *event;
static int tty_fd = -1;
static sd_event_source *tty_src;

static void usage(FILE *f)
{
    fprintf(f,
            "usage: linht-gnssd [-c FILE]          run the daemon\n"
            "       linht-gnssd status [--json]    print the daemon properties\n"
            "       linht-gnssd standby            receiver standby (PMTK161,0)\n"
            "       linht-gnssd wake               end the standby\n"
            "       linht-gnssd restart MODE       hot, warm, cold, full-cold or reset (RESET_N)\n"
            "       linht-gnssd rate MS            fix interval, 1000 to 10000 ms in whole seconds\n"
            "       linht-gnssd send COMMAND       raw command, for example PMTK414 (root only)\n");
}

// --- Receiver I/O for core ---

static int on_tty(sd_event_source *s, int fd, uint32_t revents, void *userdata);

static void tty_close(void *ctx)
{
    (void)ctx;
    tty_src = sd_event_source_unref(tty_src);
    if (tty_fd >= 0)
        close(tty_fd);
    tty_fd = -1;
}

static int tty_open_cb(void *ctx)
{
    (void)ctx;
    int fd = tty_open(core.cfg.device, core.cfg.baud);
    if (fd < 0)
        return fd;
    int r = sd_event_add_io(event, &tty_src, fd, EPOLLIN, on_tty, NULL);
    if (r < 0)
    {
        close(fd);
        return r;
    }
    tty_fd = fd;
    return 0;
}

static int tty_write(void *ctx, const char *buf, size_t n)
{
    (void)ctx;
    ssize_t w = write(tty_fd, buf, n);
    if (w < 0)
        return -errno;
    return (size_t)w == n ? 0 : -EAGAIN;
}

static int tty_reset(void *ctx)
{
    (void)ctx;
    return gpio_pulse_low(core.cfg.reset_chip, core.cfg.reset_line, core.cfg.reset_ms);
}

static void forward(void *ctx, const char *line, size_t n)
{
    (void)ctx;
    feed_write(&feed, line, n);
    for (int i = 0; i < FEED_CLIENTS; i++)
        if (feed.dead[i])
        {
            sd_event_source_unref(feed.client_src[i]);
            feed_close_client(&feed, i);
        }
}

static const struct core_ops ops = {
    .open = tty_open_cb,
    .close = tty_close,
    .write = tty_write,
    .reset = tty_reset,
    .forward = forward,
};

static int on_tty(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    (void)s, (void)userdata;
    char buf[1024];
    double now = core_now();
    for (;;)
    {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0)
        {
            core_feed(&core, buf, (size_t)r, now);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno != EAGAIN)
        {
            core_link_lost(&core, now, -errno);
            break;
        }
        if (revents & (EPOLLHUP | EPOLLERR))
            core_link_lost(&core, now, 0);
        break;
    }
    dbus_update();
    return 0;
}

// --- Pass-through sources ---

static int on_pty(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    (void)s, (void)fd, (void)revents, (void)userdata;
    feed_drain_pty(&feed);
    return 0;
}

static int on_client(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    (void)fd, (void)revents;
    int slot = (int)(intptr_t)userdata;
    if (feed_client_input(&feed, slot))
    {
        sd_event_source_unref(s);
        feed_close_client(&feed, slot);
    }
    return 0;
}

static int on_listen(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    (void)s, (void)fd, (void)revents, (void)userdata;
    int slot;
    while ((slot = feed_accept(&feed)) >= 0)
    {
        sd_event_source *src = NULL;
        if (sd_event_add_io(event, &src, feed.clients[slot], EPOLLIN, on_client,
                            (void *)(intptr_t)slot) < 0)
        {
            feed_close_client(&feed, slot);
            continue;
        }
        feed.client_src[slot] = src;
    }
    return 0;
}

// --- Daemon ---

static int on_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
    (void)userdata;
    core_tick(&core, core_now());
    dbus_update();
    int r = sd_event_source_set_time(s, usec + TICK_US);
    return r < 0 ? r : sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
}

static int daemon_run(void)
{
    sd_bus *bus = NULL;
    sd_event_source *timer = NULL, *pty_src = NULL, *listen_src = NULL;
    int r;

    core_init(&core, &ops, NULL);
    feed_init(&feed);

    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    signal(SIGPIPE, SIG_IGN);

    r = sd_event_default(&event);
    if (r >= 0)
        r = sd_event_add_signal(event, NULL, SIGTERM, NULL, NULL);
    if (r >= 0)
        r = sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);
    if (r >= 0)
    {
        feed_open(&feed, core.cfg.pty_link, core.cfg.socket_path, core.cfg.feed_group);
        if (feed.master >= 0)
            sd_event_add_io(event, &pty_src, feed.master, EPOLLIN, on_pty, NULL);
        if (feed.listen >= 0)
            sd_event_add_io(event, &listen_src, feed.listen, EPOLLIN, on_listen, NULL);
        if (feed.link[0])
            core_log(6, "NMEA pass-through %s -> %s", feed.link, feed.slave_name);
    }
    if (r >= 0)
    {
        uint64_t t0;
        sd_event_now(event, CLOCK_MONOTONIC, &t0);
        r = sd_event_add_time(event, &timer, CLOCK_MONOTONIC, t0 + TICK_US, 50000, on_timer, NULL);
    }
    // The tty first, so the first clients already see the receiver state.
    if (r >= 0)
        core_tick(&core, core_now());
    if (r >= 0)
        r = sd_bus_open_system(&bus);
    if (r >= 0)
        r = sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
    if (r >= 0)
    {
        r = dbus_start(bus, &core, &feed);
        if (r < 0)
            core_log(3, "cannot own %s: %s", GNSS_BUS_NAME, strerror(-r));
    }
    if (r >= 0)
    {
        core_log(6, "running, bus name %s, %s at %u baud", GNSS_BUS_NAME, core.cfg.device,
                 core.cfg.baud);
        r = sd_event_loop(event);
    }
    else
    {
        core_log(3, "start failed: %s", strerror(-r));
    }

    dbus_stop();
    if (bus)
        sd_bus_flush_close_unref(bus);
    for (int i = 0; i < FEED_CLIENTS; i++)
        sd_event_source_unref(feed.client_src[i]);
    sd_event_source_unref(pty_src);
    sd_event_source_unref(listen_src);
    feed_close(&feed);
    tty_close(NULL);
    sd_event_source_unref(timer);
    sd_event_unref(event);
    core_log(6, "stopped");
    return r < 0;
}

// --- Client modes ---

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; s++)
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    putchar('"');
}

static int print_value(sd_bus_message *m, int json);

// Arrays, dictionaries and structures. JSON: a{..} is an object, a
// structure an array. Text: elements separated by spaces, dictionary
// entries as key=value, structure fields joined with '/'.
static int print_container(sd_bus_message *m, char type, const char *contents, int json)
{
    int r = sd_bus_message_enter_container(m, type, contents);
    if (r < 0)
        return r;
    int dict = type == 'a' && contents[0] == '{';
    if (json)
        putchar(dict ? '{' : '[');
    for (int i = 0;; i++)
    {
        if ((r = sd_bus_message_at_end(m, 0)) != 0)
            break;
        if (i)
            printf(json ? "," : type == 'r' ? "/" : " ");
        if (type == 'e' || (type == 'a' && contents[0] == '{'))
        {
            char t;
            const char *c;
            if (dict)
            {
                sd_bus_message_peek_type(m, &t, &c);
                sd_bus_message_enter_container(m, 'e', c);
            }
            r = print_value(m, json);
            printf(json ? ":" : "=");
            if (r >= 0)
                r = print_value(m, json);
            if (dict)
                sd_bus_message_exit_container(m);
        }
        else
        {
            r = print_value(m, json);
        }
        if (r < 0)
            return r;
    }
    if (json)
        putchar(dict ? '}' : ']');
    return sd_bus_message_exit_container(m);
}

static int print_value(sd_bus_message *m, int json)
{
    char type;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r <= 0)
        return r < 0 ? r : -EINVAL;
    union { const char *s; int b; uint16_t q; int16_t n; uint32_t u; double d; int64_t x; uint64_t t; } v;

    switch (type)
    {
    case 'v':
    case 'a':
    case 'r':
        if (type == 'v')
        {
            if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0)
                return r;
            r = print_value(m, json);
            return r < 0 ? r : sd_bus_message_exit_container(m);
        }
        return print_container(m, type, contents, json);
    case 's':
        sd_bus_message_read(m, "s", &v.s);
        if (json)
            json_string(v.s);
        else
            printf("%s", v.s);
        return 0;
    case 'b':
        sd_bus_message_read(m, "b", &v.b);
        printf("%s", v.b ? "true" : "false");
        return 0;
    case 'q':
        sd_bus_message_read(m, "q", &v.q);
        printf("%u", v.q);
        return 0;
    case 'n':
        sd_bus_message_read(m, "n", &v.n);
        printf("%d", v.n);
        return 0;
    case 'u':
        sd_bus_message_read(m, "u", &v.u);
        printf("%u", v.u);
        return 0;
    case 'x':
        sd_bus_message_read(m, "x", &v.x);
        printf("%lld", (long long)v.x);
        return 0;
    case 't':
        sd_bus_message_read(m, "t", &v.t);
        printf("%llu", (unsigned long long)v.t);
        return 0;
    case 'd':
        sd_bus_message_read(m, "d", &v.d);
        if (isnan(v.d))
            printf(json ? "null" : "nan");
        else
            printf("%.9g", v.d);
        return 0;
    }
    char sig[2] = { type, 0 };
    sd_bus_message_skip(m, sig);
    printf(json ? "null" : "?");
    return 0;
}

static int status(int json)
{
    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_open_system(&bus);
    if (r >= 0)
        r = sd_bus_call_method(bus, GNSS_BUS_NAME, GNSS_PATH, "org.freedesktop.DBus.Properties",
                               "GetAll", &err, &reply, "s", GNSS_IFACE);
    if (r < 0)
    {
        fprintf(stderr, "linht-gnssd: %s\n", err.message ? err.message : strerror(-r));
        if (json)
            printf("{\"error\":\"daemon not available\"}\n");
        sd_bus_error_free(&err);
        sd_bus_unref(bus);
        return 1;
    }

    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (json)
        printf("{");
    for (int i = 0; r >= 0; i++)
    {
        const char *name;
        r = sd_bus_message_enter_container(reply, 'e', "sv");
        if (r <= 0)
            break;
        sd_bus_message_read(reply, "s", &name);
        if (json)
            printf("%s\"%s\":", i ? "," : "", name);
        else
            printf("%-26s ", name);
        r = print_value(reply, json);
        if (!json)
            putchar('\n');
        if (r >= 0)
            r = sd_bus_message_exit_container(reply);
    }
    if (json)
        printf("}\n");
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return r < 0;
}

static int call(const char *method, const char *sig, const char *arg, uint32_t num)
{
    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_open_system(&bus);
    if (r >= 0)
    {
        if (!sig[0])
            r = sd_bus_call_method(bus, GNSS_BUS_NAME, GNSS_PATH, GNSS_IFACE, method, &err, &reply, "");
        else if (sig[0] == 'u')
            r = sd_bus_call_method(bus, GNSS_BUS_NAME, GNSS_PATH, GNSS_IFACE, method, &err, &reply,
                                   "u", num);
        else
            r = sd_bus_call_method(bus, GNSS_BUS_NAME, GNSS_PATH, GNSS_IFACE, method, &err, &reply,
                                   "s", arg);
    }
    if (r < 0)
    {
        fprintf(stderr, "linht-gnssd: %s: %s\n", method,
                err.message ? err.message : strerror(-r));
    }
    else if (!strcmp(method, "SendCommand"))
    {
        const char *line;
        sd_bus_message_enter_container(reply, 'a', "s");
        while (sd_bus_message_read(reply, "s", &line) > 0)
            printf("%s\n", line);
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return r < 0;
}

int main(int argc, char **argv)
{
    const char *conf = GNSSD_CONFIG_PATH;
    int i = 1;

    for (; i < argc && argv[i][0] == '-'; i++)
    {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            conf = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            usage(stdout);
            return 0;
        }
        else
        {
            usage(stderr);
            return 2;
        }
    }

    if (i < argc)
    {
        const char *cmd = argv[i], *arg = i + 1 < argc ? argv[i + 1] : NULL;
        int nargs = argc - i - 1;
        if (!strcmp(cmd, "status") && (nargs == 0 || (nargs == 1 && !strcmp(arg, "--json"))))
            return status(nargs == 1);
        if (!strcmp(cmd, "standby") && nargs == 0)
            return call("Standby", "", NULL, 0);
        if (!strcmp(cmd, "wake") && nargs == 0)
            return call("Wake", "", NULL, 0);
        if (!strcmp(cmd, "restart") && nargs == 1)
            return call("Restart", "s", arg, 0);
        if (!strcmp(cmd, "send") && nargs == 1)
            return call("SendCommand", "s", arg, 0);
        if (!strcmp(cmd, "rate") && nargs == 1)
        {
            char *end;
            unsigned long ms = strtoul(arg, &end, 10);
            if (!*end && ms <= 100000)
                return call("SetUpdateRate", "u", NULL, (uint32_t)ms);
        }
        usage(stderr);
        return 2;
    }

    config_defaults(&core.cfg);
    int rc = config_load(&core.cfg, conf);
    if (rc < 0)
        return 1;
    if (rc > 0)
        core_log(4, "%s not found, using defaults", conf);
    return daemon_run();
}
