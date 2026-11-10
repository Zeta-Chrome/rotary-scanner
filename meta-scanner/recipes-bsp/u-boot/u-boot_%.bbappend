FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:rpi = " \
    file://scanner_defconfig \
    file://usb-delay.cfg \
"

# Override the machine default (rpi_arm64_config)
UBOOT_MACHINE:raspberrypi3-64 = "scanner_defconfig"

do_configure:prepend() {
    cp ${WORKDIR}/scanner_defconfig ${S}/configs/scanner_defconfig
}
