# Yocto recipe proposal for linht-chargerd

This directory is a proposal for the LinHT-yocto repository. Nothing here is
used by the build of LinHT-utils itself.

## Files

- `linht-chargerd_git.bb`: a recipe of its own for
  `meta-linht-software/recipes-support/linht-chargerd/`. It builds only the
  `chargerd/` directory of LinHT-utils with its Makefile and installs the
  binary, the systemd unit, the D-Bus policy and the default configuration.
  `linht-utils.bb` does not change.

## Changes in LinHT-yocto

1. Copy `linht-chargerd_git.bb` to
   `meta-linht-software/recipes-support/linht-chargerd/`.
2. Add `linht-chargerd` to `IMAGE_INSTALL` in
   `meta-linht-software/recipes-core/images/linht-image.bb`, next to
   `linht-utils`.
3. A systemd hardware watchdog for the SoC, so that a kernel hang does not
   leave the charger in HIZ (EN_HIZ is not reset by the charger watchdog).
   Decided on 2026-11-18. The stock image has `RuntimeWatchdogUSec=0`.
   Add a drop-in to the existing
   `meta-linht-software/recipes-core/systemd/systemd-conf_%.bbappend`:

   ```
   SRC_URI += "file://10-watchdog.conf"

   do_install:append() {
       install -d ${D}${systemd_unitdir}/system.conf.d
       install -m 0644 ${WORKDIR}/10-watchdog.conf ${D}${systemd_unitdir}/system.conf.d/
   }

   FILES:${PN} += "${systemd_unitdir}/system.conf.d/10-watchdog.conf"
   ```

   with `files/10-watchdog.conf`:

   ```
   [Manager]
   RuntimeWatchdogSec=30s
   ```

   `${systemd_unitdir}` is `/lib/systemd` (`/usr/lib/systemd` with usrmerge).
   A drop-in there keeps `/etc/systemd/system.conf.d/` free for local
   changes.

   Tested on Rev B and Rev C on 2026-11-18 with the same file in
   `/etc/systemd/system.conf.d/10-watchdog.conf` and `systemctl
   daemon-reexec`: the journal shows "Using hardware watchdog 'i.MX7ULP
   watchdog timer', version 0, device /dev/watchdog0" and "Watchdog running
   with a timeout of 30s", `systemctl show` gives `RuntimeWatchdogUSec=30s`
   and `WatchdogDevice=/dev/watchdog0`, and `WatchdogLastPingTimestamp`
   moves on. `/dev/watchdog`, `/dev/watchdog0` and `/dev/watchdog1` exist on
   both units.

The recipe follows `branch=main` with `AUTOREV`, so the daemon reaches the
image only after the merge of the `linht-chargerd` branch. To build the
branch before the merge, set `branch=linht-chargerd` in `SRC_URI`.

The shipped `chargerd.conf` keeps `pfm = off`, the right value for Rev C.
Rev B will not be produced again. A Rev B unit can set `pfm = on` in its
local `/etc/linht/chargerd.conf`. The file is a `CONFFILES` entry, so a
local change survives package updates.

## linht-gnssd

`linht-gnssd_git.bb` is the same kind of recipe for the `gnssd/`
directory, for `meta-linht-software/recipes-support/linht-gnssd/`. Add
`linht-gnssd` to `IMAGE_INSTALL` next to `linht-chargerd`. The service is
installed but not enabled (`SYSTEMD_AUTO_ENABLE = "disable"`) while the FART
field logger still opens `/dev/ttyLP2` itself. The daemon would take the
tty lock first and the logger would lose its GNSS data.

## Alternative: inside linht-utils.bb

If one recipe is preferred, these lines go into `linht-utils.bb`:

```
DEPENDS += "systemd"
SYSTEMD_SERVICE:${PN} += "linht-chargerd.service"

do_compile:append() {
    oe_runmake -C ${S}/chargerd CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" linht-chargerd
}

do_install:append() {
    install -Dm 0755 ${S}/chargerd/linht-chargerd ${D}${bindir}/linht-chargerd
    install -Dm 0644 ${S}/chargerd/linht-chargerd.service ${D}${systemd_unitdir}/system/linht-chargerd.service
    install -Dm 0644 ${S}/chargerd/org.m17project.linht.Charger1.conf \
        ${D}${datadir}/dbus-1/system.d/org.m17project.linht.Charger1.conf
    install -Dm 0644 ${S}/chargerd/chargerd.conf ${D}${sysconfdir}/linht/chargerd.conf
}

FILES:${PN} += "${systemd_unitdir}/system/linht-chargerd.service \
                ${datadir}/dbus-1/system.d/org.m17project.linht.Charger1.conf \
                ${sysconfdir}/linht/chargerd.conf"
CONFFILES:${PN} += "${sysconfdir}/linht/chargerd.conf"
```

`RDEPENDS` needs no change: libsystemd comes with systemd, which is the init
of the image.
