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
3. Optional, recommended: a systemd hardware watchdog for the SoC, so that a
   kernel hang does not leave the charger in HIZ. Today
   `RuntimeWatchdogUSec=0` on both units. A drop-in from the existing
   `systemd-conf_%.bbappend`, for example
   `/usr/lib/systemd/system.conf.d/10-watchdog.conf` with

   ```
   [Manager]
   RuntimeWatchdogSec=30s
   ```

   `/dev/watchdog0` exists on the units. Test it on one unit first.

The recipe follows `branch=main` with `AUTOREV`, so the daemon reaches the
image only after the merge of the `linht-chargerd` branch. To build the
branch before the merge, set `branch=linht-chargerd` in `SRC_URI`.

Rev B needs `pfm = on` in `/etc/linht/chargerd.conf`. The file is a
`CONFFILES` entry, so a local change survives package updates.

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
