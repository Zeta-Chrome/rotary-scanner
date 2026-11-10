FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://wpa_supplicant-wlan0.conf \
    file://25-wlan0.network \
"

do_install:append() {
    # Wi-Fi layer: read by wpa_supplicant@wlan0.service
    install -d ${D}${sysconfdir}/wpa_supplicant
    install -m 0600 ${WORKDIR}/wpa_supplicant-wlan0.conf ${D}${sysconfdir}/wpa_supplicant/

    # IP layer: DHCP on wlan0 via systemd-networkd
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/25-wlan0.network ${D}${sysconfdir}/systemd/network/

    # Enable the per-interface supplicant unit
    if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${sysconfdir}/systemd/system/multi-user.target.wants
        ln -sf ${systemd_system_unitdir}/wpa_supplicant@.service \
            ${D}${sysconfdir}/systemd/system/multi-user.target.wants/wpa_supplicant@wlan0.service
    fi
}

FILES:${PN} += " \
    ${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf \
    ${sysconfdir}/systemd/network/25-wlan0.network \
    ${sysconfdir}/systemd/system/multi-user.target.wants/wpa_supplicant@wlan0.service \
"

RDEPENDS:${PN} += "systemd"
