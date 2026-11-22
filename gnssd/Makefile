# linht-gnssd: Quectel LG77L GNSS daemon for LinHT
#
#   make            the daemon (needs libsystemd)
#   make test       unit tests on the host with a fake receiver (no libsystemd)
#   make asan       the same tests with AddressSanitizer and UBSan
#   make install    DESTDIR and PREFIX as usual

CC ?= gcc
CFLAGS ?= -O2
WARN = -std=gnu11 -Wall -Wextra
PREFIX ?= /usr
SYSCONFDIR ?= /etc
UNITDIR ?= /lib/systemd/system
DBUSDIR ?= $(PREFIX)/share/dbus-1/system.d

LIB = core.c nmea.c config.c timesync.c
SRC = main.c dbus.c feed.c tty.c $(LIB)
HDR = $(wildcard *.h)
TESTS = tests/run.c tests/test_nmea.c tests/test_core.c tests/test_timesync.c
TEST_HDR = $(wildcard tests/*.h)

all: linht-gnssd

linht-gnssd: $(SRC) $(HDR)
	$(CC) $(WARN) $(CFLAGS) $(LDFLAGS) $(SRC) -o $@ -lsystemd -lm

tests/run-tests: $(TESTS) $(TEST_HDR) $(LIB) $(HDR)
	$(CC) $(WARN) $(CFLAGS) -g -I. $(TESTS) $(LIB) -o $@ -lm

tests/run-tests-asan: $(TESTS) $(TEST_HDR) $(LIB) $(HDR)
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		$(TESTS) $(LIB) -o $@ -lm

test: tests/run-tests
	./tests/run-tests

asan: tests/run-tests-asan
	./tests/run-tests-asan

install: linht-gnssd
	install -D -m 0755 linht-gnssd $(DESTDIR)$(PREFIX)/bin/linht-gnssd
	install -D -m 0644 linht-gnssd.service $(DESTDIR)$(UNITDIR)/linht-gnssd.service
	install -D -m 0644 org.m17project.linht.Gnss1.conf \
		$(DESTDIR)$(DBUSDIR)/org.m17project.linht.Gnss1.conf
	install -D -m 0644 gnssd.conf $(DESTDIR)$(SYSCONFDIR)/linht/gnssd.conf

clean:
	rm -f linht-gnssd tests/run-tests tests/run-tests-asan

.PHONY: all test asan install clean
