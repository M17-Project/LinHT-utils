// linht-chargerd: configuration file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stddef.h>

#include "config.h"

void config_defaults(struct chargerd_config *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->i2c_dev, sizeof(c->i2c_dev), "/dev/i2c-2");
    c->i2c_addr = 0x6b;
    snprintf(c->state_dir, sizeof(c->state_dir), "/var/lib/linht-chargerd");
    c->pfm = 0;
    c->full_hold = 0;
    c->full_hold_resume_soc = 90.0;
    c->quiet_max_s = 300;
    c->quiet_min_soc = 25.0;
    c->quiet_end_soc = 20.0;
    c->quiet_resume_soc = 40.0;
    c->quiet_min_vbat = 7.45;
    c->quiet_end_vbat = 7.35;
    c->low_soc = 15.0;
    c->critical_soc = 5.0;
    snprintf(c->battery, sizeof(c->battery), "Retevis C62 2S Li-ion");
    c->cells = 2;
    c->capacity_rated_wh = 14.8;
    c->capacity_rated_mah = 2000.0;
}

enum kind { K_STR, K_INT, K_UINT, K_DBL, K_BOOL };

struct key
{
    const char *name;
    enum kind kind;
    size_t off;
    size_t len;
};

#define S(n) { #n, K_STR, offsetof(struct chargerd_config, n), sizeof(((struct chargerd_config *)0)->n) }
#define I(n) { #n, K_INT, offsetof(struct chargerd_config, n), 0 }
#define U(n) { #n, K_UINT, offsetof(struct chargerd_config, n), 0 }
#define D(n) { #n, K_DBL, offsetof(struct chargerd_config, n), 0 }
#define B(n) { #n, K_BOOL, offsetof(struct chargerd_config, n), 0 }

static const struct key keys[] = {
    S(i2c_dev), I(i2c_addr), S(state_dir),
    B(pfm), B(full_hold), D(full_hold_resume_soc),
    U(quiet_max_s), D(quiet_min_soc), D(quiet_end_soc), D(quiet_resume_soc),
    D(quiet_min_vbat), D(quiet_end_vbat),
    D(low_soc), D(critical_soc),
    S(battery), I(cells), D(capacity_rated_wh), D(capacity_rated_mah),
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

static int set_value(struct chargerd_config *c, const struct key *k, const char *v)
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
    }
    return -1;
}

int config_load(struct chargerd_config *c, const char *path)
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
