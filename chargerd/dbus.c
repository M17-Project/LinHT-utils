// linht-chargerd: D-Bus interface org.m17project.linht.Charger1

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"

// Snapshot of all properties, refreshed after every poll. The getters read
// it through the sd-bus property offset.
struct props
{
    const char *state, *chg_status, *input_limit, *blocked;
    int usb, chg_en, quiet, pfm, cap_reliable;
    uint32_t holders;
    const char *faults[CORE_MAX_FAULTS];
    int nfaults;

    double vbat, vsys, vbus, ibat, ibus, pbat, pbus, tdie, age, vreg;
    double pavg1, pavg10, pavg60, iavg1, iavg10, iavg60;
    double soc, energy, cap_rated, cap_learned, soh, cycles;
    const char *soc_source;
    double learn_w;
    uint32_t learn_n, recoveries;
    int64_t runtime, ttf;
    uint64_t onbat, charging;
};

struct holder
{
    char name[POLICY_OWNER_LEN];
    sd_bus_track *track;
    struct holder *next;
};

static struct
{
    sd_bus *bus;
    struct core *c;
    struct props p, old;
    struct holder *holders;
} S;

static void refresh(struct props *p)
{
    struct core *c = S.c;
    const struct bq_meas *m = &c->m;
    const struct telemetry *t = &c->tel;
    int disch = core_discharging(c);

    memset(p, 0, sizeof(*p));
    p->state = policy_state_name(c->pol.state);
    p->chg_status = bq_chg_stat_name(m->chg_stat);
    p->input_limit = m->vindpm ? "voltage" : m->iindpm ? "current" : "none";
    p->blocked = policy_block_name(c->pol.block);
    p->usb = core_usb(c);
    p->chg_en = c->pol.charging_enabled;
    p->quiet = policy_quiet_active(&c->pol);
    p->holders = (uint32_t)c->pol.nlease;
    p->pfm = !(c->regs[BQ_REG_CTRL3] & BQ_PFM_FWD_DIS);
    p->cap_reliable = tel_capacity_reliable(t);
    p->nfaults = core_faults(c, p->faults, CORE_MAX_FAULTS);

    p->vbat = m->vbat;
    p->vsys = m->vsys;
    p->vbus = m->vbus;
    p->ibat = m->ibat;
    p->ibus = m->ibus;
    p->pbat = m->vbat * m->ibat;
    p->pbus = m->vbus * m->ibus;
    p->tdie = m->tdie;
    p->vreg = m->vreg;
    p->age = c->age;
    p->recoveries = c->recoveries;
    p->pavg1 = tel_avg_power(t, 60);
    p->pavg10 = tel_avg_power(t, 600);
    p->pavg60 = tel_avg_power(t, 3600);
    p->iavg1 = tel_avg_current(t, 60);
    p->iavg10 = tel_avg_current(t, 600);
    p->iavg60 = tel_avg_current(t, 3600);
    p->soc = t->soc;
    p->soc_source = isnan(t->soc) ? "" : t->soc_from_counter ? "counter" : "voltage";
    p->energy = tel_energy_remaining(t);
    p->cap_rated = c->cfg.capacity_rated_wh;
    p->cap_learned = t->cap_wh;
    p->soh = tel_soh(t);
    p->learn_n = (uint32_t)t->learn_n;
    p->learn_w = t->learn_w;
    p->cycles = tel_equivalent_cycles(t);
    p->runtime = tel_runtime_s(t, disch);
    p->ttf = tel_time_to_full_s(t, c->pol.state == ST_CHARGING);
    double now = c->tel.last_now;
    p->onbat = tel_since(t->discharge_since, now);
    p->charging = tel_since(t->charge_since, now);
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

static int get_x(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "x", *(int64_t *)userdata);
}

static int get_t(sd_bus *b, const char *path, const char *iface, const char *prop,
                 sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err;
    return sd_bus_message_append(reply, "t", *(uint64_t *)userdata);
}

static int get_faults(sd_bus *b, const char *path, const char *iface, const char *prop,
                      sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
    (void)b, (void)path, (void)iface, (void)prop, (void)err, (void)userdata;
    int r = sd_bus_message_open_container(reply, 'a', "s");
    for (int i = 0; r >= 0 && i < S.p.nfaults; i++)
        r = sd_bus_message_append(reply, "s", S.p.faults[i]);
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

// --- Lease holders: one sd_bus_track per unique name ---

static struct holder *holder_find(const char *name)
{
    for (struct holder *h = S.holders; h; h = h->next)
        if (!strcmp(h->name, name))
            return h;
    return NULL;
}

static void holder_free(struct holder *h)
{
    struct holder **pp = &S.holders;
    while (*pp && *pp != h)
        pp = &(*pp)->next;
    if (*pp)
        *pp = h->next;
    sd_bus_track_unref(h->track);
    free(h);
}

static int holder_gone(sd_bus_track *track, void *userdata)
{
    struct holder *h = userdata;
    (void)track;
    core_log(6, "quiet holder %s left the bus", h->name);
    policy_release(&S.c->pol, h->name);
    holder_free(h);
    core_apply(S.c, core_now());
    dbus_update();
    return 0;
}

static int holder_add(sd_bus_message *m, const char *name)
{
    if (holder_find(name))
        return 0;
    struct holder *h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    snprintf(h->name, sizeof(h->name), "%s", name);
    int r = sd_bus_track_new(S.bus, &h->track, holder_gone, h);
    if (r >= 0)
        r = sd_bus_track_add_sender(h->track, m);
    if (r < 0)
    {
        sd_bus_track_unref(h->track);
        free(h);
        return r;
    }
    h->next = S.holders;
    S.holders = h;
    return 0;
}

// Leases that ended in the policy (expiry, low battery, fault) drop their
// tracking here.
static void holders_sync(void)
{
    struct holder *h = S.holders, *next;
    for (; h; h = next)
    {
        next = h->next;
        if (!policy_has_lease(&S.c->pol, h->name))
            holder_free(h);
    }
}

// --- Methods ---

static int method_quiet(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    uint32_t seconds;
    unsigned granted = 0;
    int r = sd_bus_message_read(m, "u", &seconds);
    if (r < 0)
        return r;
    const char *sender = sd_bus_message_get_sender(m);
    if (!sender)
        return -EINVAL;

    double now = core_now();
    r = policy_quiet(&S.c->pol, sender, seconds, now, &granted);
    switch (r)
    {
    case PERR_LOW_BATTERY:
        return sd_bus_error_set(err, CHARGER_ERROR "LowBattery", "battery too low for quiet");
    case PERR_FAULT:
        return sd_bus_error_set(err, CHARGER_ERROR "Fault", "charger fault");
    case PERR_CHIP:
        return sd_bus_error_set(err, CHARGER_ERROR "Chip", "charger does not answer");
    case PERR_TOO_MANY:
        return sd_bus_error_set(err, CHARGER_ERROR "TooManyHolders", "too many quiet holders");
    }
    if (granted)
    {
        r = holder_add(m, sender);
        if (r < 0)
        {
            policy_release(&S.c->pol, sender);
            return r;
        }
    }
    core_apply(S.c, now);
    holders_sync();
    dbus_update();
    return sd_bus_reply_method_return(m, "u", (uint32_t)granted);
}

static int method_release(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    const char *sender = sd_bus_message_get_sender(m);
    if (sender)
        policy_release(&S.c->pol, sender);
    core_apply(S.c, core_now());
    holders_sync();
    dbus_update();
    return sd_bus_reply_method_return(m, "");
}

static int method_set_charging(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    int enable;
    int r = sd_bus_message_read(m, "b", &enable);
    if (r < 0)
        return r;
    core_log(6, "charging %s by %s", enable ? "enabled" : "disabled",
             sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "?");
    policy_set_charging(&S.c->pol, enable);
    core_apply(S.c, core_now());
    dbus_update();
    return sd_bus_reply_method_return(m, "");
}

static int method_get_registers(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata, (void)err;
    uint8_t regs[BQ_NREGS];
    sd_bus_message *reply = NULL;

    memcpy(regs, S.c->regs, sizeof(regs));
    // Clear on read flags: everything seen since the previous call.
    memcpy(regs + BQ_REG_FLAG0, S.c->flags_acc, sizeof(S.c->flags_acc));
    memset(S.c->flags_acc, 0, sizeof(S.c->flags_acc));

    int r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0)
        r = sd_bus_message_append_array(reply, 'y', regs, sizeof(regs));
    if (r >= 0)
        r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}

#define EMITS SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE
#define P(name, sig, get, field, flags) SD_BUS_PROPERTY(name, sig, get, offsetof(struct props, field), flags)

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_NAMES("Quiet", "u", SD_BUS_PARAM(seconds), "u", SD_BUS_PARAM(granted),
                             method_quiet, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", method_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("SetCharging", "b", SD_BUS_PARAM(enable), "", ,
                             method_set_charging, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_NAMES("GetRegisters", "", , "ay", SD_BUS_PARAM(registers),
                             method_get_registers, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL_WITH_NAMES("LowBattery", "sd", SD_BUS_PARAM(level) SD_BUS_PARAM(percent), 0),

    P("State", "s", get_s, state, EMITS),
    P("ChargeStatus", "s", get_s, chg_status, EMITS),
    P("UsbPresent", "b", get_b, usb, EMITS),
    P("InputLimit", "s", get_s, input_limit, EMITS),
    P("ChargingEnabled", "b", get_b, chg_en, EMITS),
    P("Quiet", "b", get_b, quiet, EMITS),
    P("QuietHolders", "u", get_u, holders, EMITS),
    P("QuietBlockedReason", "s", get_s, blocked, EMITS),
    P("Pfm", "b", get_b, pfm, EMITS),
    SD_BUS_PROPERTY("Faults", "as", get_faults, 0, EMITS),
    P("CapacityReliable", "b", get_b, cap_reliable, EMITS),

    P("BatteryVoltage", "d", get_d, vbat, 0),
    P("SystemVoltage", "d", get_d, vsys, 0),
    P("UsbVoltage", "d", get_d, vbus, 0),
    P("BatteryCurrent", "d", get_d, ibat, 0),
    P("UsbCurrent", "d", get_d, ibus, 0),
    P("BatteryPower", "d", get_d, pbat, 0),
    P("UsbPower", "d", get_d, pbus, 0),
    P("DieTemperature", "d", get_d, tdie, 0),
    P("ChargeVoltage", "d", get_d, vreg, 0),
    P("MeasurementAgeSec", "d", get_d, age, 0),
    P("ChargerRecoveries", "u", get_u, recoveries, 0),
    P("BatteryPowerAvg1m", "d", get_d, pavg1, 0),
    P("BatteryPowerAvg10m", "d", get_d, pavg10, 0),
    P("BatteryPowerAvg60m", "d", get_d, pavg60, 0),
    P("BatteryCurrentAvg1m", "d", get_d, iavg1, 0),
    P("BatteryCurrentAvg10m", "d", get_d, iavg10, 0),
    P("BatteryCurrentAvg60m", "d", get_d, iavg60, 0),
    P("StateOfChargePercent", "d", get_d, soc, 0),
    P("StateOfChargeSource", "s", get_s, soc_source, 0),
    P("EnergyRemainingWh", "d", get_d, energy, 0),
    P("RunTimeRemainingSec", "x", get_x, runtime, 0),
    P("TimeToFullSec", "x", get_x, ttf, 0),
    P("OnBatterySec", "t", get_t, onbat, 0),
    P("ChargingSec", "t", get_t, charging, 0),
    P("CapacityRatedWh", "d", get_d, cap_rated, 0),
    P("CapacityLearnedWh", "d", get_d, cap_learned, 0),
    P("CapacityLearnDischarges", "u", get_u, learn_n, 0),
    P("CapacityLearnWeight", "d", get_d, learn_w, 0),
    P("StateOfHealthPercent", "d", get_d, soh, 0),
    P("EquivalentCycles", "d", get_d, cycles, 0),
    SD_BUS_VTABLE_END,
};

void dbus_update(void)
{
    const char *changed[16];
    int n = 0;

    if (!S.bus)
        return;
    holders_sync();
    S.old = S.p;
    refresh(&S.p);

#define CH(field, name) \
    if (S.old.field != S.p.field) \
        changed[n++] = name
    CH(state, "State");
    CH(chg_status, "ChargeStatus");
    CH(usb, "UsbPresent");
    CH(input_limit, "InputLimit");
    CH(chg_en, "ChargingEnabled");
    CH(quiet, "Quiet");
    CH(holders, "QuietHolders");
    CH(blocked, "QuietBlockedReason");
    CH(pfm, "Pfm");
    CH(cap_reliable, "CapacityReliable");
#undef CH
    if (S.old.nfaults != S.p.nfaults || memcmp(S.old.faults, S.p.faults, sizeof(S.p.faults)))
        changed[n++] = "Faults";
    changed[n] = NULL;
    if (n)
        sd_bus_emit_properties_changed_strv(S.bus, CHARGER_PATH, CHARGER_IFACE, (char **)changed);

    if (S.c->lowbat_event)
    {
        const char *level = S.c->lowbat_event == 2 ? "critical" : "low";
        core_log(4, "battery %s: %.0f %%", level, S.c->tel.soc);
        sd_bus_emit_signal(S.bus, CHARGER_PATH, CHARGER_IFACE, "LowBattery", "sd", level, S.c->tel.soc);
        S.c->lowbat_event = 0;
    }
}

int dbus_start(sd_bus *bus, struct core *c)
{
    S.bus = bus;
    S.c = c;
    refresh(&S.p);
    int r = sd_bus_add_object_vtable(bus, NULL, CHARGER_PATH, CHARGER_IFACE, vtable, &S.p);
    if (r < 0)
        return r;
    return sd_bus_request_name(bus, CHARGER_BUS_NAME, 0);
}

void dbus_stop(void)
{
    while (S.holders)
        holder_free(S.holders);
    if (S.bus)
        sd_bus_release_name(S.bus, CHARGER_BUS_NAME);
    S.bus = NULL;
}
