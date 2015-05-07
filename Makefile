#
# Makefile for the Linkstation AVR Event daemon running under Linux
#
# Copyright 2008 Bob Perry
# Copyright 2008-2015 Rogério Brito <rbrito@users.sf.net>
#
TITLE=Linkstation AVR Event daemon

#
# User configurable variables
#
CC = c++
CFLAGS = -Wall -Wextra -Weffc++ -Os

######################################################################
# Almost no user should need to change the contents below
######################################################################

ifeq (, $(PREFIX))
	 PREFIX := usr/local
endif

# Main targets
all: avr-evtd

avr-evtd: avr-evtd.cpp
	$(CC) $(CFLAGS) -o avr-evtd avr-evtd.cpp

clean:
	rm -f avr-evtd *~ *.o

install: avr-evtd
	# ENSURE DAEMON IS STOPPED
	if [ -e /etc/init.d/avr-evtd ]; then /etc/init.d/avr-evtd stop ; fi
	install -D -m 755 Install/avr-evtd.init $(DESTDIR)/etc/init.d/avr-evtd

	# ENSURE LOCAL DIRECTORY EXISTS AND UPDATE EXECUTABLE
	install -D -m 755 avr-evtd $(DESTDIR)/$(PREFIX)/sbin/avr-evtd

	# TRANSFER EVENT SCRIPT
	install -D -m 755 Install/EventScript $(DESTDIR)/etc/avr-evtd/EventScript
	install -D        Install/emergency-eth0 $(DESTDIR)/etc/avr-evtd/emergency-eth0
	install -D -m 644 Install/recovery.tar $(DESTDIR)/etc/avr-evtd/recovery.tar

	# ENSURE DEFAULT AVAILABLE
	if [ ! -e /etc/default/avr-evtd.config ]; then \
	install -D Install/avr-evtd.config $(DESTDIR)/etc/default/avr-evtd.config ; else \
	install -D Install/avr-evtd.config $(DESTDIR)/etc/default/avr-evtd.sample ; fi

	# ENSURE LOCAL MAN AVAILABLE
	install -D        Install/avr-evtd.8 $(DESTDIR)/$(PREFIX)/share/man/man8/avr-evtd.8

start:
	/etc/init.d/avr-evtd start

uninstall:
	@if [ $(shell id -u) = 0 ]; then \
	rm -f /etc/init.d/avr-evtd; \
	rm -f /etc/default/avr-evtd.sample; \
	rm -f /etc/avr_evtd/EventScript; \
	rm -f /usr/local/sbin/avr-evtd; \
	rm -f /usr/local/man/man8/avr-evtd.8; \
	fi
