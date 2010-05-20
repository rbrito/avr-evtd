AVR EVENT DAEMON (PPC-EVTD)
Copyright © 2006	Bob Perry <lb-source@users.sf.net>
Copyright © 2008-2010	Rogério Brito <rbrito@users.sf.net>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

--------------------------------------------------------------------------------

1. USAGE:
	The avr-evtd daemon is aimed at the Buffalo series of Linkstation and
	Kurobox.  This replaces the existing ppc_uartd daemon.  The daemon
	configures the AVR and provides the necessary 'keep-alives' to the AVR
	watchdog timer.

	The daemon also checks that the /mnt and / filesystems are within
	scope (disk space remaining) and also monitors the power button and
	the 'red' reset button.  These button events are turned into requests
	into the EventScript.  This allows a user to control what occurs
	(system wise) when an event is received.  The same script is also used
	by the timer shutdown process.

2. SYSTEM REQUIREMENTS:
	Linkstation or equivalent.  Either stock or custom kernel and either
	standard or Debian distribution.

3.INSTALLATION:
	If you have the GCC tools installed, rebuild and install the process
	by typing:

	make
	make install

	Remove the current start and shutdown symbolic links for the
	ppc_uartd:

	update-rc.d -f ppc_uartd remove

	Create the new symbolic links:
	
	update-rc.d avr-evtd start 12 2 . stop 95 0 6 .

	This daemon can use either the existing melco provided files or a
	custom configuration file.  This custom file should exist within the
	/etc/default directory.  Just edit the sample file contained there and
	move:
	
	mv avr-evtd.sample avr-evtd

4. CREDITS:
	The Linkstation and Kuro communities.

5. REVISION HISTORY:
	1.7.2	Script corrections to the ip control and routing check.
		Other changes to daemon to allow script to run in background
		to improve performance.  New event 'S' added to indicate
		5 minute shutdown warning.

	1.7.1	Some minor corrections to the parser.  Changes to the keep
		alive mechanism to allow user control over the disk full LED.

	1.7	Early additions for ntp time creep. Allow box to run without
		fan.  A  bit more code  reduction. Allow the user to specify
		checked drive partitions so to cater for those with customised
		drives.

	1.6.3	Minor code changes to 'round' up disk usage calculations.
		Changes to configuration file, more readable.  Added
		control over location of debug log files.  Changed the
		'special event' to flash the DISK FULL LED to provide
		some form of feedback of selected mode.

	1.6.2	Changes to EventScript to correct telnet session launch
		and to add launching of the apservd firmware updater
		daemon.

	1.6.1	More code reduction.  Error reporting changed.  Updated
		man pages to reflect changes.  Corrections to EM-Mode
		for the MIPS.  Corrections to scripts for shutdown/reboot.
		Added fan speedup on shutdown/reboot.

	1.6	Changes to many things again.  Improved timer sleep
		resolution.  Addition of paused shutdown. Reducing
		CPU loading. Bug fixes to macro'd timer modes. Corrected
		AVR message 0x31.  Added EM-Mode to the event script
		and changed mode of the reset button. Changed the fan
		fault to provide a scripted shutdown path - following
		reported fan failures on my HG.

	1.5 	Changes to almost everything.  Added macro'd power
		on/off events to allow multiple on/off events each
		day.  Days can be grouped (if similar times) or removed
		if the unit is not required to be powered on.  More
		control added to refresh rates and power hold down
		cycles.  Control over disk full event messages added.
		Minor bugs in shutdown timer corrected to allow shutdown
		for the following day of power on.  Standard SHUTDOWN
		and POWERON now operate as default times if no macro
		times are specified.  Can be removed if no default is
		required.  Double reset event added to launch telnet
		daemon.  Fan monitoring added.  Many more changes made
		too.

	1.4	Changes to the EventScript for the MIPS with
		debian loads.  Changes to the Makefile to add proper
		install process and creation of an Install directory
		containing all the other parts of the package.  Slight
		corrections to the daemon and addition of NO_MELCO
		compilier option to reduce build if melco scripts are not
		used.

	1.3	Modified the start and event scripts to pick up UART
		device from kernel.  Output fan fault messages and
		attempt slow down after 5 minutes.

	1.2	Few more typo's fixed.  Added MIPSEL support and
		changed the makefile to autodetect the device.Changed
		the operation such that the defered configuration files
		are the melco ones.  If /etc/melco is removed, then the
		/etc/default/avr_evtd configuration files are used.  These
		are also monitored for timer update.  Added configuration
		for the remaining disk warning and extracted event out
		to the EventScript.

	1.1 	Renamed from ppc to avr.  Cleaned some typo's from
		this document.
		
		Changes to EventScript to add comments and add three
		new events (7, 8 and 9).

		Changes to avr_evtd launch script for comments and
		try and decode UART required from the machine name.

	Release (1.0)

6. KNOW ISSUES:
	None known.
	Tested now on PPC, MIPS and KURO systems with 2.4 and 2.6 releases of
	the kernel.  Also tested under stock, openlink and freelink versions
	of the updated firmware.

--------------------------------------------------------------------------------
This is Free Software for Linkstation/kuro fans and lovers!

Please just mention my name if modified or linked to.
Bob Perry (UK) JUNE 2006.
Rogério Brito Aug 2009.
