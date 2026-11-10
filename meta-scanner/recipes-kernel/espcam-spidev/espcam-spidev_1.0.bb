SUMMARY = "ESP32-CAM SPI-slave V4L2 capture driver + DT overlay (rotary scanner)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module deploy

SRC_URI = " \
    file://Makefile \
    file://espcam_spidev.c \
    file://espcam-spidev-overlay.dts \
"

S = "${WORKDIR}"

DEPENDS += "dtc-native"

do_compile:append() {
    dtc -@ -I dts -O dtb -o ${S}/espcam-spidev.dtbo ${S}/espcam-spidev-overlay.dts
}

do_install:append() {
    install -d ${D}/boot/overlays
    install -m 0644 ${S}/espcam-spidev.dtbo ${D}/boot/overlays/espcam-spidev.dtbo
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${S}/espcam-spidev.dtbo ${DEPLOYDIR}/espcam-spidev.dtbo
}
addtask deploy after do_compile before do_build

FILES:${PN} += "/boot/overlays/espcam-spidev.dtbo"

# Load the module at boot
KERNEL_MODULE_AUTOLOAD += "espcam_spidev"

RPROVIDES:${PN} += "kernel-module-espcam-spidev"

