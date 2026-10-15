# The agent lives in src/ — its source, its Makefile and its init script — and that directory is
# what a firmware build points at (`make -C src`). This one only forwards to it, so `make`,
# `make STATIC=1`, `make install DESTDIR=...` work from the root as they always did. Variables given
# on the command line reach src/Makefile on their own; the two that are paths are made absolute
# first, since src/Makefile runs one directory down.

FORWARD = $(if $(DESTDIR),DESTDIR=$(abspath $(DESTDIR))) $(if $(MBEDTLS),MBEDTLS=$(abspath $(MBEDTLS)))

all install size clean:
	$(MAKE) -C src $@ $(FORWARD)

test:
	tests/run.sh

.PHONY: all install size clean test
