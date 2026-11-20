// linht-gnssd: D-Bus interface org.m17project.linht.Gnss1

#ifndef GNSSD_DBUS_H
#define GNSSD_DBUS_H

#include <systemd/sd-bus.h>

#include "core.h"
#include "feed.h"

#define GNSS_BUS_NAME "org.m17project.linht.Gnss1"
#define GNSS_PATH     "/org/m17project/linht/Gnss1"
#define GNSS_IFACE    "org.m17project.linht.Gnss1"
#define GNSS_ERROR    "org.m17project.linht.Gnss1.Error."

// Adds the object and claims the bus name.
int dbus_start(sd_bus *bus, struct core *c, struct feed *f);
// After new data and every tick: finishes waiting method calls, refreshes
// the properties, sends PropertiesChanged, FixChanged and PositionChanged.
void dbus_update(void);
void dbus_stop(void);

#endif
