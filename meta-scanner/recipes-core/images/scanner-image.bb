SUMMARY = "Rotary scanner image"
LICENSE = "MIT"

require recipes-core/images/core-image-base.bb

IMAGE_INSTALL:append = " libgpiod libgpiod-tools v4l-utils kernel-modules"
