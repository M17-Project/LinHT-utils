// linht-chargerd: BQ25792 charger daemon for LinHT
//
//   linht-chargerd [-c FILE]          the daemon (systemd unit)
//   linht-chargerd --restore          stock charger values, no D-Bus
//   linht-chargerd status [--json]    one GetAll, as text or one JSON line
//   linht-chargerd quiet SECONDS      holds a quiet lease (0 = until Ctrl-C)

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "core.h"
#include "dbus.h"

#define POLL_US 1000000ULL

static struct core core;

static void usage(FILE *f)
{
    fprintf(f,
            "usage: linht-chargerd [-c FILE]        run the daemon\n"
            "       linht-chargerd [-c FILE] --restore  stock charger values, then exit\n"
            "       linht-chargerd status [--json]  print the daemon properties\n"
            "       linht-chargerd quiet SECONDS    hold a quiet lease, 0 = until Ctrl-C\n");
}

static int open_chip(void)
{
    int rc = bq_open(&core.io, core.cfg.i2c_dev, core.cfg.i2c_addr);
    if (rc == -EWOULDBLOCK)
        core_log(3, "%s is locked by another charger tool", core.cfg.i2c_dev);
    else if (rc == -EBUSY)
        core_log(3, "a kernel driver owns 0x%02x on %s, refusing to start", core.cfg.i2c_addr,
                 core.cfg.i2c_dev);
    else if (rc < 0)
        core_log(3, "%s: %s", core.cfg.i2c_dev, strerror(-rc));
    return rc;
}

static void log_readback(const uint8_t rb[5])
{
    core_log(6, "stock values: REG0F 0x%02x REG10 0x%02x REG12 0x%02x REG14 0x%02x REG2E 0x%02x"
             " (expected 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x, REG10 bit 3 reads 0)",
             rb[0], rb[1], rb[2], rb[3], rb[4], BQ_STOCK_REG0F, BQ_STOCK_REG10, BQ_STOCK_REG12,
             BQ_STOCK_REG14, BQ_STOCK_REG2E);
}

static int restore(void)
{
    uint8_t rb[5];
    if (open_chip() < 0)
        return 1;
    int rc = bq_restore(&core.io, rb);
    if (rc < 0)
        core_log(3, "restore: %s", strerror(-rc));
    log_readback(rb);
    bq_close(&core.io);
    return rc < 0;
}

// --- Daemon ---

static int on_timer(sd_event_source *s, uint64_t usec, void *userdata)
{
    (void)userdata;
    core_poll(&core, core_now(), (double)time(NULL));
    dbus_update();
    int r = sd_event_source_set_time(s, usec + POLL_US);
    return r < 0 ? r : sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
}

static int daemon_run(void)
{
    sd_event *ev = NULL;
    sd_bus *bus = NULL;
    sd_event_source *timer = NULL;
    uint8_t rb[5];
    int r;

    if (open_chip() < 0)
        return 1;
    r = core_init(&core, core_now());
    if (r < 0)
    {
        core_log(3, "charger init failed: %s", strerror(-r));
        bq_close(&core.io);
        return 1;
    }

    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, NULL);

    r = sd_event_default(&ev);
    if (r >= 0)
        r = sd_event_add_signal(ev, NULL, SIGTERM, NULL, NULL);
    if (r >= 0)
        r = sd_event_add_signal(ev, NULL, SIGINT, NULL, NULL);
    if (r >= 0)
    {
        uint64_t t0;
        sd_event_now(ev, CLOCK_MONOTONIC, &t0);
        r = sd_event_add_time(ev, &timer, CLOCK_MONOTONIC, t0 + POLL_US, 50000, on_timer, NULL);
    }
    if (r >= 0)
        r = sd_bus_open_system(&bus);
    if (r >= 0)
        r = sd_bus_attach_event(bus, ev, SD_EVENT_PRIORITY_NORMAL);
    // The first poll before the name is claimed, so clients see real values.
    if (r >= 0)
    {
        core_poll(&core, core_now(), (double)time(NULL));
        r = dbus_start(bus, &core);
        if (r < 0)
            core_log(3, "cannot own %s: %s", CHARGER_BUS_NAME, strerror(-r));
    }
    if (r >= 0)
    {
        core_log(6, "running, bus name %s", CHARGER_BUS_NAME);
        r = sd_event_loop(ev);
    }
    else
    {
        core_log(3, "start failed: %s", strerror(-r));
    }

    // Clean stop: bus name first, then the stock values.
    dbus_stop();
    if (bus)
        sd_bus_flush_close_unref(bus);
    sd_event_source_unref(timer);
    sd_event_unref(ev);
    int rc = core_stop(&core, rb);
    if (rc < 0)
        core_log(3, "restore at stop: %s", strerror(-rc));
    log_readback(rb);
    bq_close(&core.io);
    return r < 0 || rc < 0;
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

static int print_variant(sd_bus_message *m, int json)
{
    char type;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0)
        return r;
    if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0)
        return r;
    union { const char *s; int b; uint32_t u; double d; int64_t x; uint64_t t; } v;

    switch (contents[0])
    {
    case 's':
        sd_bus_message_read(m, "s", &v.s);
        if (json)
            json_string(v.s);
        else
            printf("%s", v.s);
        break;
    case 'b':
        sd_bus_message_read(m, "b", &v.b);
        printf("%s", v.b ? "true" : "false");
        break;
    case 'u':
        sd_bus_message_read(m, "u", &v.u);
        printf("%u", v.u);
        break;
    case 'x':
        sd_bus_message_read(m, "x", &v.x);
        printf("%lld", (long long)v.x);
        break;
    case 't':
        sd_bus_message_read(m, "t", &v.t);
        printf("%llu", (unsigned long long)v.t);
        break;
    case 'd':
        sd_bus_message_read(m, "d", &v.d);
        if (isnan(v.d))
            printf(json ? "null" : "nan");
        else
            printf("%.4g", v.d);
        break;
    case 'a':
    {
        sd_bus_message_enter_container(m, 'a', "s");
        printf(json ? "[" : "");
        for (int i = 0; sd_bus_message_read(m, "s", &v.s) > 0; i++)
        {
            if (json)
            {
                printf(i ? "," : "");
                json_string(v.s);
            }
            else
            {
                printf("%s%s", i ? " " : "", v.s);
            }
        }
        printf(json ? "]" : "");
        sd_bus_message_exit_container(m);
        break;
    }
    default:
        sd_bus_message_skip(m, contents);
        printf(json ? "null" : "?");
    }
    return sd_bus_message_exit_container(m);
}

static int status(int json)
{
    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_open_system(&bus);
    if (r >= 0)
        r = sd_bus_call_method(bus, CHARGER_BUS_NAME, CHARGER_PATH, "org.freedesktop.DBus.Properties",
                               "GetAll", &err, &reply, "s", CHARGER_IFACE);
    if (r < 0)
    {
        fprintf(stderr, "linht-chargerd: %s\n", err.message ? err.message : strerror(-r));
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
            printf("%-22s ", name);
        r = print_variant(reply, json);
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

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int quiet(long total)
{
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    if (r < 0)
    {
        fprintf(stderr, "linht-chargerd: system bus: %s\n", strerror(-r));
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    double end = total > 0 ? core_now() + total : INFINITY;
    double next = 0;
    int rc = 0;
    while (!stop)
    {
        double now = core_now();
        if (now >= end)
            break;
        if (now >= next)
        {
            // Renew every 60 s with 120 s, the lease never runs out between.
            double left = end - now;
            uint32_t ask = left < 120 ? (uint32_t)ceil(left) : 120;
            sd_bus_error err = SD_BUS_ERROR_NULL;
            sd_bus_message *reply = NULL;
            uint32_t granted = 0;
            r = sd_bus_call_method(bus, CHARGER_BUS_NAME, CHARGER_PATH, CHARGER_IFACE, "Quiet", &err,
                                   &reply, "u", ask);
            if (r >= 0)
                r = sd_bus_message_read(reply, "u", &granted);
            sd_bus_message_unref(reply);
            if (r < 0)
            {
                fprintf(stderr, "linht-chargerd: Quiet refused: %s\n",
                        err.name ? err.name : strerror(-r));
                sd_bus_error_free(&err);
                rc = 1;
                break;
            }
            printf("quiet lease %u s\n", granted);
            fflush(stdout);
            next = now + (granted > 60 ? 60 : granted / 2.0 + 0.5);
        }
        // Drain incoming messages (NameAcquired and similar).
        while (sd_bus_process(bus, NULL) > 0)
            ;
        usleep(200000);
    }
    sd_bus_call_method(bus, CHARGER_BUS_NAME, CHARGER_PATH, CHARGER_IFACE, "Release", NULL, NULL, "");
    sd_bus_flush_close_unref(bus);
    return rc;
}

int main(int argc, char **argv)
{
    const char *conf = CHARGERD_CONFIG_PATH;
    int do_restore = 0, i = 1;

    for (; i < argc && argv[i][0] == '-'; i++)
    {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            conf = argv[++i];
        else if (!strcmp(argv[i], "--restore"))
            do_restore = 1;
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

    if (i < argc && !strcmp(argv[i], "status"))
        return status(i + 1 < argc && !strcmp(argv[i + 1], "--json"));
    if (i < argc && !strcmp(argv[i], "quiet"))
    {
        char *end;
        long n = i + 1 < argc ? strtol(argv[i + 1], &end, 10) : -1;
        if (i + 1 >= argc || *end || n < 0)
        {
            usage(stderr);
            return 2;
        }
        return quiet(n);
    }
    if (i < argc)
    {
        usage(stderr);
        return 2;
    }

    config_defaults(&core.cfg);
    int rc = config_load(&core.cfg, conf);
    // The stock values are written even with a bad config: --restore is the
    // fail safe of the unit file.
    if (rc < 0 && !do_restore)
        return 1;
    if (rc > 0 && !do_restore)
        core_log(4, "%s not found, using defaults", conf);
    if (do_restore)
        return restore();
    return daemon_run();
}
