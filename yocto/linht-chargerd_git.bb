SUMMARY = "BQ25792 charger daemon for the LinHT handheld transceiver"
HOMEPAGE = "https://github.com/M17-Project/LinHT-utils"
LICENSE = "CLOSED"

# Same repository as linht-utils, only the chargerd/ directory is built.
SRC_URI = "git://github.com/M17-Project/LinHT-utils.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.1+git${SRCPV}"

S = "${WORKDIR}/git/chargerd"

inherit systemd

DEPENDS = "systemd"

SYSTEMD_SERVICE:${PN} = "linht-chargerd.service"
SYSTEMD_AUTO_ENABLE = "enable"

EXTRA_OEMAKE = "CC='${CC}' CFLAGS='${CFLAGS}' LDFLAGS='${LDFLAGS}'"

do_compile() {
    oe_runmake linht-chargerd
}

do_install() {
    oe_runmake install DESTDIR=${D} PREFIX=${prefix} SYSCONFDIR=${sysconfdir} \
        UNITDIR=${systemd_system_unitdir} DBUSDIR=${datadir}/dbus-1/system.d
}

FILES:${PN} += "${systemd_system_unitdir}/linht-chargerd.service \
                ${datadir}/dbus-1/system.d/org.m17project.linht.Charger1.conf \
                ${sysconfdir}/linht/chargerd.conf"

CONFFILES:${PN} += "${sysconfdir}/linht/chargerd.conf"
