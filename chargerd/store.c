// linht-chargerd: state file and cycle log in the state directory

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "store.h"

static void path(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

// JSON has no NaN, it is written as null.
static void num(FILE *f, const char *key, double v, int last)
{
    if (isnan(v) || isinf(v))
        fprintf(f, "  \"%s\": null%s\n", key, last ? "" : ",");
    else
        fprintf(f, "  \"%s\": %.6f%s\n", key, v, last ? "" : ",");
}

static int write_atomic(const char *file, const char *text, size_t len)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;
    int rc = 0;
    if (write(fd, text, len) != (ssize_t)len || fsync(fd) < 0)
        rc = errno ? -errno : -EIO;
    if (close(fd) < 0 && !rc)
        rc = -errno;
    if (!rc && rename(tmp, file) < 0)
        rc = -errno;
    if (rc)
        unlink(tmp);
    return rc;
}

int store_save(const char *dir, const struct tel_saved *s)
{
    char file[256], *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return -ENOMEM;

    fprintf(f, "{\n");
    num(f, "capacity_learned_wh", s->cap_wh, 0);
    fprintf(f, "  \"learn_n50\": %d,\n  \"learn_n70\": %d,\n", s->n50, s->n70);
    num(f, "discharged_total_wh", s->out_total_wh, 0);
    num(f, "energy_since_full_wh", s->e_wh, 0);
    fprintf(f, "  \"anchored\": %d,\n", s->anchored);
    num(f, "soc", s->soc, 0);
    num(f, "time", s->wall, 0);
    fprintf(f, "  \"learn_active\": %d,\n", s->learn_active);
    num(f, "learn_energy_wh", s->learn_e, 0);
    num(f, "learn_time_s", s->learn_t, 0);
    num(f, "learn_soc_voltage", s->learn_socv, 0);
    num(f, "capacity_partial_wh", s->part_wh, 0);
    num(f, "partial_weight", s->part_w, 0);
    fprintf(f, "  \"partial_samples\": %d,\n", s->part_n);
    fprintf(f, "  \"clean_stop\": %d\n}\n", s->clean_stop);
    fclose(f);

    path(file, sizeof(file), dir, STORE_STATE);
    int rc = write_atomic(file, buf, len);
    free(buf);
    return rc;
}

static double get(const char *text, const char *key, double def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p)
        return def;
    p += strlen(pat);
    while (*p == ' ')
        p++;
    if (!strncmp(p, "null", 4))
        return NAN;
    char *end;
    double v = strtod(p, &end);
    return end == p ? def : v;
}

int store_load(const char *dir, struct tel_saved *s)
{
    char file[256], text[2048];
    path(file, sizeof(file), dir, STORE_STATE);
    FILE *f = fopen(file, "r");
    if (!f)
        return -errno;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;

    memset(s, 0, sizeof(*s));
    s->cap_wh = get(text, "capacity_learned_wh", 0);
    s->n50 = (int)get(text, "learn_n50", 0);
    s->n70 = (int)get(text, "learn_n70", 0);
    s->out_total_wh = get(text, "discharged_total_wh", 0);
    s->e_wh = get(text, "energy_since_full_wh", 0);
    s->anchored = (int)get(text, "anchored", 0);
    s->soc = get(text, "soc", NAN);
    s->wall = get(text, "time", 0);
    s->learn_active = (int)get(text, "learn_active", 0);
    s->learn_e = get(text, "learn_energy_wh", 0);
    s->learn_t = get(text, "learn_time_s", 0);
    s->learn_socv = get(text, "learn_soc_voltage", NAN);
    s->clean_stop = (int)get(text, "clean_stop", 0);
    s->part_wh = get(text, "capacity_partial_wh", NAN);
    s->part_w = get(text, "partial_weight", 0);
    s->part_n = (int)get(text, "partial_samples", 0);
    if (isnan(s->part_wh) || isnan(s->part_w))
        s->part_n = 0;
    if (isnan(s->cap_wh) || isnan(s->e_wh) || isnan(s->out_total_wh))
        return -EINVAL;
    return 0;
}

static void jnum(FILE *f, const char *key, double v, const char *fmt)
{
    fprintf(f, "\"%s\":", key);
    if (isnan(v))
        fprintf(f, "null");
    else
        fprintf(f, fmt, v);
    fputc(',', f);
}

// Keeps the newest max - 1 lines, so the next append makes max.
static int trim_log(const char *file, int max)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return 0;
    char *line = NULL, **keep = calloc((size_t)max, sizeof(char *));
    size_t cap = 0;
    int n = 0;
    if (!keep)
    {
        fclose(f);
        return -ENOMEM;
    }
    while (getline(&line, &cap, f) > 0)
    {
        free(keep[n % max]);
        keep[n % max] = strdup(line);
        n++;
    }
    free(line);
    fclose(f);

    int rc = 0;
    if (n >= max)
    {
        char *buf = NULL;
        size_t len = 0;
        FILE *m = open_memstream(&buf, &len);
        if (!m)
            rc = -ENOMEM;
        else
        {
            for (int k = n - max + 1; k < n; k++)
                fputs(keep[k % max], m);
            fclose(m);
            rc = write_atomic(file, buf, len);
            free(buf);
        }
    }
    for (int k = 0; k < max; k++)
        free(keep[k]);
    free(keep);
    return rc;
}

int store_append_cycle(const char *dir, const struct tel_cycle *c)
{
    char file[256];
    path(file, sizeof(file), dir, STORE_CYCLES);
    trim_log(file, STORE_CYCLES_MAX);

    FILE *f = fopen(file, "a");
    if (!f)
        return -errno;
    fprintf(f, "{\"kind\":\"%s\",", c->kind == TEL_DISCHARGE ? "discharge" : "charge");
    jnum(f, "start", c->t_start, "%.0f");
    jnum(f, "end", c->t_end, "%.0f");
    jnum(f, "duration_s", c->duration, "%.0f");
    jnum(f, "vbat_start", c->vbat_start, "%.3f");
    jnum(f, "vbat_end", c->vbat_end, "%.3f");
    jnum(f, "soc_start", c->soc_start, "%.1f");
    jnum(f, "soc_end", c->soc_end, "%.1f");
    jnum(f, "energy_wh", c->energy_wh, "%.3f");
    jnum(f, "charge_ah", c->charge_ah, "%.3f");
    jnum(f, "power_mean_w", c->power_mean_w, "%.3f");
    jnum(f, "tdie_max", c->tdie_max, "%.1f");
    fprintf(f, "\"vbat_curve\":[");
    for (int k = 0; k < TEL_CURVE_PTS; k++)
    {
        if (isnan(c->curve[k]))
            fprintf(f, "%snull", k ? "," : "");
        else
            fprintf(f, "%s%.3f", k ? "," : "", c->curve[k]);
    }
    fprintf(f, "]}\n");
    int rc = fflush(f) ? -errno : 0;
    if (fsync(fileno(f)) < 0 && !rc)
        rc = -errno;
    fclose(f);
    return rc;
}
