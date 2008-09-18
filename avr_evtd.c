/*
* Linkstation AVR daemon
* 
* Written by Bob Perry (2006) lb-source@users.sourceforge.net
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*
*/

/*	V1.7.2	Script changes.  Additions to run script in background and
			added 5 minute shutdown event and moved other event messages
			to the system.
	V1.7.1	Minor fixes to the config parser.  Changes to the keep alives to
			allow user control over DISK FULL LED
	V1.7	Allow running without fan.  A  bit more code  reduction. Allow
			the user to specify checked drive partitions so to cater for
			those with customised drives.
	V1.6.3	Correct rounding down in disk usage calculations. Also corrected
			time skew as a result of user/ntp time updates. 
	V1.6.2	Script changes only.
	V1.6.1	Offical release.  Improvements to code size.  Additions made
			to scripts for 'boost' cooling at shutdown/reboot (following
			issues in hot environments).  Fixes to timer calcs.
	V1.6 	Improve available timer resolution.  Provided 'paused' shutdown
			which inhibits timer revalidation as clock is now wrong.  Aid
			CPU loading but also pick up timer demands shortly after init.
			so long refresh periods are caught.  Fixed bug with finding off time as current.
			Corrected AVR message 0x31 to a halt request. Profiled code.
	V1.5	Introduced macro'd ON/OFF control.  Many changes made to
			default and melco file detection.  User control over button
			detection times and daemon refresh rates.
	V1.4	Conditional compilation #def's added for NO_MELCO
	V1.3	Added fan fault messaging.
	V1.2	Changes to UART initialisation.  Code added to support MIPS range.
	V1.1	Project renamed.  Disk check introduced.
	V1.0	Release
*/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <syslog.h>
#include <stdio.h> 
#include <string.h> 
#include <stdlib.h> 
#include <sys/time.h> 
#include <linux/serial.h>

/* A few defs for later */
#define HOLD_TIME		    1
#define HOLD_SECONDS		3
#define FIVE_MINUTES		5*60
#define TWELVEHR		    12*60
#define TWENTYFOURHR		TWELVEHR*2
#define TIMER_RESOLUTION	4095
#define FAN_SEIZE_TIME		30
#define EM_MODE_TIME		20
#define SP_MONITOR_TIME		10

/* Event message definitions */
#define SPECIAL_RESET	'0'
#define AVR_HALT		'1'
#define TIMED_SHUTDOWN	'2'
#define POWER_RELEASE	'3'
#define POWER_PRESS		'4'
#define RESET_RELEASE	'5'
#define RESET_PRESS		'6'
#define USER_POWER_DOWN	'7'
#define USER_RESET		'8'
#define DISK_FULL		'9'
#define FAN_FAULT		'F'
#define EM_MODE			'E'
#define FIVE_SHUTDOWN	'S'
#define ERRORED			'D'

/* Macro event object definition */
typedef struct _OFF_TIMER
{
	int day;	/* Event day */
	long time;	/* Event time (24hr) */
	void* pointer;	/* Pointer to next event */
} TIMER;

/* Some global variables */
#ifdef MIPS
	char avr_device[]="/dev/ttyS0";
#else
	char avr_device[]="/dev/ttyS1";
#endif
TIMER* poffTimer=NULL;
TIMER* ponTimer=NULL;
int i_FileDescriptor = 0;
time_t tt_LastMelcoAcess = 0;
int i_TimerFlag = 0;
long l_ShutdownTimer=9999; /* Careful here */
char c_FirstTimeFlag=1;
char c_FirstWarning=1;
long iOffTime=-1; /* Default, NO defaults */
long iOnTime=-1; /* Default, NO defaults */
#ifndef NO_MELCO
	char c_CommandLineUpdate=0;
#else
	char c_CommandLineUpdate=1;
#endif
int i_checkPercentage=90;
int last_day;
int refreshRate=40;
int holdCycle=3;
char i_debug=0;
char pesterMessage=0;
int fanFaultSeize=30;
int checkState=1; /* Will force an update within 15 seconds of starting up 
					to resolve those pushed out refresh times */
char em_mode=0;
const char strVersion[]="Linkstation/Kuro AVR daemon Version 1.7.2\n";
char rootPartition[10]=""; /* Default, no defaults for both root and working partitions */
char workingPartition[10]="";
int diskCheckNumber=0;
char keepAlive=0x5B;
char resetPresses=0;
int diskUsed=0;

/* Declarations */
static int check_timer(char type);
static void termination_handler(int signum);
static int open_serial(char *device) __attribute__((always_inline));

// Not legal indented #ifdef statement but makes it readable
#ifdef MIPS
	#ifndef NO_MELCO
		static void parse_mips(char* buff) __attribute__((always_inline));
	#endif
#else
	#ifndef NO_MELCO
		static void parse_timer(char* buff);
	#endif
#endif

static int close_serial(void);
static void avr_evtd_main(void);
static char check_disk(void) __attribute__((always_inline));
static void set_avr_timer(char type);
static void parse_avr(char* buff);
static void GetTime(long timeNow, TIMER* pTimerLocate, long* time, long defaultTime);
static int FindNextToday(long timeNow, TIMER* pTimer, long* time);
static int FindNextDay(long timeNow, TIMER* pTimer, long* time, long* offset);
static void destroyObject(TIMER* pTimer);
static void writeUART(char output);
static void errorReport(int errorNumber);
static void execute_command1(char cmd);
static void execute_command(char cmd, int cmd2);

static void writeUART(char output)
{
	/* Handle ALL UART messages from a central point, reduce code overhead */
	char strOutput[4];
	strOutput[0] = strOutput[1] = strOutput[2] = strOutput[3] = output;
	write(i_FileDescriptor, strOutput, 4);
}

static int open_serial(char *device) 
{
	/* Establish connection to comport and initialise the port
		as required */
	struct termios newtio;
#ifndef MIPS
	struct serial_struct serinfo;
#endif

	/* Need read/write access to the AVR */
	i_FileDescriptor = open(device, O_RDWR | O_NOCTTY );

	if(i_FileDescriptor < 0)
	{
		perror(device);
		return -1;
	}
	
#ifndef MIPS
	/* Requested device memory address? */
	if (2 == i_debug)
	{
		ioctl(i_FileDescriptor, TIOCGSERIAL, &serinfo);
		if (serinfo.iomem_base)
			printf("%p\n", serinfo.iomem_base);
		else
			printf("%X\n", serinfo.port);

		return 0;
	}
#endif

	ioctl(i_FileDescriptor, TCFLSH, 2);
	/* Clear data structures */
	memset(&newtio, 0, sizeof(newtio));
	newtio.c_iflag = PARMRK;
	newtio.c_oflag = OPOST;

#ifdef MIPS
	newtio.c_cflag = 0x9FD;	/* CREAD | CS7 | B9600 */
#else
	newtio.c_cflag = PARENB | CLOCAL | CREAD | CSTOPB | CS8 | B9600;
#endif

	/* Update tty settings */
	ioctl(i_FileDescriptor, TCSETS, &newtio);

	ioctl(i_FileDescriptor, TCFLSH, 2);

	/* Initialise the AVR device
	   this includes clearing down memory and reseting
	   the timer */
	writeUART(0x41);
	writeUART(0x46);
	writeUART(0x4A);
	writeUART(0x3E);
	
	// Remove flashing DISK  LED
	writeUART(0x58);
	
	return 0;
}

static int close_serial(void)
{
	if (i_FileDescriptor != 0)
	{
		/* The AVR does not really need to see this, just stops the timer watchdog which happens when it powers down anyway */
#ifndef MIPS
		writeUART(0x4B);
#endif
		/* Close port and invalidate our pointer */
		close(i_FileDescriptor);

		i_FileDescriptor = 0;
	}

	/* Destroy the macro timer objects */
	destroyObject(poffTimer);
	destroyObject(ponTimer);

	/* Tidy up please */
	closelog();
	return 0;
}

static void termination_handler(int signum)
{
	switch (signum)
	{
	case SIGTERM:
		close_serial();
		exit(0);
		break;
	case SIGCONT:
		break;
	default:
		break;
	}
}

static void execute_command(char cmd, int cmd2)
{
	char strEventScript[45];

	/* Send device info to the event script handler */
	sprintf(strEventScript, "/etc/avr_evtd/EventScript %c %s %d &", cmd, avr_device, cmd2);
	system(strEventScript);
}

static void execute_command1(char cmd)
{
	execute_command(cmd, 0);
}

static void avr_evtd_main(void)
{
	/* Our main entry, decode requests and monitor activity */
	char buf[17];
	char cmd;
	char c_PushedPowerFlag = 0;
	char c_PushedResetFlag = 0;
	char c_PressedPowerFlag = 0;
	char c_PressedResetFlag = 0;
	char currentStatus=0;
	time_t tt_TimeIdle = time(NULL);
	time_t tt_Power_Press = tt_TimeIdle;
	time_t tt_fault_time;
	time_t tt_LastShutdownPing;
	time_t tt_TimeNow;
	fd_set fReadFS;
	struct timeval tt_TimeoutPoll;
	int iResult;
	int i_fan_fault=0;
	long lTimerDiff;
	char extraTime=0;
	char diskFull=0;
	
	/* Update the shutdown timer */
	tt_fault_time = 0;
	tt_LastShutdownPing = time(NULL);

	/* Loop whilst port is valid */
	while(i_FileDescriptor)
	{
		tt_TimeoutPoll.tv_usec = 0;
		iResult = refreshRate;
		/* After file change or startup, update the time within 20 secs
		as the user may have pushed the refresh time out */
		if (checkState>0)
		{
			iResult = 2;
		}
		else
		{
			/* Change our timer to check for a power/reset request
			need a faster poll rate here to see the double press event properly */
			if (c_PushedPowerFlag || c_PushedResetFlag || c_FirstTimeFlag > 1)
			{
				tt_TimeoutPoll.tv_usec = 250;
				iResult = 0;
				checkState = -2; /* Hold off any configuration file updates */
			}
		}

		if (checkState != -2)
		{
			/* Ensure we shutdown on the nail if the timer is enabled 
			will be off slightly as timer reads are different */
			if (1 == i_TimerFlag)
			{
				if (l_ShutdownTimer < iResult)
					iResult = l_ShutdownTimer;
			}
			
			/* If we have a fan failure report, then ping frequently */
			if (i_fan_fault > 0)
				iResult = i_fan_fault == 6 ? fanFaultSeize : 2;
		}
		
		tt_TimeoutPoll.tv_sec = iResult;

		FD_ZERO(&fReadFS);
		FD_SET(i_FileDescriptor, &fReadFS);

		/* Wait for AVR message or time-out? */
		iResult = select(i_FileDescriptor + 1, &fReadFS, NULL, NULL, &tt_TimeoutPoll);

		tt_TimeNow = time(NULL);
		
		/* catch input? */
		if(iResult > 0)
		{
			/* Read AVR message */
			iResult = read(i_FileDescriptor, buf, 16);
			/* AVR command detected so force to ping only */
			checkState = -2;
			
			switch(buf[0])
			{
				/* power button release */
				case 0x20:
					if(0 == c_PressedPowerFlag)
					{
						cmd = POWER_RELEASE;

						if ((tt_TimeNow - tt_Power_Press) <= HOLD_TIME && c_FirstTimeFlag < 2)
						{
							cmd = USER_RESET;
						}
						else if (l_ShutdownTimer < FIVE_MINUTES || c_FirstTimeFlag > 1)
						{
							if (0 == c_FirstTimeFlag)
								c_FirstTimeFlag = 10;
								
							l_ShutdownTimer += FIVE_MINUTES;
							c_FirstTimeFlag--;
							extraTime = 1;
						}
						
						execute_command1(cmd);

						tt_Power_Press = tt_TimeNow;
					}

					c_PushedPowerFlag = c_PressedPowerFlag = 0;
					break;

				/* power button push */
				case 0x21:
					execute_command1(POWER_PRESS);

					c_PressedPowerFlag = 0;
					c_PushedPowerFlag = 1;
					break;

				/* reset button release */
				case 0x22:
					if(0 == c_PressedResetFlag)
					{
						cmd = RESET_RELEASE;
						iResult = 0;
						
						/* Launch our telnet daemon */
						if ((tt_TimeNow - tt_Power_Press) <= HOLD_TIME)
						{
							cmd = SPECIAL_RESET;
							iResult = resetPresses;
							resetPresses++;
						}

						execute_command(cmd, iResult);

						tt_Power_Press = tt_TimeNow;
					}
				
					c_PushedResetFlag = c_PressedResetFlag = 0;
					break;

				/* reset button push */
				case 0x23:
					execute_command1(RESET_PRESS);
					
					c_PressedResetFlag = 0;
					c_PushedResetFlag = 1;
					break;
					
				/* Fan on high speed */
				case 0x24:
					i_fan_fault = 6;
					tt_fault_time = tt_TimeNow;
					break;

				/* Fan fault */
				case 0x25:
					/* Flag the EventScript */
					execute_command(FAN_FAULT, i_fan_fault);

					if (fanFaultSeize>0)
					{
						i_fan_fault = 2;
						tt_fault_time = tt_TimeNow;
					}
					else
						i_fan_fault = -1;
						
					break;

				/* Acknowledge */
				case 0x30:
					break;

				/* AVR halt requested */
				case 0x31:
					close_serial();
					execute_command1(AVR_HALT);
					break;

				/* AVR initialisation complete */
				case 0x33:
					break;
#ifdef DEBUG
				default:
					if (buf[0] != 0)
						syslog(LOG_INFO, "unknown message %X[%d]", buf[0], iResult);
					break;
#endif
			}

			/* Get time for use later */
			time(&tt_TimeIdle);
		}
		/* Time-out event */
		else
		{
			/* Check if button(s) are still held after holdcyle seconds */
			if((tt_TimeIdle + holdCycle) < tt_TimeNow)
			{
				/* Power down selected */
				if(1 == c_PushedPowerFlag)
				{
					/* Re-validate our time wake-up; do not perform if in extra time */
					if (!extraTime)
						set_avr_timer(1);

					execute_command1(USER_POWER_DOWN);

					c_PushedPowerFlag = 0;
					c_PressedPowerFlag = 1;
				}

			}

#ifndef UBOOT
			/* Has user held the reset button long enough to request EM-Mode? */
			if ((tt_TimeIdle + EM_MODE_TIME) < tt_TimeNow)
			{
				if(1 == c_PushedResetFlag && em_mode)
				{
					/* Send EM-Mode request to script.  The script handles the flash device decoding
					and writes the HDD no-good flag NGNGNG into the flash status.  It then flags a
					reboot which causes the box to boot from ram-disk backup to recover the HDD */
					execute_command1(EM_MODE);

					c_PushedResetFlag = 0;
					c_PressedResetFlag = 1;
				}
			}
#endif	
			/* Skip this processing during power/reset scan */
			if (!c_PushedResetFlag && !c_PushedPowerFlag && c_FirstTimeFlag < 2)
			{
				/* shutdown timer event? */
				if(1 == i_TimerFlag)
				{
					/* Decrement our powerdown timer */
					if (l_ShutdownTimer>0)
					{
						lTimerDiff = (tt_TimeNow - tt_LastShutdownPing);
					
						/* If time difference is more than a minute, force a re-calculation of shutdown time */
						if (refreshRate + 60 > abs(lTimerDiff))
						{
							l_ShutdownTimer -= lTimerDiff;

							/* Within five minutes of shutdown? */
							if (l_ShutdownTimer < FIVE_MINUTES)
							{
								if (c_FirstTimeFlag)
								{
									c_FirstTimeFlag = 0;

									/* Inform the EventScript */
									execute_command(FIVE_SHUTDOWN, l_ShutdownTimer);
									
									/* Re-validate out time wake-up; do not perform if in extra time */
									if (!extraTime)
										set_avr_timer(1);
								}
							}
						}
						/* Large clock drift, either user set time or an ntp update, handle accordingly. */
						else
						{
							check_timer(2);
						}
					}
					else
					{
						/* Prevent re-entry and execute command */
						c_PushedPowerFlag = c_PressedResetFlag = 2;
						execute_command1(TIMED_SHUTDOWN);
					}
				}

				/* Keep track of shutdown time remaining */
				tt_LastShutdownPing = time(NULL);

				/* Split loading, handle disk checks over a number of cycles, reduce CPU hog */
				switch(checkState)
				{
					/* Kick state machine */
					case 0:
						checkState = 1;
						break;
						
					/* Check for timer change through configuration file */
					case 1:
						check_timer(0);
						checkState = 2;
						break;
					
					/* Check the disk and ping AVR accordingly */
					case -2:

					/* Check the disk to see if full and output appropriate AVR command? */
					case 2:
						cmd = keepAlive;

						if ((currentStatus = check_disk()))
						{
							/* Execute some user code on disk full */
							if (c_FirstWarning)
							{
								c_FirstWarning = pesterMessage;
								execute_command(DISK_FULL, diskUsed);
							}
						}

						/* Only update DISK LED on disk full change */
						if (diskFull != currentStatus)
						{
							/* LED status */
							cmd = 0x56;
							if (currentStatus)
								cmd ++;
							else
							{
								c_FirstWarning = 0;
								execute_command(DISK_FULL, 0);
							}

							diskFull = currentStatus;
						}
						
						/* Ping AVR */
						writeUART(cmd);
							
						checkState = 3;
						break;
						
					/* Wait for next refresh kick */
					case 3:
						checkState = 0;
						break;
				}
			}

			/* Try and catch spurious fan fault messages */
			switch(i_fan_fault)
			{
				case -1:
					break;
				case 1:
					i_fan_fault = 0;
					break;
				/* Check how long we have been operating with a fan failure */
				case 2:
				case 3:
				case 4:
					if ((tt_fault_time + fanFaultSeize) < tt_TimeNow)
					{
						/* Run some user script on no fan restart message after FAN_FAULT_SEIZE time */
						execute_command(FAN_FAULT, 4);
						i_fan_fault = 5;
					}
				
					break;
				/* Fan sped up message received */
				case 6:
					/* Attempt to slow fan down again after 5 minutes */
					if ((tt_fault_time + FIVE_MINUTES) < tt_TimeNow)
					{
						writeUART(0x5C);
						i_fan_fault = 1;
					}
					
					break;
			}
			
			/* Check that the shutdown pause function (if activated) is still available, no then ping the delayed time */
			if ((tt_Power_Press + SP_MONITOR_TIME) < tt_TimeNow && c_FirstTimeFlag > 1)
			{
				/* Inform the EventScript */
				execute_command(FIVE_SHUTDOWN, (int)((float)l_ShutdownTimer/60.0f));
				c_FirstTimeFlag = 1;
				tt_Power_Press = 0;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	char *thisarg;

	argc--;
	argv++;

	/* Parse any options */
	while (argc >= 1 && '-' == **argv)
	{
		thisarg = *argv;
		thisarg++;
		switch (*thisarg)
		{
#ifndef MIPS
		case 'd':
			--argc;
			argv++;
			sprintf(avr_device, "%s", *argv);
			break;
		case 'i':
			--argc;
			i_debug = 2;
			break;
#endif
		case 'c':
			--argc;
			i_debug = 1;
			break;
		case 'v':
			--argc;
			printf("%s", strVersion);
			exit(0);
			break;
		case 'e':
			--argc;
			em_mode = 1;
			break;
		}
		argc--;
		argv++;
	}

	if (!i_debug)
	{
		/* Run in background? */
		if(daemon(0, 0) != 0)
		{
			exit(-1);
		}
	}
	else if (1 == i_debug)
		check_timer(0);

	
	/* Set up termination handlers */
	signal(SIGTSTP, SIG_IGN); /* ignore tty signals */
	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGTERM, termination_handler);
	signal(SIGCONT, termination_handler);
	signal(SIGINT, termination_handler);

	/* Specified port? */
	if(open_serial(avr_device))
	{
		exit(-3);
	}

#ifndef MIPS
	if (i_debug > 1)
	{
		close(i_FileDescriptor);
		exit(0);
	}
#endif

	/* make child session leader */
	setsid();

	/* clear file creation mask */
	umask(0);

	/* Open logger for this daemon */
	openlog("avr-daemon", LOG_PID | LOG_NOWAIT | LOG_CONS, LOG_WARNING);

	syslog(LOG_INFO,"%s", strVersion);
	
	/* Our main */
	avr_evtd_main();

	return 0;
}

static void errorReport(int errorNumber)
{
	execute_command(ERRORED, errorNumber);
}

static char check_disk(void)
{
	/* Check that the filesystem is intact and we have at least DISKCHECK% spare capacity 
	NOTE: DISK FULL LED may flash during a disk check as /dev/hda3 mount check will not
	be available, this is not an error and light will extinguish once volume has been located */
	static char c_FirstTime=0;
	static char strRoot[16];
	static char strWorking[16];
	struct statfs mountfs;
	char bFull=0;
	int errno;
	int total=0;
	int total2=0;
	char cmd;
	char* pos;
	int file;
	int iRead;
	int i;
	char buff[4096];

	/* First time then determine paths */
	if (c_FirstTime < diskCheckNumber)
	{
		c_FirstTime = 0;
		/* Get list of mounted devices */
		file = open("/etc/mtab", O_RDONLY);

		/* Read in the mounted devices */
		if (file)
		{
			iRead = read(file, buff, 4095);

			pos = strtok(buff, " \n");
			if (iRead > 0)
			{
				for(i=0;i<60;i++)
				{
					cmd = -1;

					if (strcasecmp(pos, rootPartition) == 0) cmd = 0;
					else if (strcasecmp(pos, workingPartition) == 0) cmd = 1;
					
					pos = strtok(NULL, " \n");
					if (!pos)
						break;

					/* Increment firsttime check, with bad restarts, /dev/hda3 may not be mounted yet (running a disk check) */
					switch(cmd)
					{
						case 0: sprintf(strRoot, "%s", pos); c_FirstTime ++; break;
						case 1: sprintf(strWorking, "%s", pos); c_FirstTime ++; break;
					}
				}
			}
		}
		close(file);
	}

	/* Only perform these tests if DISKCHECK is enabled and partition's havev been defined */
	if (i_checkPercentage > 0 && diskCheckNumber > 0)
	{
		errno = -1;
		/* Ensure root and/or working paths have been located */
		if (diskCheckNumber == c_FirstTime)
		{
			/* Check mount directory */
			if (strlen(strRoot)>0)
			{
				errno = statfs(strRoot, &mountfs);
				/* This is okay for ext2/3 but may not be correct for other formats */
				if (0 == errno)
				{
					total = 100 - (int)((((double)mountfs.f_bavail/(double)mountfs.f_blocks)*100.0f)+0.99);

					if (total >= i_checkPercentage)
					{
						bFull = 1;
					}
				}
			}

			if (strlen(strWorking)>0)
			{
				/* Check root */
				errno = statfs(strWorking, &mountfs);
				if (0 == errno)
				{
					total2 = 100 - (int)((((double)mountfs.f_bavail/(double)mountfs.f_blocks)*100.0f)+0.99);

					if (total2 >= i_checkPercentage)
					{
						bFull = 1;
					}
				}
			}
		}

		/* Ensure device is mounted */
		if (0 != errno)
		{
			/* Indicate the /mnt is not available */
			writeUART(0x59);
		}
	}

	diskUsed = total2;
	if (total > total2)
		diskUsed = total;

	return bFull;
}

#ifndef NO_MELCO

#ifndef MIPS
static void parse_timer(char* buff)
{
	/* Parse our time requests */
	int offHour, offMinutes, onHour, onMinutes;
	long offTime=-1, onTime=-1;

	/* Parse the data for breakdown later */
	if (sscanf(buff, "on<>%02d:%02d<>%02d:%02d", &offHour, &offMinutes, &onHour, &onMinutes))
	{
		i_TimerFlag = 1;
		offTime = (offHour * 60) + offMinutes;
		onTime = (onHour * 60) + onMinutes;
	}
	else if (sscanf(buff, "off<>%02d:%02d<>%02d:%02d",  &offHour, &offMinutes, &onHour, &onMinutes))
	{
		i_TimerFlag = 0;
	}
	else
	{
		i_TimerFlag = 0;
	}

	if (1 == i_TimerFlag)
	{
		iOffTime = offTime;
		iOnTime = onTime;
	}
}
#endif

#endif

static void parse_avr(char* buff)
{
	/* Parse the /etc/default/avr_evtd file
	Valid options are listed in the command definition below */
	const char *command[] = {
			"TIMER",
			"SHUTDOWN",
			"OFF",
			"POWERON",
			"ON",
			"DISKCHECK",
			"REFRESH",
			"HOLD",
			"SUN", "MON", "TUE", "WED", "THR", "FRI", "SAT",
			"DISKNAG",
			"FANSTOP",
			"ROOT",
			"WORK"
			};

	char* pos;
	char* last; /* Used by strtoc_r to point to current token */
	int i,j;
	int cmd;
	int iHour;
	int iMinutes;
	int iGroup = 0;
	int ilastGroup = 0;
	int iFirstDay=-1;
	int iProcessDay=-1;
	TIMER* pTimer;
	TIMER* pOff;
	TIMER* pOn;

	/* Parse our time requests */
	pos = strtok_r(buff, ",=\n", &last);

	/* Destroy the macro timer objects, if any */
	destroyObject(poffTimer);
	destroyObject(ponTimer);

	/* Now create our timer objects for on and off events */
	pOn = ponTimer = calloc(sizeof(TIMER), sizeof(char));
	pOff = poffTimer = calloc(sizeof(TIMER), sizeof(char));

	/* Establish some defaults */
	pesterMessage = 0;
	i_TimerFlag = 0;
	refreshRate = 40;
	holdCycle = 3;
	diskCheckNumber = 0;
	
	/* To prevent looping */
	for (i=0;i<200;i++)
	{
		cmd = -1;

		/* Ignore comment lines? */
		if ('#' != pos[0])
		{
			/* Could return groups, say MON-THR, need to strip '-' out */
			if ('-' == pos[3])
			{
				*(last-1)=(char)'='; /* Plug the '0' with token parameter  */
				iGroup = 1;
				last-=8;
				pos = strtok_r(NULL, "-", &last);
			}

			/* Locate our expected commands */
			for(cmd=0;cmd<19;cmd++)
				if (strcasecmp(pos, command[cmd]) == 0) break;

			pos = strtok_r(NULL, ",=\n", &last);
		}
		else
		{
			pos = strtok_r(NULL, "\n", &last);

			/* After the first remark we have ignored, make sure we detect a valid line
			and move the tokeniser pointer if none remark field */
			if ('#' != pos[0])
			{
				j = strlen(pos);
				*(last-1)=(char)','; /* Plug the '0' with token parameter  */
				last=last-(j+1);

				/* Now lets tokenise this valid line */
				pos = strtok_r(NULL, ",=\n", &last);
			}
		}

		if (!pos)
			break;

		if ('#' == pos[0]) cmd = -1;

		/* Now parse the setting */
		/* Excuse the goto coding, not nice but necessary here */
		switch (cmd)
		{
			/* Timer on/off? */
			case 0: 
					if (strcasecmp(pos, "ON") == 0)
							i_TimerFlag = 1;
					break;

			/* Shutdown? */
			case 1: 
					pTimer = pOff;
					iHour = iMinutes = -1;
					goto process;

			/* Macro OFF? */
			case 2:
					pTimer = pOff; iHour = 24; iMinutes = 0;
					goto process;

			/* Power-on? */
			case 3: 
					pTimer = pOn;
					iHour = iMinutes = -1;
					goto process;

			/* Macro ON? */
			case 4:
					pTimer = pOn; iHour = iMinutes = 0;
process:
					if (!sscanf(pos, "%02d:%02d", &iHour, &iMinutes))
						i_TimerFlag = -1;

					/* Ensure time entry is valid */
					else if ((iHour>=0 && iHour <=24) && (iMinutes >=0 && iMinutes <= 59))
					{
						/* Valid macro'd OFF/ON entry? */
						if (2 == cmd || 4 == cmd)
						{
							/* Group macro so create the other events */
							if (iGroup!=0)
							{
								j = iFirstDay-1;
								/* Create the multiple entries for each day in range specified */
								while (j!=iProcessDay)
								{
									j++;
									if (j>7) j = 0;
									pTimer->day = j;
									pTimer->time = (iHour*60)+iMinutes;
									/* Allocate space for the next event object */
									pTimer->pointer = (void*)calloc(sizeof(TIMER), sizeof(char));
									pTimer = (TIMER*)pTimer->pointer;
								}
							}
							else
							{
								pTimer->day = iProcessDay;
								pTimer->time = (iHour*60)+iMinutes;
								/* Allocate space for the next event object */
								pTimer->pointer = (void*)calloc(sizeof(TIMER), sizeof(char));
								pTimer = (TIMER*)pTimer->pointer;
							}
						}

						/* Now handle the defaults */
						else if (1 == cmd) iOffTime = (iHour*60)+iMinutes;
						else if (3 == cmd) iOnTime = (iHour*60)+iMinutes;
					}
					else
						i_TimerFlag = -1;

					/* Update our pointers */
					if (cmd < 3) pOff = pTimer;
					else pOn = pTimer;

					break;

			/* Disk check percentage? */
			case 5: 
					if (!sscanf(pos, "%d", &i_checkPercentage)) i_checkPercentage = -1;
					/* Ensure valid percentage range */
					else if (i_checkPercentage > 100) i_checkPercentage = 100;
					else if (i_checkPercentage < 0) i_checkPercentage = -1;
					break;

			/* Refresh/re-scan time? */
			case 6: 
					if (sscanf(pos, "%03d", &refreshRate)) refreshRate = 40;
					/* Limit to something sensible */
					else if (refreshRate > FIVE_MINUTES) refreshRate = FIVE_MINUTES;
					else if (refreshRate < 10) refreshRate = 1;
					break;

			/* Button hold-in time? */
			case 7: 
					if (sscanf(pos, "%02d", &holdCycle)) holdCycle = HOLD_SECONDS;
					/* Limit to something sensible */
					else if (holdCycle > 10) holdCycle = 10;
					else if (holdCycle < 2) holdCycle = 2;
					break;

			/* Macro days in week? */
			case 8:
			case 9:
			case 10:
			case 11:
			case 12:
			case 13:
			case 14:
				/* For groups, */
				iProcessDay = cmd-8;
				/* Remove grouping flag for next defintion */
				ilastGroup += iGroup;
				if  (ilastGroup>2)
				{
					iGroup = 0;
					ilastGroup = 0;
				}

				if (1 == ilastGroup)
					iFirstDay = iProcessDay;

				break;

			case 15: 
				if (strcasecmp(pos, "ON") == 0)
					pesterMessage = 1;
					
			/* Fan failure stop time before event trigger */
			case 16:
					if (strcasecmp(pos, "OFF") == 0)
						fanFaultSeize = 0;
					else
					{
						if (sscanf(pos, "%02d", &fanFaultSeize)) fanFaultSeize = FAN_SEIZE_TIME;
						/* Limit to something sensible */
						else if (fanFaultSeize > 60) fanFaultSeize = 60;
						else if (fanFaultSeize < 1) fanFaultSeize = 1;
					}
					break;
			
				break;

			/* Specified partiton names */
			case 17:
			case 18:
				if (strlen(pos)<=5)
				{
					diskCheckNumber++;

					/* Specified ROOT partiton */
					if (17 == cmd)
						sprintf(rootPartition, "/dev/%s", pos);
					else
					/* Specified WORKING partiton */
						sprintf(workingPartition, "/dev/%s", pos);
				}
				break;
		}
	}

	if (i_TimerFlag <0)
	{
		i_TimerFlag = 0;
		errorReport(3);
	}
}

static void destroyObject(TIMER* pTimer)
{
	/* Destroy this object by free-ing up the memory we grabbed through calloc */
	TIMER* pObj;

	/* Ensure valid pointer */
	if (pTimer)
	{
		/* Bad this, can loop but let's destroy and free our objects */
		for(;;)
		{
			pObj = pTimer->pointer;
			if (NULL == pObj)
				break;
			free (pTimer);
			pTimer = NULL;
			pTimer = pObj;
		}

		pTimer = NULL;
	}
}

static int FindNextToday(long timeNow, TIMER* pTimer, long* time)
{
	/* Scan macro objects for a valid event from 'time' today */
	int iLocated = 0;

	while(pTimer != NULL && pTimer->pointer != NULL)
	{
		/* Next event for today?, at least 1 minute past current */
		if (pTimer->day == last_day && pTimer->time > timeNow)
		{
			iLocated = 1;
			*time = pTimer->time;
			pTimer = NULL;
		}
		else
		{
			pTimer = pTimer->pointer;
		}
	}

	return iLocated;
}

static int FindNextDay(long timeNow, TIMER* pTimer, long* time, long* offset)
{
	/* Locate the next valid event */
	int iLocated = 0;

	while(pTimer != NULL && pTimer->pointer != NULL)
	{
		/* Next event for tomorrow onwards? */
		if (pTimer->day > last_day)
		{
			/* Grouped events?, ie only tomorrow */
			if (pTimer->day > last_day)
				*offset = (pTimer->day - last_day) * TWENTYFOURHR;

			iLocated = 1;
			*time = pTimer->time;
			pTimer = NULL;
		}
		else
		{
			pTimer = pTimer->pointer;
		}
	}

	return iLocated;
}

static void GetTime(long timeNow, TIMER* pTimerLocate, long* time, long defaultTime)
{
	/* Get next timed macro event */
	long lOffset=0;
	char onLocated=0;
	TIMER* pTimer;

	/* Ensure that macro timer object is valid */
	if (pTimerLocate && pTimerLocate->pointer != NULL)
	{
		lOffset = 0;
		pTimer = pTimerLocate;
		/* Next event for today */
		onLocated = FindNextToday(timeNow, pTimer, time);

		/* Failed to find a time for today, look for the next power-up time */
		if (0 == onLocated)
		{
			pTimer = pTimerLocate;
			onLocated = FindNextDay(timeNow, pTimer, time, &lOffset);
		}

		/* Nothing for week-end, look at start */
		if (0 == onLocated)
		{
			*time = pTimerLocate->time;
			lOffset = ((6 - last_day) + pTimerLocate->day) * TWENTYFOURHR;
		}

		*time += lOffset;

		if (lOffset > TWENTYFOURHR && defaultTime > 0)
			*time = defaultTime;
	}
	else
		*time = defaultTime;
}

#ifndef NO_MELCO

#ifdef MIPS
static void parse_mips(char* buff)
{
/*
	type=off
	backup_status=off
	backup_time=0:00
	backup_type=day
	backup_week=Sun
	backup_overwrite=off
	sleep_start=0:00
	sleep_finish=9:00
	*/
	const char *command[] = {"type", "backup_time", "sleep_start", "sleep_finish"};
	char* pos;
	int i;
	int cmd=0;
	int backupHour, backupMinutes, offHour, offMinutes, onHour, onMinutes;
	long offTime, onTime;

	i_TimerFlag = 0;

	/* Parse our time requests */
	pos = strtok(buff, "=\n");

	/* Parse the data for breakdown later */
	for (i=0;i<16;i++)
	{
		cmd = -1;
		/* Locate our expected commands */
		for(cmd=0;cmd<4;cmd++)
			if (strcasecmp(pos, command[cmd]) == 0) break;

		pos = strtok(NULL, "=\n");
		if (!pos)
			break;

		/* Now parse the setting
			type is sleep otherwise deemed off
			backup_time expects HH:MM NOT CURRENTLY SUPPORTED
			sleep_start expects HH:MM if not, then timer is disabled
			sleep_finish expects HH:MM if not, then timer is disabled
			*/
		switch (cmd)
		{
			case 0: if (strcasecmp(pos, "sleep") == 0) i_TimerFlag = 1; break;
			case 1: sscanf(pos, "%02d:%02d", &backupHour, &backupMinutes); break;
			case 2: if (!sscanf(pos, "%02d:%02d", &offHour, &offMinutes)) i_TimerFlag = -1; break;
			case 3: if (!sscanf(pos, "%02d:%02d", &onHour, &onMinutes)) i_TimerFlag = -1; break;
		}
	}

	/* Failed timer needs timer=sleep and both off time and on time to be specified */
	if (1 == i_TimerFlag)
	{
		offTime = (offHour * 60) + offMinutes;
		onTime = (onHour * 60) + onMinutes;

		iOffTime = offTime;
		iOnTime = onTime;
	}
	else if (i_TimerFlag != 0)
	{
		i_TimerFlag = 0;
		errorReport(4);
	}
}

#endif

#endif

static void set_avr_timer(char type)
{
	/* Determine shutdown/power up time and fire relevant string update to the AVR */
	const char *strMessage[]={"file update", "re-validation", "clock skew"};
	long current_time, wait_time;
	time_t ltime, ttime;
	struct tm* decode_time;
	char message[80];
	char strAVR;
	char twelve;
	int i;
	long mask=0x800;
	long offTime, onTime;

	/* Timer enabled? */
	if (i_TimerFlag)
	{
		/* Get time of day */
		time(&ltime);

		decode_time = localtime(&ltime);
		current_time = (decode_time->tm_hour*60) + decode_time->tm_min;
		last_day = decode_time->tm_wday;

		GetTime(current_time, poffTimer, &offTime, iOffTime);
		/* Correct search if switch-off is tommorrow */
		if (offTime>TWENTYFOURHR)
			GetTime(current_time, ponTimer, &onTime, iOnTime);
		else
			GetTime(offTime, ponTimer, &onTime, iOnTime);

		/* Protect for tomorrows setting */
		twelve = 0;
		if (offTime < current_time)
		{
			twelve = 1;
			l_ShutdownTimer = ((TWELVEHR + (offTime - (current_time - TWELVEHR))) * 60);
		}
		else
		{
			l_ShutdownTimer = ((offTime - current_time) * 60);
		}

		/* Remeber the current seconds passed the minute */
		l_ShutdownTimer-=decode_time->tm_sec;
		
		ttime = ltime + l_ShutdownTimer;
		decode_time = localtime(&ttime);
		
		sprintf(message, "Timer is set with %02d/%02d %02d:%02d", 
			decode_time->tm_mon+1, decode_time->tm_mday, decode_time->tm_hour, decode_time->tm_min);

		/* Now setup the AVR with the power-on time */

		/* Correct to AVR oscillator */
		if (onTime < current_time)
		{
			wait_time = (TWELVEHR + (onTime - (current_time - TWELVEHR))) * 60;
			onTime = ((TWELVEHR + (onTime - (current_time - TWELVEHR))) * 100)/112;
		}
		else
		{
			if (onTime < (offTime-TWENTYFOURHR))
				onTime += TWENTYFOURHR;
			else if (onTime < offTime)
				onTime += TWENTYFOURHR;

			wait_time = (onTime - current_time) * 60;
			onTime = ((onTime - current_time) * 100)/112;
		}

		/* Limit max off time to next power-on to the resolution of the timer */
		if (onTime > TIMER_RESOLUTION && (onTime-(l_ShutdownTimer/60)) > TIMER_RESOLUTION)
		{
			wait_time -= ((onTime - TIMER_RESOLUTION)*672)/10;
			errorReport(2);
			/* Reset to timer resolution */
			onTime = TIMER_RESOLUTION;
		}

		ttime = ltime + wait_time;
		decode_time = localtime(&ttime);

		sprintf(message, "%s-%02d/%02d %02d:%02d (Following timer %s)", message,
			decode_time->tm_mon+1, decode_time->tm_mday, decode_time->tm_hour, decode_time->tm_min,
			strMessage[(int)type]);

		syslog(LOG_INFO, message);

		/* Now tell the AVR we are updating the 'on' time */
		writeUART(0x3E);
		writeUART(0x3C);
		writeUART(0x3A);
		writeUART(0x38);

		/* Bit pattern (12-bits) detailing time to wake */
		for (i=0;i<12;i++)
		{
			strAVR = (onTime&mask ? 0x21 : 0x20) + ((11-i)*2);
			mask >>=1;

			/* Output to AVR */
			writeUART(strAVR);
		}

		/* Complete output and set LED state (power) to pulse */
		writeUART(0x3F);
		keepAlive = 0x5B;
	}
	/* Inform AVR its not in timer mode */
	else
	{
		writeUART(0x3E);
		keepAlive = 0x5A;
	}

	writeUART(keepAlive);
}

static int check_timer(char type)
{
	/* Check to see if we need to perform an update.
	We check the original melco timer file (timer_sleep) for timer
	requests if required */
	int iReturn = 1;
	int iRead;
	int errno;
	char buff[4096];
	int file;
	struct stat filestatus;

#ifndef NO_MELCO
	/* Expect its a file time read required? 
	This is purely for legacy timer files only.  If this does not
	exist, then we look for our default */
	if (0 == c_CommandLineUpdate)
	{
		c_CommandLineUpdate = 1;

		/* Get file status of sleep timer file */
#ifdef MIPS
		errno = stat("/etc/melco/timer_status", &filestatus);
#else
		errno = stat("/etc/melco/timer_sleep", &filestatus);
#endif

		/* If exists? */
		if (0 == errno)
		{
			/* Has this file changed? */
			if (filestatus.st_mtime != tt_LastMelcoAcess)
			{
				iRead = -1;

				/* Open and read the contents */
#ifdef MIPS
				file = open("/etc/melco/timer_status", O_RDONLY);

				if (file)
				{
					iRead = read(file, buff, 254);

					/* Dump the file pointer for others */
					close(file);

					if (iRead >0)
						parse_mips(buff);
				}
#else
				file = open("/etc/melco/timer_sleep", O_RDONLY);

				if (file)
				{
					iRead = read(file, buff, 31);

					/* Dump the file pointer for others */
					close(file);

					if (iRead >0)
						parse_timer(buff);
				}
#endif
				if (iRead >0)
				{
					/* Return flag */
					iReturn = c_CommandLineUpdate = 0;
					set_avr_timer(type);
				}
			}
			else
				c_CommandLineUpdate = 0;

			/* Update our lasttimes timer file access */
			tt_LastMelcoAcess = filestatus.st_mtime;
		}
		/* standard Melco files do not exist, back to the avr_evtd defaults */
		else
		{
			tt_LastMelcoAcess = 0;
			c_CommandLineUpdate = 1;
		}
	}
#endif

	/* Time from avr_evtd configuration file */
	if (1 == c_CommandLineUpdate)
	{
		/* File is missing so default to off and do not do this again */
		c_CommandLineUpdate = 2;

		errno = stat("/etc/default/avr_evtd", &filestatus);

		/* If exists? */
		if (0 == errno)
		{
			/* Has this file changed? */
			if (filestatus.st_mtime != tt_LastMelcoAcess)
			{
				file = open("/etc/default/avr_evtd", O_RDONLY);

				if (file)
				{
					iRead = read(file, buff, 4095);

					/* Dump the file pointer for others */
					close(file);

					if (iRead>0)
					{
						/* Return flag */
						iReturn = 0;
						c_CommandLineUpdate = 1;
						parse_avr(buff);
						set_avr_timer(type);
					}
				}
			}
			else
				c_CommandLineUpdate = 1;

			/* Update our lasttimes timer file access */
			tt_LastMelcoAcess = filestatus.st_mtime;
		}
	}

	/* Ensure that if we have any configuration errors we at least set timer off */
	if (2 == c_CommandLineUpdate)
	{
		c_CommandLineUpdate = 3;
		set_avr_timer(type);
		errorReport(1);
	}

	return iReturn;
}
