# cerberus — on-camera agent. mbedTLS is the only dependency; everything else is libc.
#
# This directory is the agent and nothing else: a firmware build (Buildroot, Yocto, an SDK) points
# at it with `make -C src` and its own CC, MBEDTLS and DESTDIR. Installers, CI and tests live above.
#
# Native build:      make
# Static binary:     make STATIC=1
# Cross (OpenIPC):   make CC=arm-openipc-linux-musleabi-gcc MBEDTLS=/path/to/staging STATIC=1
# Release version:   make VERSION=1.2.0      (otherwise the version in cerberus.c)
# Tests:             make test               (from the repository root; needs Docker)
#
# MBEDTLS points at a prefix holding include/mbedtls and lib/libmbed*. Leave it empty when the
# toolchain already sees them (a distro's mbedtls-dev, or a Buildroot sysroot).

CC      ?= cc
STRIP   ?= strip
DESTDIR ?=
PREFIX  ?= /usr
MBEDTLS ?=
VERSION ?=

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

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)
	install -D -m 0755 S99cerberus $(DESTDIR)/etc/init.d/S99cerberus

clean:
	rm -f $(BIN)

.PHONY: all size install clean
