TITLE=Linkstation AVR Event daemon
#
# Add -DUBOOT to the build process if you are using U-Boot as this will
# remove the redundant EM-Mode NGNGNG poke into flash
#
UNAME-M = $(shell uname -m)
LOCATION=PPC
BUILD=$(shell ls /etc)
ifeq (${UNAME-M}, mips)
  BUILD_CPU=-DMIPS
  LOCATION=MIPS
endif

all: avr_evtd

avr_evtd: avr_evtd.c
	-test -d /etc/melco || \
	gcc -Wall -s -Os -o $(LOCATION)/avr_evtd avr_evtd.c $(BUILD_CPU) -DNO_MELCO
	-test ! -d /etc/melco || \
	gcc -Wall -s -Os -o $(LOCATION)/avr_evtd avr_evtd.c $(BUILD_CPU)
	
clean: avr_evtd
	-rm -f avr_evtd
	-rm -f /etc/init.d/avr_evtd
	-rm -f /usr/sbin/man/man1/avr_evtd.1.gz
	-rm -f /etc/default/avr_evtd
	-rm -f /etc/avr_evtd/EventScript
	-rm -f /usr/local/sbin/avr_evtd
	-rm -f /usr/local/man/man1/avr_evtd.1.gz
	
install: avr_evtd
	#
	# ENSURE DAEMON IS STOPPED
	if [ -e /etc/init.d/avr_evtd ]; then /etc/init.d/avr_evtd stop ; fi
	-rm -f /etc/init.d/avr_evtd
	if [ -e /usr/bin/strip ]; then strip --strip-unneeded avr_evtd ; fi
	cp Install/avr_evtd /etc/init.d/.
	chmod +x /etc/init.d/avr_evtd

	#
	# ENSURE LOCAL DIRECTORY EXISTS
	if [ ! -d /usr/local/sbin ]; then mkdir /usr/local/sbin ; fi

	#
	# UPDATE EXECUTABLE WITH TARGET BUILD
	-cp $(LOCATION)/avr_evtd /usr/local/sbin/.

	#
	# ENSURE STORAGE AVAILABLE
	if [ ! -d /etc/avr_evtd ]; then mkdir /etc/avr_evtd ; fi

	#
	# TRANSFER EVENT SCRIPT
	-cp Install/EventScript /etc/avr_evtd/.
	-chmod +x /etc/avr_evtd/*
	-cp Install/emergency_eth0 /etc/avr_evtd/.
	-cp Install/recovery.tar /etc/avr_evtd/.

	#
	# ENSURE DEFAULT AVAILABLE
	if [ ! -d /etc/default ]; then mkdir /etc/default ; fi
	if [ ! -e /etc/default/avr_evtd ]; then \
	cp Install/avr_evtd.sample /etc/default/avr_evtd ; else \
	cp Install/avr_evtd.sample /etc/default/avr_evtd.sample ; fi

	#
	# ENSURE LOCAL MAN AVAILABLE
	if [ ! -d /usr/local/man ]; then mkdir /usr/local/man ; fi
	if [ ! -d /usr/local/man/man1 ]; then mkdir /usr/local/man/man1 ; fi
	-rm -f /usr/sbin/man/man1/avr_evtd.1.gz
	-cp Install/avr_evtd.1.gz /usr/local/man/man1/.

	-/etc/init.d/avr_evtd start
