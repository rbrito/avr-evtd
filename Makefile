#
# Makefile for the Linkstation AVR Event daemon running under Linux
#
# Copyright 2008 Bob Perry
# Copyright 2008, 2009 Rogério Brito <rbrito@users.sf.net>
TITLE=Linkstation AVR Event daemon

#
# User configurable variables
#
# Add -DUBOOT to the build process if you are using U-Boot as this will
# remove the redundant EM-Mode NGNGNG poke into flash
#
CC = cc
CFLAGS = -Wall -Wextra -Os
CFLAGS += -DUBOOT

######################################################################
# Almost no user should need to change the contents below
######################################################################
# We have the option to build the daemon for PPC or MIPS

MACHINE = $(shell uname -m)
ifeq (${MACHINE}, mips)
  CFLAGS += -DMIPS
endif

ifneq (, $(shell test /etc/melco))
  CFLAGS += -DNO_MELCO
endif

# Main targets
all: avr-evtd

avr-evtd: avr-evtd.c
	$(CC) $(CFLAGS) -o avr-evtd avr-evtd.c

clean: avr-evtd
	rm -f avr-evtd
	rm -f /etc/init.d/avr-evtd
	rm -f /etc/default/avr-evtd.sample
	rm -f /etc/avr_evtd/EventScript
	rm -f /usr/local/sbin/avr-evtd
	rm -f /usr/local/man/man8/avr-evtd.8

install: avr-evtd
	# ENSURE DAEMON IS STOPPED
	if [ -e /etc/init.d/avr-evtd ]; then /etc/init.d/avr-evtd stop ; fi
	install -m 755    Install/avr-evtd.init /etc/init.d/avr-evtd

	# ENSURE LOCAL DIRECTORY EXISTS AND UPDATE EXECUTABLE
	install -D -m 755 avr-evtd /usr/local/sbin

	# TRANSFER EVENT SCRIPT
	install -D -m 755 Install/EventScript /etc/avr-evtd/EventScript
	install -D        Install/emergency-eth0 /etc/avr-evtd/emergency-eth0
	install -D        Install/recovery.tar /etc/avr-evtd/recovery.tar

	# ENSURE DEFAULT AVAILABLE
	if [ ! -e /etc/default/avr-evtd.config ]; then \
	install -D Install/avr_evtd.config /etc/default/avr_evtd.config ; else \
	install -D Install/avr_evtd.config /etc/default/avr_evtd.sample ; fi

	# ENSURE LOCAL MAN AVAILABLE
	install -D        Install/avr-evtd.8 /usr/local/man/man8/avr-evtd.8

start:
	/etc/init.d/avr-evtd start
