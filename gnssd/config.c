// linht-gnssd: configuration file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stddef.h>

#include "config.h"

void config_defaults(struct gnssd_config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->device, sizeof(c->device), "/dev/ttyLP2");
    c->baud = 9600;
    c->stale_s = 5.0;
    c->watchdog_s = 10.0;
    c->watchdog_reset = 1;
    c->reset_after_reopens = 3;
    c->reset_min_interval_s = 600.0;
    snprintf(c->reset_chip, sizeof(c->reset_chip), "/dev/gpiochip2");
    c->reset_line = 3;
    c->reset_ms = 150;
    c->position_interval_s = 5.0;
    snprintf(c->pty_link, sizeof(c->pty_link), "/run/linht-gnssd/nmea");
    snprintf(c->socket_path, sizeof(c->socket_path), "/run/linht-gnssd/nmea.sock");
    snprintf(c->feed_group, sizeof(c->feed_group), "dialout");
    c->send_command = 0;
    c->time_sync = TS_AUTO;
    c->time_sync_threshold_s = 2.0;
    c->time_sync_delay_ms = 125;
    snprintf(c->rtc_device, sizeof(c->rtc_device), "/dev/rtc1");
}

static const char *const ts_modes[] = { "auto", "off", "always" };

const char *time_sync_mode_name(int mode)
{
    return mode >= 0 && mode <= TS_ALWAYS ? ts_modes[mode] : "?";
}

enum kind { K_STR, K_INT, K_UINT, K_DBL, K_BOOL, K_MODE };

struct key
{
    const char *name;
    enum kind kind;
    size_t off;
    size_t len;
};

#define S(n) { #n, K_STR, offsetof(struct gnssd_config, n), sizeof(((struct gnssd_config *)0)->n) }
#define I(n) { #n, K_INT, offsetof(struct gnssd_config, n), 0 }
#define U(n) { #n, K_UINT, offsetof(struct gnssd_config, n), 0 }
#define D(n) { #n, K_DBL, offsetof(struct gnssd_config, n), 0 }
#define B(n) { #n, K_BOOL, offsetof(struct gnssd_config, n), 0 }
#define M(n) { #n, K_MODE, offsetof(struct gnssd_config, n), 0 }

static const struct key keys[] = {
    S(device), U(baud), D(stale_s), D(watchdog_s), B(watchdog_reset), U(reset_after_reopens),
    D(reset_min_interval_s), S(reset_chip), I(reset_line), U(reset_ms), D(position_interval_s),
    S(pty_link),
    { "socket", K_STR, offsetof(struct gnssd_config, socket_path), sizeof(((struct gnssd_config *)0)->socket_path) },
    S(feed_group), B(send_command), M(time_sync), D(time_sync_threshold_s), D(time_sync_delay_ms),
    S(rtc_device),
};

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int set_value(struct gnssd_config *c, const struct key *k, const char *v)
{
    char *p = (char *)c + k->off;
    char *end;

    switch (k->kind)
    {
    case K_STR:
        if (strlen(v) >= k->len)
            return -1;
        strcpy(p, v);
        return 0;
    case K_INT:
        *(int *)p = (int)strtol(v, &end, 0);
        return *end ? -1 : 0;
    case K_UINT:
        *(unsigned *)p = (unsigned)strtoul(v, &end, 0);
        return *end ? -1 : 0;
    case K_DBL:
        *(double *)p = strtod(v, &end);
        return *end ? -1 : 0;
    case K_BOOL:
        if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcmp(v, "1"))
            *(int *)p = 1;
        else if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcmp(v, "0"))
            *(int *)p = 0;
        else
            return -1;
        return 0;
    case K_MODE:
        for (int i = 0; i <= TS_ALWAYS; i++)
            if (!strcasecmp(v, ts_modes[i]))
            {
                *(int *)p = i;
                return 0;
            }
        return -1;
    }
    return -1;
}

int config_load(struct gnssd_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;

    char line[256];
    int n = 0, rc = 0;
    while (fgets(line, sizeof(line), f))
    {
        n++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *s = trim(line);
        if (!*s)
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
        {
            fprintf(stderr, "<3>%s:%d: expected key = value\n", path, n);
            rc = -1;
            continue;
        }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        size_t i;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(keys[i].name, k))
                break;
        if (i == sizeof(keys) / sizeof(keys[0]) || set_value(c, &keys[i], v) < 0)
        {
            fprintf(stderr, "<3>%s:%d: bad setting '%s'\n", path, n, k);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}
