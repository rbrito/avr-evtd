/*
 * @file avr-evtd.c
 *
 * Linkstation AVR daemon
 *
 * Copyright © 2006 Bob Perry <lb-source@users.sf.net>
 * Copyright © 2008-2011 Rogério Brito <rbrito@users.sf.net>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 *
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
#define HOLD_TIME		1
#define HOLD_SECONDS		3
#define FIVE_MINUTES		(5*60)
#define TWELVEHR		(12*60)
#define TWENTYFOURHR		(TWELVEHR*2)
#define TIMER_RESOLUTION	4095
#define FAN_SEIZE_TIME		30
#define EM_MODE_TIME		20
#define SP_MONITOR_TIME		10

/* Event message definitions */
#define SPECIAL_RESET		'0'
#define AVR_HALT		'1'
#define TIMED_SHUTDOWN		'2'
#define POWER_RELEASE		'3'
#define POWER_PRESS		'4'
#define RESET_RELEASE		'5'
#define RESET_PRESS		'6'
#define USER_POWER_DOWN		'7'
#define USER_RESET		'8'
#define DISK_FULL		'9'
#define FAN_FAULT		'F'
#define EM_MODE			'E'
#define FIVE_SHUTDOWN		'S'
#define ERRORED			'D'

/* Constants for readable code */
#define COMMENT_PREFIX		'#'
#define CONFIG_FILE_LOCATION	"/etc/default/avr-evtd"
#define VERSION			"Linkstation/Kuro AVR daemon 1.7.7\n"
#define CMD_LINE_LENGTH		64

/* Macro event object definition */
struct event {
	int day;		/* Event day */
	long time;		/* Event time (24hr) */
	struct event *next;	/* Pointer to next event */
};

typedef struct event event;

/* Variables and macros that depend on the architecture */
#ifdef MIPS
#define STD_DEVICE	"/dev/ttyS0"
#else
#define STD_DEVICE	"/dev/ttyS1"
#endif

static char avr_device[] = STD_DEVICE;

event *off_timer = NULL;
event *on_timer = NULL;
int serialfd = 0;
time_t LastMelcoAccess = 0;
int TimerFlag = 0;
long ShutdownTimer = 9999;	/* Careful here */
char FirstTimeFlag = 1;
char FirstWarning = 1;
long OffTime = -1;		/* Default, NO defaults */
long OnTime = -1;		/* Default, NO defaults */

char CommandLineUpdate = 1;

int check_pct = 90;
int last_day;
int refresh_rate = 40;
int hold_cycle = 3;
char debug = 0;
char pesterMessage = 0;
int fanFaultSeize = 30;
int checkState = 1;		/* Will force an update within 15
				 * seconds of starting up to resolve
				 * those pushed out refresh times */
char em_mode = 0;
char rootdev[10] = "";		/* root filesystem device */
char workdev[10] = "";		/* work filesystem device */
int diskCheckNumber = 0;
char keepAlive = 0x5B;		/* '[' */
char reset_presses = 0;
int diskUsed = 0;

/* Function declarations */
static void usage(void);
static int check_timer(int type);
static void termination_handler(int signum);
static int open_serial(char *device);

static inline void ensure_limits(int *value, int lower, int upper);

static void close_serial(void);
static void avr_evtd_main(void);
static char check_disk(void);
static void set_avr_timer(int type);
static void parse_avr(char *buff);
static void GetTime(long timeNow, event *pTimerLocate, long *time, long defaultTime);
static int FindNextToday(long timeNow, event *pTimer, long *time);
static int FindNextDay(event * pTimer, long *time, long *offset);
static void destroy_timer(event *e);
static void write_to_uart(char);
static void report_error(int number);
static void exec_simple_cmd(char cmd);
static void exec_cmd(char cmd, int cmd2);


/**
 * Print usage of the program.
 */
static void usage(void)
{
	printf("Usage: avr-evtd [OPTION...]\n"
#ifndef MIPS
	       "  -d DEVICE     listen for events on DEVICE\n"
	       "  -i            display memory location for device used with -d\n"
#endif
	       "  -c            run in the foreground, not as a daemon\n"
	       "  -v            display program version\n"
	       "  -h            display this usage notice\n");
	exit(0);
}


/**
 * Ensure that value lies in the interval [@a lower, @a upper]. To avoid
 * degenerate cases, we assume that @a lower <= @a upper.
 *
 * @param value Pointer to integer whose value is to be checked against limits.
 * @param lower Integer specifying the lowest accepted value.
 * @param upper Integer specifying the highest accepted value.
 */
static inline void ensure_limits(int* value, int lower, int upper)
{
	if (*value < lower) *value = lower;
	if (*value > upper) *value = upper;
}


/**
 * Write command to the UART.  The character received by the function is sent
 * to the UART 4 (four) times in a row.
 *
 * @param cmd The command to be sent to the UART.
 */
static void write_to_uart(char cmd)
{
	char output[4];
	output[0] = output[1] = output[2] = output[3] = cmd;
	write(serialfd, output, 4);
}


/**
 * Establish connection to serial port and initialise it.
 *
 * @param device A string containing the device to be used to communicate with
 * the UART.
 *
 * @return A negative value if problems were encountered and 0 otherwise.
 */
static int open_serial(char *device)
{
	struct termios newtio;

	/* Need read/write access to the AVR */
	serialfd = open(device, O_RDWR | O_NOCTTY);

	if (serialfd < 0) {
		perror(device);
		return -1;
	}

#ifndef MIPS
	/* Requested device memory address? */
	if (debug == 2) {
		struct serial_struct serinfo;
		ioctl(serialfd, TIOCGSERIAL, &serinfo);
		if (serinfo.iomem_base)
			printf("%p\n", serinfo.iomem_base);
		else
			printf("%X\n", serinfo.port);
		return 0;
	}
#endif

	ioctl(serialfd, TCFLSH, 2);
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
	ioctl(serialfd, TCSETS, &newtio);
	ioctl(serialfd, TCFLSH, 2);

	/* Initialise the AVR device: clear memory and reset the timer */
	write_to_uart(0x41); /* 'A' */
	write_to_uart(0x46); /* 'F' */
	write_to_uart(0x4A); /* 'J' */
	write_to_uart(0x3E); /* '>' */

	/* Remove flashing DISK LED */
	write_to_uart(0x58); /* 'X' */

	return 0;
}


/**
 *
 * Close the serial port associated with @a serialfd.
 *
 * Before the closing of the file descriptor, a command is sent to the UART
 * so that it will stop the watchdog timer. This is not necessary when
 * powering off the machine, but *is* important when the system
 * administrator has, for some reason, stopped the daemon for some kind of
 * maintenance.
 *
 * FIXME: The memory used by the linked list of events is freed. This should
 * probably be better to keep split in another function. Also, the file
 * descriptor used for syslog is closed (optional, but nice).  Perhaps the
 * function should just be renamed, instead of being split in many smaller
 * functions?
 */
static void close_serial(void)
{
	if (serialfd != 0) {
		/* Stop the watchdog timer */
#ifndef MIPS
		write_to_uart(0x4B); /* 'K' */
#endif
		close(serialfd);
	}

	/* Destroy the macro timer objects */
	destroy_timer(off_timer);
	destroy_timer(on_timer);

	closelog();
}


/**
 * Set up termination handlers when receiving signals.
 *
 * @param signum The number of the signal received by the daemon.
 *
 */
static void termination_handler(int signum)
{
	switch (signum) {
	case SIGTERM:
		close_serial();
		exit(EXIT_SUCCESS);
	default:
		break;
	}
}


/**
 * Execute event script handler (put in background in a shell) with the
 * commands passed as parameters.
 *
 * @param cmd1 First part of the command to the event script. A single character.
 * @param cmd2 Second part of the command to the event script. An integer.
 *
 */
static void exec_cmd(char cmd1, int cmd2)
{
	char cmd_line[CMD_LINE_LENGTH];

	sprintf(cmd_line, "/etc/avr-evtd/EventScript %c %s %d &",
		cmd1, avr_device, cmd2);
	system(cmd_line);
}


/**
 * Abbreviated version of the executioner of the event script handler.
 *
 * @param cmd Command to be sent to the event script. It must consist of a
 * single character.
 *
 */
static void exec_simple_cmd(char cmd)
{
	exec_cmd(cmd, 0);
}


/**
 * Our main entry, decode requests and monitor activity
 */
static void avr_evtd_main(void)
{
	char buf[17];
	char cmd;
	char pushedpower = 0;
	char pushedreset = 0;
	char PressedPowerFlag = 0;
	char PressedResetFlag = 0;
	char currentStatus = 0;
	time_t idle = time(NULL);
	time_t power_press = idle;
	time_t fault_time;
	time_t last_shutdown_ping;
	time_t time_now;
	fd_set fReadFS;
	struct timeval timeout_poll;
	int res;
	int fan_fault = 0;
	long time_diff;
	char extraTime = 0;
	char disk_full = 0;

	/* Update the shutdown timer */
	fault_time = 0;
	last_shutdown_ping = time(NULL);

	/* Loop whilst port is valid */
	while (serialfd) {
		timeout_poll.tv_usec = 0;
		res = refresh_rate;
		/* After file change or startup, update the time within
		 * 20 secs as the user may have pushed the refresh time
		 * out */
		if (checkState > 0) {
			res = 2;
		} else {
			/* Change our timer to check for a power/reset
			 * request need a faster poll rate here to see
			 * the double press event properly */
			if (pushedpower || pushedreset || FirstTimeFlag > 1) {
				timeout_poll.tv_usec = 250;
				res = 0;
				checkState = -2;
				/* Hold off any configuration file updates */
			}
		}

		if (checkState != -2) {
			/* Ensure we shutdown on the nail if the timer
			 * is enabled will be off slightly as timer
			 * reads are different */
			if (TimerFlag == 1) {
				if (ShutdownTimer < res)
					res = ShutdownTimer;
			}

			/* If we have a fan failure report, then ping
			 * frequently */
			if (fan_fault > 0)
				res = fan_fault == 6 ? fanFaultSeize : 2;
		}

		timeout_poll.tv_sec = res;

		FD_ZERO(&fReadFS);
		FD_SET(serialfd, &fReadFS);

		/* Wait for AVR message or time-out? */
		res = select(serialfd + 1, &fReadFS, NULL, NULL,
				 &timeout_poll);

		time_now = time(NULL);

		/* catch input? */
		if (res > 0) {
			/* Read AVR message */
			res = read(serialfd, buf, 16);
			/* AVR command detected so force to ping only */
			checkState = -2;

			switch (buf[0]) {
				/* power button release */
			case 0x20: /* ' ' */
				if (PressedPowerFlag == 0) {
					cmd = POWER_RELEASE;

					if ((time_now - power_press) <= HOLD_TIME && FirstTimeFlag < 2) {
						cmd = USER_RESET;
					} else if (ShutdownTimer < FIVE_MINUTES || FirstTimeFlag > 1) {
						if (FirstTimeFlag == 0)
							FirstTimeFlag = 10;

						ShutdownTimer += FIVE_MINUTES;
						FirstTimeFlag--;
						extraTime = 1;
					}

					exec_simple_cmd(cmd);
					power_press = time_now;
				}

				pushedpower = PressedPowerFlag = 0;
				break;

				/* power button push */
			case 0x21: /* '!' */
				exec_simple_cmd(POWER_PRESS);

				PressedPowerFlag = 0;
				pushedpower = 1;
				break;

				/* reset button release */
			case 0x22: /* '"' */
				if (PressedResetFlag == 0) {
					cmd = RESET_RELEASE;
					res = 0;

					/* Launch our telnet daemon */
					if ((time_now - power_press) <= HOLD_TIME) {
						cmd = SPECIAL_RESET;
						res = reset_presses;
						reset_presses++;
					}

					exec_cmd(cmd, res);
					power_press = time_now;
				}

				pushedreset = PressedResetFlag = 0;
				break;

				/* reset button push */
			case 0x23: /* '#' */
				exec_simple_cmd(RESET_PRESS);

				PressedResetFlag = 0;
				pushedreset = 1;
				break;

				/* Fan on high speed */
			case 0x24: /* '$' */
				fan_fault = 6;
				fault_time = time_now;
				break;

				/* Fan fault */
			case 0x25: /* '%' */
				/* Flag the EventScript */
				exec_cmd(FAN_FAULT, fan_fault);

				if (fanFaultSeize > 0) {
					fan_fault = 2;
					fault_time = time_now;
				} else
					fan_fault = -1;

				break;

				/* Acknowledge */
			case 0x30: /* '0' */
				break;

				/* AVR halt requested */
			case 0x31: /* '1' */
				close_serial();
				exec_simple_cmd(AVR_HALT);
				break;

				/* AVR initialisation complete */
			case 0x33: /* '3' */
				break;
#ifdef DEBUG
			default:
				if (buf[0] != 0)
					syslog(LOG_INFO, "unknown message %X[%d]",
					       buf[0], res);
				break;
#endif
			}

			/* Get time for use later */
			time(&idle);
		} else {	/* Time-out event */
			/* Check if button(s) are still held after
			 * holdcyle seconds */
			if ((idle + hold_cycle) < time_now) {
				/* Power down selected */
				if (pushedpower == 1) {
					/* Re-validate our time wake-up;
					 * do not perform if in extra
					 * time */
					if (!extraTime)
						set_avr_timer(1);

					exec_simple_cmd(USER_POWER_DOWN);

					pushedpower = 0;
					PressedPowerFlag = 1;
				}

			}
#ifndef UBOOT
			/* Has user held the reset button long enough to
			 * request EM-Mode? */
			if ((idle + EM_MODE_TIME) < time_now) {
				if (pushedreset == 1 && em_mode) {
					/* Send EM-Mode request to
					 * script.  The script handles
					 * the flash device decoding and
					 * writes the HDD no-good flag
					 * NGNGNG into the flash status.
					 * It then flags a reboot which
					 * causes the box to boot from
					 * ram-disk backup to recover
					 * the HDD */
					exec_simple_cmd(EM_MODE);

					pushedreset = 0;
					PressedResetFlag = 1;
				}
			}
#endif
			/* Skip this processing during power/reset scan */
			if (!pushedreset && !pushedpower && FirstTimeFlag < 2) {
				/* shutdown timer event? */
				if (TimerFlag == 1) {
					/* Decrement our powerdown timer */
					if (ShutdownTimer > 0) {
						time_diff = (time_now - last_shutdown_ping);

						/* If time difference is more than a minute,
						 * force a re-calculation of shutdown time */
						if (refresh_rate + 60 > abs(time_diff)) {
							ShutdownTimer -= time_diff;

							/* Within five
							 * minutes of
							 * shutdown? */
							if (ShutdownTimer < FIVE_MINUTES) {
								if (FirstTimeFlag) {
									FirstTimeFlag = 0;

									/* Inform the EventScript */
									exec_cmd(FIVE_SHUTDOWN,
									     ShutdownTimer);

									/* Re-validate out time
									   wake-up; do not perform
									   if in extra time */
									if (!extraTime)
										set_avr_timer(1);
								}
							}
						}
						/* Large clock drift,
						 * either user set time
						 * or an ntp update,
						 * handle
						 * accordingly. */
						else {
							check_timer(2);
						}
					} else {
						/* Prevent re-entry and
						 * execute command */
						pushedpower =  PressedResetFlag = 2;
						exec_simple_cmd(TIMED_SHUTDOWN);
					}
				}

				/* Keep track of shutdown time remaining */
				last_shutdown_ping = time(NULL);

				/* Split loading, handle disk checks
				 * over a number of cycles, reduce CPU hog */
				switch (checkState) {
					/* Kick state machine */
				case 0:
					checkState = 1;
					break;

					/* Check for timer change
					 * through configuration file */
				case 1:
					check_timer(0);
					checkState = 2;
					break;

					/* Check the disk and ping AVR accordingly */
				case -2:

					/* Check the disk to see if full
					 * and output appropriate AVR
					 * command? */
				case 2:
					cmd = keepAlive;

					if ((currentStatus = check_disk())) {
						/* Execute some user code on disk full */
						if (FirstWarning) {
							FirstWarning = pesterMessage;
							exec_cmd(DISK_FULL, diskUsed);
						}
					}

					/* Only update DISK LED on disk full change */
					if (disk_full != currentStatus) {
						/* LED status */
						cmd = 0x56;  /* 'V' */
						if (currentStatus)
							cmd++;
						else {
							FirstWarning = 0;
							exec_cmd(DISK_FULL, 0);
						}

						disk_full = currentStatus;
					}

					/* Ping AVR */
					write_to_uart(cmd);

					checkState = 3;
					break;

					/* Wait for next refresh kick */
				case 3:
					checkState = 0;
					break;
				}
			}

			/* Try and catch spurious fan fault messages */
			switch (fan_fault) {
			case -1:
				break;
			case 1:
				fan_fault = 0;
				break;
				/* Check how long we have been operating with a fan failure */
			case 2:
			case 3:
			case 4:
				if ((fault_time + fanFaultSeize) < time_now) {
					/* Run some user script on no
					 * fan restart message after
					 * FAN_FAULT_SEIZE time */
					exec_cmd(FAN_FAULT, 4);
					fan_fault = 5;
				}

				break;
				/* Fan sped up message received */
			case 6:
				/* Attempt to slow fan down again after 5 minutes */
				if ((fault_time + FIVE_MINUTES) < time_now) {
					write_to_uart(0x5C);  /* '\\' */
					fan_fault = 1;
				}

				break;
			}

			/* Check that the shutdown pause function (if
			 * activated) is still available, no then ping
			 * the delayed time */
			if ((power_press + SP_MONITOR_TIME) < time_now && FirstTimeFlag > 1) {
				/* Inform the EventScript */
				exec_cmd(FIVE_SHUTDOWN, (int) (ShutdownTimer/60.0));
				FirstTimeFlag = 1;
				power_press = 0;
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
	while (argc >= 1 && '-' == **argv) {
		thisarg = *argv;
		thisarg++;
		switch (*thisarg) {
#ifndef MIPS
		case 'd':
			--argc;
			argv++;
			sprintf(avr_device, "%s", *argv);
			break;
		case 'i':
			--argc;
			debug = 2;
			break;
#endif
		case 'c':
			--argc;
			debug = 1;
			break;
		case 'v':
			--argc;
			printf("%s", VERSION);
			exit(0);
		case 'e':
			--argc;
			em_mode = 1;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			usage();
		}
		argc--;
		argv++;
	}

	if (!debug) {
		/* Run in background? */
		if (daemon(0, 0) != 0) {
			exit(-1);
		}
	} else if (debug == 1)
		check_timer(0);

	/* ignore tty signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	/* Set up termination handlers */
	signal(SIGTERM, termination_handler);
	signal(SIGCONT, termination_handler);
	signal(SIGINT, termination_handler);

	/* Specified port? */
	if (open_serial(avr_device)) {
		exit(-3);
	}

#ifndef MIPS
	if (debug > 1) {
		close(serialfd);
		exit(0);
	}
#endif

	/* make child session leader */
	setsid();

	/* clear file creation mask */
	umask(0);

	/* Open logger for this daemon */
	openlog("avr-daemon", LOG_PID | LOG_NOWAIT | LOG_CONS, LOG_WARNING);

	syslog(LOG_INFO, "%s", VERSION);

	/* Our main */
	avr_evtd_main();

	return 0;
}


/**
 * Report errors. This function sends an error command to the UART.
 *
 * @param number An integer containing the number of the error to report.
 */
static void report_error(int number)
{
	exec_cmd(ERRORED, number);
}


/**
 * Check that the filesystem is intact and we have at least DISKCHECK%
 * spare capacity.
 *
 * NOTE: DISK FULL LED may flash during a disk check as /dev/hda3 mount
 * check will not be available, this is not an error and light will
 * extinguish once volume has been located
 */
static char check_disk(void)
{
	static char FirstTime = 0;
	static char root_mountpt[16];
	static char work_mountpt[16];
	struct statfs mountfs;
	char bFull = 0;
	int errno;
	int total = 0;
	int total2 = 0;
	char cmd;
	char *pos;
	int file;
	int iRead;
	int i;
	char buff[4096];

	/* First time then determine paths */
	if (FirstTime < diskCheckNumber) {
		FirstTime = 0;
		/* Get list of mounted devices */
		file = open("/etc/mtab", O_RDONLY);

		/* Read in the mounted devices */
		if (file) {
			iRead = read(file, buff, 4095);

			pos = strtok(buff, " \n");
			if (iRead > 0) {
				for (i = 0; i < 60; i++) {
					cmd = -1;

					if (strcasecmp(pos, rootdev) == 0)
						cmd = 0;
					else if (strcasecmp(pos, workdev) == 0)
						cmd = 1;

					pos = strtok(NULL, " \n");
					if (!pos)
						break;

					/* Increment firsttime check,
					 * with bad restarts, /dev/hda3
					 * may not be mounted yet
					 * (running a disk check) */
					switch (cmd) {
					case 0:
						sprintf(root_mountpt, "%s", pos);
						FirstTime++;
						break;
					case 1:
						sprintf(work_mountpt, "%s", pos);
						FirstTime++;
						break;
					}
				}
			}
		}
		close(file);
	}

	/* Only perform these tests if DISKCHECK is enabled and
	 * partition's havev been defined */
	if (check_pct > 0 && diskCheckNumber > 0) {
		errno = -1;
		/* Ensure root and/or working paths have been located */
		if (diskCheckNumber == FirstTime) {
			/* check root partition */
			if (strlen(root_mountpt) > 0) {
				errno = statfs(root_mountpt, &mountfs);
				/* This is okay for ext2/3 but may not
				 * be correct for other formats */
				if (0 == errno) {
					total = 100 - (int) ((100.0 * mountfs.f_bavail)/mountfs.f_blocks);

					if (total >= check_pct)
						bFull = 1;
				}
			}

			if (strlen(work_mountpt) > 0) {
				/* check work partition */
				errno = statfs(work_mountpt, &mountfs);
				if (0 == errno) {
					total2 = 100 - (int) ((100.0 * mountfs.f_bavail)/mountfs.f_blocks);
					if (total2 >= check_pct)
						bFull = 1;
				}
			}
		}

		/* Ensure device is mounted */
		if (0 != errno) {
			/* Indicate the /mnt is not available */
			write_to_uart(0x59); /* 'Y' */
		}
	}

	diskUsed = total2;
	if (total > total2)
		diskUsed = total;

	return bFull;
}


/**
 * Parse configuration file.
 *
 * @param buff A buffer containing the contents of the configuration file
 * (usually, /etc/default/avr-evtd).
 */
static void parse_avr(char *buff)
{
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

	char *pos;
	char *last;		/* Used by strtok_r to point to current token */
	int i, j;
	int cmd;
	int hour;
	int minutes;
	int iGroup = 0;
	int ilastGroup = 0;
	int first_day = -1;
	int process_day = -1;
	event *pTimer;
	event *pOff;
	event *pOn;

	/* Parse our time requests */
	pos = strtok_r(buff, ",=\n", &last);

	/* Destroy the macro timer objects, if any */
	destroy_timer(off_timer);
	destroy_timer(on_timer);

	/* Now create our timer objects for on and off events */
	pOn = on_timer = calloc(sizeof(event), sizeof(char));
	pOff = off_timer = calloc(sizeof(event), sizeof(char));

	/* Establish some defaults */
	pesterMessage = 0;
	TimerFlag = 0;
	refresh_rate = 40;
	hold_cycle = 3;
	diskCheckNumber = 0;

	/* To prevent looping */
	for (i = 0; i < 200; i++) {
		cmd = -1;

		if (pos[0] != COMMENT_PREFIX) {
			/* Could return groups, say MON-THR, need to
			 * strip '-' out */
			if ('-' == pos[3]) {
				*(last - 1) = '=';	/* Plug the '0' with token parameter  */
				iGroup = 1;
				last -= 8;
				pos = strtok_r(NULL, "-", &last);
			}

			/* Locate our expected commands */
			for (cmd = 0; cmd < 19; cmd++)
				if (strcasecmp(pos, command[cmd]) == 0)
					break;

			pos = strtok_r(NULL, ",=\n", &last);
		} else {
			pos = strtok_r(NULL, "\n", &last);

			/* After the first remark we have ignored, make
			 * sure we detect a valid line and move the
			 * tokeniser pointer if none remark field */
			if (pos[0] != COMMENT_PREFIX) {
				j = strlen(pos);
				*(last - 1) = ',';	/* Plug the '0' with token parameter  */
				last = last - (j + 1);

				/* Now lets tokenise this valid line */
				pos = strtok_r(NULL, ",=\n", &last);
			}
		}

		if (!pos)
			break;

		if (pos[0] == COMMENT_PREFIX)
			cmd = -1;

		/* Now parse the setting */
		/* Excuse the goto coding, not nice but necessary here */
		switch (cmd) {
			/* Timer on/off? */
		case 0:
			if (strcasecmp(pos, "ON") == 0)
				TimerFlag = 1;
			break;

			/* Shutdown? */
		case 1:
			pTimer = pOff;
			hour = minutes = -1;
			goto process;

			/* Macro OFF? */
		case 2:
			pTimer = pOff;
			hour = 24;
			minutes = 0;
			goto process;

			/* Power-on? */
		case 3:
			pTimer = pOn;
			hour = minutes = -1;
			goto process;

			/* Macro ON? */
		case 4:
			pTimer = pOn;
			hour = minutes = 0;
		process:
			if (!sscanf(pos, "%02d:%02d", &hour, &minutes))
				TimerFlag = -1;

			/* Ensure time entry is valid */
			else if ((hour >= 0 && hour <= 24)
				 && (minutes >= 0 && minutes <= 59)) {
				/* Valid macro'd OFF/ON entry? */
				if (cmd == 2 || cmd == 4) {
					/* Group macro so create the other events */
					if (iGroup != 0) {
						j = first_day - 1;
						/* Create the multiple
						 * entries for each day
						 * in range specified */
						while (j != process_day) {
							j++;
							if (j > 7)
								j = 0;
							pTimer->day = j;
							pTimer->time = (hour * 60) + minutes;
							pTimer->next = calloc(sizeof(event), sizeof(char));
							pTimer = pTimer->next;
						}
					} else {
						pTimer->day = process_day;
						pTimer->time = (hour * 60) + minutes;
						pTimer->next = calloc(sizeof(event), sizeof(char));
						pTimer = pTimer->next;
					}
				}

				/* Now handle the defaults */
				else if (cmd == 1)
					OffTime = (hour * 60) + minutes;
				else if (cmd == 3)
					OnTime = (hour * 60) + minutes;
			} else
				TimerFlag = -1;

			/* Update our pointers */
			if (cmd < 3)
				pOff = pTimer;
			else
				pOn = pTimer;

			break;

			/* Disk check percentage? */
		case 5:
			if (!sscanf(pos, "%d", &check_pct))
				check_pct = -1;
			ensure_limits(&check_pct, -1, 100);
			break;

			/* Refresh/re-scan time? */
		case 6:
			if (!sscanf(pos, "%03d", &refresh_rate))
				refresh_rate = 40;
			ensure_limits(&refresh_rate, 10, FIVE_MINUTES);
			break;

			/* Button hold-in time? */
		case 7:
			if (!sscanf(pos, "%02d", &hold_cycle))
				hold_cycle = HOLD_SECONDS;
			ensure_limits(&hold_cycle, 2, 10);
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
			process_day = cmd - 8;
			/* Remove grouping flag for next defintion */
			ilastGroup += iGroup;
			if (ilastGroup > 2) {
				iGroup = 0;
				ilastGroup = 0;
			}

			if (ilastGroup == 1)
				first_day = process_day;

			break;

		case 15:
			if (strcasecmp(pos, "ON") == 0)
				pesterMessage = 1;

			/* Fan failure stop time before event trigger */
		case 16:
			if (strcasecmp(pos, "OFF") == 0)
				fanFaultSeize = 0;
			else {
				if (!sscanf(pos, "%02d", &fanFaultSeize))
					fanFaultSeize = FAN_SEIZE_TIME;
				ensure_limits(&fanFaultSeize, 1, 60);
			}
			break;

		/* Specified partiton names */
		case 17:
		case 18:
			if (strlen(pos) <= 5) {
				diskCheckNumber++;
				if (cmd == 17)
					sprintf(rootdev, "/dev/%s", pos);
				else
					sprintf(workdev, "/dev/%s", pos);
			}
			break;
		}
	}

	if (TimerFlag < 0) {
		TimerFlag = 0;
		report_error(3);
	}
}


/**
 * Destroys time objects
 *
 * @param e A pointer to the event structure containing the head of the
 * linked list of events to be deleted.
 *
 */
static void destroy_timer(event *e)
{
	event *aux;

	while (e) {
		aux = e->next;
		free(e);
		e = aux;
	}
}


/**
 * Scan macro objects for a valid event from @a time today
 */
static int FindNextToday(long timeNow, event *cur, long *time)
{
	int found = 0;

	while (cur != NULL && !found) {
		/* Next event for today?, at least 1 minute past current */
		if (cur->day == last_day && cur->time > timeNow) {
			found = 1;
			*time = cur->time;
		}
		cur = cur->next;
	}

	return found;
}


/**
 * Find one next event in the linked list beginning with @a cur with day
 * strictly greater than @a last_day.
 *
 * @param cur Initial cell of a linked list of events.
 * @param time The time of the next event found, if any.
 * @param offset The difference in minutes between @a last_day and the day of
 * the event found by the function, if any.
 *
 * @return 1 if an event was found and 0 otherwise.
 */
static int FindNextDay(event *cur, long *time, long *offset)
{
	int found = 0;

	while (cur != NULL && !found) {
		if (cur->day > last_day) {
			*offset = (cur->day - last_day) * TWENTYFOURHR;
			*time = cur->time;
			found = 1;
		}
		cur = cur->next;
	}

	return found;
}


/**
 * Get next timed macro event.
 */
static void GetTime(long timeNow, event *pTimerLocate, long *time, long defaultTime)
{
	long lOffset = 0;
	char onLocated = 0;
	event *pTimer;

	/* Ensure that macro timer object is valid */
	if (pTimerLocate && pTimerLocate->next != NULL) {
		lOffset = 0;
		pTimer = pTimerLocate;
		/* Next event for today */
		onLocated = FindNextToday(timeNow, pTimer, time);

		/* Failed to find a time for today, look for the next
		 * power-up time */
		if (0 == onLocated) {
			pTimer = pTimerLocate;
			onLocated = FindNextDay(pTimer, time, &lOffset);
		}

		/* Nothing for week-end, look at start */
		if (0 == onLocated) {
			*time = pTimerLocate->time;
			lOffset = ((6 - last_day) + pTimerLocate->day) * TWENTYFOURHR;
		}

		*time += lOffset;

		if (lOffset > TWENTYFOURHR && defaultTime > 0)
			*time = defaultTime;
	} else
		*time = defaultTime;
}


/**
 * Determine shutdown/power up time and fire relevant string update to
 * the AVR.
 */
static void set_avr_timer(int type)
{
	const char *strMessage[] = { "file update", "re-validation", "clock skew" };
	long current_time, wait_time;
	time_t ltime, ttime;
	struct tm *decode_time;
	char message[80];
	char avr_cmd;
	int i;
	long mask = 0x800;
	long offTime, onTime;

	/* Timer enabled? */
	if (TimerFlag) {
		/* Get time of day */
		time(&ltime);

		decode_time = localtime(&ltime);
		current_time = (decode_time->tm_hour * 60) + decode_time->tm_min;
		last_day = decode_time->tm_wday;

		GetTime(current_time, off_timer, &offTime, OffTime);
		/* Correct search if switch-off is tommorrow */
		if (offTime > TWENTYFOURHR)
			GetTime(current_time, on_timer, &onTime, OnTime);
		else
			GetTime(offTime, on_timer, &onTime, OnTime);

		/* Protect for tomorrows setting */
		if (offTime < current_time) {
			ShutdownTimer =
			    ((TWELVEHR + (offTime - (current_time - TWELVEHR))) * 60);
		} else {
			ShutdownTimer = ((offTime - current_time) * 60);
		}

		/* Remeber the current seconds passed the minute */
		ShutdownTimer -= decode_time->tm_sec;

		ttime = ltime + ShutdownTimer;
		decode_time = localtime(&ttime);

		sprintf(message, "Timer is set with %02d/%02d %02d:%02d",
			decode_time->tm_mon + 1, decode_time->tm_mday,
			decode_time->tm_hour, decode_time->tm_min);

		/* Now setup the AVR with the power-on time */

		/* Correct to AVR oscillator */
		if (onTime < current_time) {
			wait_time =
			    (TWELVEHR + (onTime - (current_time - TWELVEHR))) * 60;
			onTime =
			    ((TWELVEHR + (onTime - (current_time - TWELVEHR))) * 100) / 112;
		} else {
			if (onTime < (offTime - TWENTYFOURHR))
				onTime += TWENTYFOURHR;
			else if (onTime < offTime)
				onTime += TWENTYFOURHR;

			wait_time = (onTime - current_time) * 60;
			onTime = ((onTime - current_time) * 100) / 112;
		}

		/* Limit max off time to next power-on to the resolution of the timer */
		if (onTime > TIMER_RESOLUTION
		    && (onTime - (ShutdownTimer / 60)) > TIMER_RESOLUTION) {
			wait_time -= ((onTime - TIMER_RESOLUTION) * 672) / 10;
			report_error(2);
			/* Reset to timer resolution */
			onTime = TIMER_RESOLUTION;
		}

		ttime = ltime + wait_time;
		decode_time = localtime(&ttime);

		/* FIXME: this has undefined behaviour and should be fixed -- rbrito */
		sprintf(message,
			"%s-%02d/%02d %02d:%02d (Following timer %s)",
			message, decode_time->tm_mon + 1,
			decode_time->tm_mday, decode_time->tm_hour,
			decode_time->tm_min, strMessage[type]);

		syslog(LOG_INFO, message);

		/* Now tell the AVR we are updating the 'on' time */
		write_to_uart(0x3E); /* '>' */
		write_to_uart(0x3C); /* '<' */
		write_to_uart(0x3A); /* ':' */
		write_to_uart(0x38); /* '8' */

		/* Bit pattern (12-bits) detailing time to wake */
		for (i = 0; i < 12; i++) {
			avr_cmd = (onTime & mask ? 0x21 : 0x20) + ((11 - i) * 2);
			mask >>= 1;

			/* Output to AVR */
			write_to_uart(avr_cmd);
		}

		/* Complete output and set LED state (power) to pulse */
		write_to_uart(0x3F); /* '?' */
		keepAlive = 0x5B; /* '[' */
	} else { 	/* Inform AVR its not in timer mode */
		write_to_uart(0x3E); /* '>' */
		keepAlive = 0x5A; /* 'Z' */
	}

	write_to_uart(keepAlive);
}


/**
 * Check to see if we need to perform an update.
 */
static int check_timer(int type)
{
	int retcode = 1;
	int iRead;
	char buff[4096];
	int file;
	struct stat filestatus;

	/* Time from avr-evtd configuration file */
	if (CommandLineUpdate == 1) {
		/* File is missing so default to off and do not do this
		 * again */
		CommandLineUpdate = 2;

		if (stat(CONFIG_FILE_LOCATION, &filestatus) == 0) {
			/* Has this file changed? */
			if (filestatus.st_mtime != LastMelcoAccess) {
				file = open(CONFIG_FILE_LOCATION, O_RDONLY);

				if (file) {
					iRead = read(file, buff, sizeof(buff)-1);

					/* Dump the file pointer for others */
					close(file);

					if (iRead > 0) {
						/* Return flag */
						retcode = 0;
						CommandLineUpdate = 1;
						parse_avr(buff);
						set_avr_timer(type);
					}
				}
			} else
				CommandLineUpdate = 1;

			/* Update our lasttimes timer file access */
			LastMelcoAccess = filestatus.st_mtime;
		} else {
			/* The file could not be stat'ed */
		}
	}

	/* Ensure that if we have any configuration errors we at least
	 * set timer off */
	if (CommandLineUpdate == 2) {
		CommandLineUpdate = 3;
		set_avr_timer(type);
		report_error(1);
	}

	return retcode;
}
