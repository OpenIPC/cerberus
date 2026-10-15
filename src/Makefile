# cerberus — on-camera agent. mbedTLS is the only dependency; everything else is libc.
#
# This directory is the agent and nothing else: a firmware build (Buildroot, Yocto, an SDK) points
# at it with `make -C src` and its own CC, MBEDTLS and DESTDIR. Installers, CI and tests live above.
#
# Native build:      make
# Static binary:     make STATIC=1
# Cross (OpenIPC):   make CC=arm-openipc-linux-musleabi-gcc MBEDTLS=/path/to/staging STATIC=1
# Release version:   make VERSION=1.2.0      (otherwise the version in cerberus.c)
# Install:           make install DESTDIR=/path/to/rootfs
# Tests:             make test               (from the repository root; needs Docker)
#
# MBEDTLS points at a prefix holding include/mbedtls and lib/libmbed*. Leave it empty when the
# toolchain already sees them (a distro's mbedtls-dev, or a Buildroot sysroot).
#
# The init script is not a file here: the binary carries it and writes it (`cerberus init-script`), so
# `make install` runs the binary it just built to get it. A cross build cannot do that on its own —
# give it an emulator (RUN=qemu-mipsel), or INIT_SCRIPT=no and have the firmware run
# `cerberus init-script` on the camera. Without either it stops rather than install an agent that
# nothing starts.

CC      ?= cc
STRIP   ?= strip
DESTDIR ?=
PREFIX  ?= /usr
MBEDTLS ?=
VERSION ?=
RUN     ?=
INIT_SCRIPT ?= yes

CFLAGS  ?= -Os -std=c99 -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections
LIBS     = -lmbedtls -lmbedx509 -lmbedcrypto

# CI adds -Werror here without having to restate the defaults above.
CFLAGS  += $(EXTRA_CFLAGS)

ifneq ($(VERSION),)
CFLAGS  += -DCERBERUS_VERSION=\"$(VERSION)\"
endif

ifneq ($(MBEDTLS),)
CFLAGS  += -I$(MBEDTLS)/include
LDFLAGS += -L$(MBEDTLS)/lib
endif

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

BIN = cerberus

all: $(BIN)

$(BIN): cerberus.c
	$(CC) $(CFLAGS) -o $@ cerberus.c $(LDFLAGS) $(LIBS)
	$(STRIP) $@
	@echo "==> $@: $$(wc -c <$@) bytes"

size: $(BIN)
	@size $(BIN) || true
	@ls -l $(BIN)

# DAEMON is wherever the binary that wrote the script was, i.e. here; on the target it is PREFIX.
install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)
ifneq ($(INIT_SCRIPT),no)
	@$(RUN) ./$(BIN) init-script - >/dev/null 2>&1 || { \
		echo "$(BIN) does not run on this machine, so it cannot write its init script:"; \
		echo "  make install RUN=qemu-<arch> ...   (run it under an emulator), or"; \
		echo "  make install INIT_SCRIPT=no ...    (and run 'cerberus init-script' on the camera)"; \
		exit 1; }
	mkdir -p $(DESTDIR)/etc/init.d
	$(RUN) ./$(BIN) init-script - | sed "s|^DAEMON=.*|DAEMON='$(PREFIX)/sbin/$(BIN)'|" >$(DESTDIR)/etc/init.d/S99cerberus
	chmod 0755 $(DESTDIR)/etc/init.d/S99cerberus
endif

clean:
	rm -f $(BIN)

.PHONY: all size install clean
