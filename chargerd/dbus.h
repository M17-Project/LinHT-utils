// linht-chargerd: D-Bus interface org.m17project.linht.Charger1

#ifndef CHARGERD_DBUS_H
#define CHARGERD_DBUS_H

#include <systemd/sd-bus.h>

#include "core.h"

#define CHARGER_BUS_NAME "org.m17project.linht.Charger1"
#define CHARGER_PATH     "/org/m17project/linht/Charger1"
#define CHARGER_IFACE    "org.m17project.linht.Charger1"
#define CHARGER_ERROR    "org.m17project.linht.Charger1.Error."

// Adds the object and claims the bus name.
int dbus_start(sd_bus *bus, struct core *c);
// After every poll: refreshes the properties, sends PropertiesChanged for
// the state properties and LowBattery when a limit was crossed.
void dbus_update(void);
void dbus_stop(void);

#endif
