SUMMARY = "LG77L GNSS daemon for the LinHT handheld transceiver"
HOMEPAGE = "https://github.com/M17-Project/LinHT-utils"
LICENSE = "CLOSED"

# Same repository as linht-utils, only the gnssd/ directory is built.
SRC_URI = "git://github.com/M17-Project/LinHT-utils.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.1+git${SRCPV}"

S = "${WORKDIR}/git/gnssd"

inherit systemd

DEPENDS = "systemd"

SYSTEMD_SERVICE:${PN} = "linht-gnssd.service"
# Not enabled at boot yet: the FART field logger still opens /dev/ttyLP2
# itself. Switch to "enable" once it reads the pass-through or D-Bus.
SYSTEMD_AUTO_ENABLE = "disable"

EXTRA_OEMAKE = "CC='${CC}' CFLAGS='${CFLAGS}' LDFLAGS='${LDFLAGS}'"

do_compile() {
    oe_runmake linht-gnssd
}

do_install() {
    oe_runmake install DESTDIR=${D} PREFIX=${prefix} SYSCONFDIR=${sysconfdir} \
        UNITDIR=${systemd_system_unitdir} DBUSDIR=${datadir}/dbus-1/system.d
}

FILES:${PN} += "${systemd_system_unitdir}/linht-gnssd.service \
                ${datadir}/dbus-1/system.d/org.m17project.linht.Gnss1.conf \
                ${sysconfdir}/linht/gnssd.conf"

CONFFILES:${PN} += "${sysconfdir}/linht/gnssd.conf"
