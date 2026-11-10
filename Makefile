KAS_FILE ?= kas/scanner.yml
MACHINE  ?= raspberrypi3-64
IMAGE    ?= scanner-image
DEPLOY   := build/tmp/deploy/images/$(MACHINE)

BAUD ?= 115200
PORT ?= $(firstword $(wildcard /dev/serial/by-id/*) $(wildcard /dev/ttyUSB*) $(wildcard /dev/ttyACM*))

WIC  = $(lastword $(sort $(wildcard $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs-*.wic.bz2)))
BMAP = $(lastword $(sort $(wildcard $(DEPLOY)/$(IMAGE)-$(MACHINE).rootfs-*.wic.bmap)))

.PHONY: build shell fetch flash monitor

build:
	kas build $(KAS_FILE)

shell:
	kas shell $(KAS_FILE)

fetch:
	kas shell $(KAS_FILE) -c "bitbake --runall=fetch $(IMAGE)"

flash:
	@test -n "$(DEV)" || { echo "usage: make flash DEV=/dev/sdX"; exit 1; }
	@echo "Image: $(WIC)"
	@lsblk $(DEV)
	@read -p "This ERASES $(DEV). Type y: " ok && [ "$$ok" = y ]
	-sudo umount $(DEV)* 2>/dev/null
	sudo bmaptool copy --bmap $(BMAP) $(WIC) $(DEV)
	sync

monitor:
	picocom -b $(BAUD) $(PORT)
