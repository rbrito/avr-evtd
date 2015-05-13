/*
 * @file avr-evtd.c
 *
 * Linkstation AVR daemon
 *
 * Copyright © 2006 Bob Perry <lb-source@users.sf.net>
 * Copyright © 2008-2015 Rogério Theodoro de Brito <rbrito@ime.usp.br>
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
#include <sys/mount.h>
#include <sys/statfs.h>
#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <linux/serial.h>

#include <cstdlib>


/* A few defs for later */
const int HOLD_TIME = 1;
const int HOLD_SECONDS = 3;
const int FIVE_MINUTES = (5*60);
const int TWELVEHR = (12*60);
const int TWENTYFOURHR = (TWELVEHR*2);
const int TIMER_RESOLUTION = 4095;
const int FAN_SEIZE_TIME = 30;
const int EM_MODE_TIME = 20;
const int SP_MONITOR_TIME = 10;

/* Event message definitions */
const unsigned char SPECIAL_RESET = '0';
const unsigned char AVR_HALT = '1';
const unsigned char TIMED_SHUTDOWN = '2';
const unsigned char POWER_RELEASE = '3';
const unsigned char POWER_PRESS = '4';
const unsigned char RESET_RELEASE = '5';
const unsigned char RESET_PRESS = '6';
const unsigned char USER_POWER_DOWN = '7';
const unsigned char USER_RESET = '8';
const unsigned char DISK_FULL = '9';
const unsigned char FAN_FAULT = 'F';
const unsigned char EM_MODE = 'E';
const unsigned char FIVE_SHUTDOWN = 'S';
const unsigned char ERRORED = 'D';

/* Constants for readable code */
const unsigned char COMMENT_PREFIX = '#';
#define CONFIG_FILE_LOCATION	"/etc/default/avr-evtd"
#define VERSION			"Linkstation/Kuro AVR daemon 1.7.7\n"
const int CMD_LINE_LENGTH = 64;

/* Macro event object definition */
struct event {
	int day;		/* Event day */
	long time;		/* Event time (24h) */
	struct event *next;	/* Pointer to next event */
};

typedef struct event event;

static char avr_device[] = "/dev/ttyS1";

event *off_timer;
event *on_timer;
int serialfd;
time_t last_config_mtime;
int timer_flag;
long shutdown_timer = 9999;	/* Careful here */
char first_time_flag = 1;
char first_warning = 1;
long off_time = -1;		/* Default, NO defaults */
long on_time = -1;		/* Default, NO defaults */

char command_line_update = 1;

int max_pct = 90;
int last_day;			/* Day of week.	[0-6] */
int refresh_rate = 40;
int hold_cycle = 3;
char pester_message;
int fan_fault_seize = 30;
int check_state = 1;	/* Will force an update within 15 seconds of starting
			   up to resolve those pushed out refresh times. */
char in_em_mode = 0;
char root_device[10];		/* root filesystem device */
char work_device[10];		/* work filesystem device */
int diskcheck_number;
char keep_alive = 0x5B;		/* '[' */
char reset_presses;
int pct_used;

/* Function declarations */
static void usage(void);
static void check_timer(int type);
static void termination_handler(int signum);
static int open_serial(char *device, char probe);

static void close_serial(void);
static void avr_evtd_main(void);
static char check_disk(void);
static void set_avr_timer(int type);
static void parse_config(char *content);
static void get_time(long now, event *pTimerLocate, long *time, long default_time);
static int find_next_today(long now, event *pTimer, long *time);
static int find_next_day(event * pTimer, long *time, long *offset);
static void destroy_timer(event *e);
static void write_to_uart(char);
static void report_error(int number);
static void exec_simple_cmd(char cmd);
static void exec_cmd(char cmd, int cmd2);


/**
 * Print usage of the program and terminate execution.
 */
static void usage(void)
{
	printf("Usage: avr-evtd [OPTION...]\n"
	       "  -d DEVICE     listen for events on DEVICE\n"
	       "  -i            display memory location for device used with -d\n"
	       "  -c            run in the foreground, not as a daemon\n"
	       "  -e            force the device to enter emergency mode\n"
	       "  -v            display program version\n"
	       "  -h            display this usage notice\n");
	exit(1);
}


/**
 * Ensure that value lies in the interval [@a lower, @a upper]. To avoid
 * degenerate cases, we assume that @a lower <= @a upper.
 *
 * @param value Pointer to integer whose value is to be checked against limits.
 * @param lower Integer specifying the lowest accepted value.
 * @param upper Integer specifying the highest accepted value.
 */
template <typename T>
static inline void ensure_limits(T &value, T lower, T upper)
{
	if (value < lower) value = lower;
	if (value > upper) value = upper;
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
 * Establish connection to serial port.
 *
 * @param device A string containing the device to be used to communicate
 * with the UART.
 *
 * @param probe The value 0 if we are opening the device for regular use or
 *              1 if we just want to probe the memory address associated
 *              with @a device.
 *
 * @return A negative value if problems were encountered while opening @a
 * device.
 */
static int open_serial(char *device, char probe)
{
	struct termios newtio;

	/* Need read/write access to the AVR */
	if ((serialfd = open(device, O_RDWR | O_NOCTTY)) < 0) {
		perror(device);
		return -1;
	}

	if (probe) {
		struct serial_struct serinfo;

		ioctl(serialfd, TIOCGSERIAL, &serinfo);

		if (serinfo.iomem_base)
			printf("%p\n", serinfo.iomem_base);
		else
			printf("%X\n", serinfo.port);

		return 0;
	}

	ioctl(serialfd, TCFLSH, 2);
	/* Clear data structures */
	memset(&newtio, 0, sizeof(newtio));
	newtio.c_iflag = PARMRK;
	newtio.c_oflag = OPOST;
	newtio.c_cflag = PARENB | CLOCAL | CREAD | CSTOPB | CS8 | B9600;

	/* Update tty settings */
	ioctl(serialfd, TCSETS, &newtio);
	ioctl(serialfd, TCFLSH, 2);

	/* Initialise the AVR device: clear memory and reset the timer */
	write_to_uart(0x41);	/* 'A' */
	write_to_uart(0x46);	/* 'F' */
	write_to_uart(0x4A);	/* 'J' */
	write_to_uart(0x3E);	/* '>' */

	/* Remove flashing DISK LED */
	write_to_uart(0x58);	/* 'X' */

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
		write_to_uart(0x4B);	/* 'K' */
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
	char pushed_power = 0;
	char pushed_reset = 0;
	char pressed_power_flag = 0;
	char pressed_reset_flag = 0;
	char current_status = 0;
	time_t idle = time(NULL);
	time_t power_press = idle;
	time_t fault_time;
	time_t last_shutdown_ping;
	fd_set serialfd_set;
	struct timeval timeout_poll;
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
		int res = refresh_rate;
		/* After file change or startup, update the time within 20 secs as the
		 * user may have pushed the refresh time out. */
		if (check_state > 0) {
			res = 2;
		} else {
			/* Change our timer to check for a power/reset request need a
			 * faster poll rate here to see the double press event
			 * properly. */
			if (pushed_power || pushed_reset || first_time_flag > 1) {
				timeout_poll.tv_usec = 250;
				res = 0;
				check_state = -2;
				/* Hold off any configuration file updates */
			}
		}

		if (check_state != -2) {
			/* Ensure we shutdown on the nail if the timer is enabled will
			 * be off slightly as timer reads are different */
			if (timer_flag == 1) {
				if (shutdown_timer < res)
					res = shutdown_timer;
			}

			/* If we have a fan failure report, then ping frequently */
			if (fan_fault > 0)
				res = fan_fault == 6 ? fan_fault_seize : 2;
		}

		timeout_poll.tv_sec = res;

		FD_ZERO(&serialfd_set);
		FD_SET(serialfd, &serialfd_set);

		/* Wait for AVR message or time-out? */
		res = select(serialfd + 1, &serialfd_set, NULL, NULL, &timeout_poll);

		time_t time_now = time(NULL);

		/* catch input? */
		if (res > 0) {
			/* Read AVR message */
			res = read(serialfd, buf, 16);
			/* AVR command detected so force to ping only */
			check_state = -2;

			switch (buf[0]) {
				/* power button release */
			case 0x20:	/* ' ' */
				if (pressed_power_flag == 0) {
					cmd = POWER_RELEASE;

					if ((time_now - power_press) <= HOLD_TIME && first_time_flag < 2) {
						cmd = USER_RESET;
					} else if (shutdown_timer < FIVE_MINUTES || first_time_flag > 1) {
						if (first_time_flag == 0)
							first_time_flag = 10;

						shutdown_timer += FIVE_MINUTES;
						first_time_flag--;
						extraTime = 1;
					}

					exec_simple_cmd(cmd);
					power_press = time_now;
				}

				pushed_power = pressed_power_flag = 0;
				break;

				/* power button push */
			case 0x21:	/* '!' */
				exec_simple_cmd(POWER_PRESS);

				pressed_power_flag = 0;
				pushed_power = 1;
				break;

				/* reset button release */
			case 0x22:	/* '"' */
				if (pressed_reset_flag == 0) {
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

				pushed_reset = pressed_reset_flag = 0;
				break;

				/* reset button push */
			case 0x23:	/* '#' */
				exec_simple_cmd(RESET_PRESS);

				pressed_reset_flag = 0;
				pushed_reset = 1;
				break;

				/* Fan on high speed */
			case 0x24:	/* '$' */
				fan_fault = 6;
				fault_time = time_now;
				break;

				/* Fan fault */
			case 0x25:	/* '%' */
				/* Flag the EventScript */
				exec_cmd(FAN_FAULT, fan_fault);

				if (fan_fault_seize > 0) {
					fan_fault = 2;
					fault_time = time_now;
				} else
					fan_fault = -1;

				break;

				/* Acknowledge */
			case 0x30:	/* '0' */
				break;

				/* AVR halt requested */
			case 0x31:	/* '1' */
				close_serial();
				exec_simple_cmd(AVR_HALT);
				break;

				/* AVR initialization complete */
			case 0x33:	/* '3' */
				break;
			default:
				syslog(LOG_INFO, "unknown message %X[%d]", buf[0], res);
				break;
			}

			/* Get time for use later */
			time(&idle);
		} else {	/* Time-out event */
			/* Check if button(s) are still held after holdcyle seconds */
			if ((idle + hold_cycle) < time_now) {
				/* Power down selected */
				if (pushed_power == 1) {
					/* Re-validate our time wake-up; do not perform if in extra time */
					if (!extraTime)
						set_avr_timer(1);

					exec_simple_cmd(USER_POWER_DOWN);

					pushed_power = 0;
					pressed_power_flag = 1;
				}

			}

			/* Has user held the reset button long enough to request EM-Mode? */
			if ((idle + EM_MODE_TIME) < time_now) {
				if (pushed_reset == 1 && in_em_mode) {
					/* Send EM-Mode request to script.  The script handles the
					 * flash device decoding and writes the HDD no-good flag
					 * NGNGNG into the flash status.  It then flags a reboot
					 * which causes the box to boot from ram-disk backup to
					 * recover the HDD.
					 */
					exec_simple_cmd(EM_MODE);

					pushed_reset = 0;
					pressed_reset_flag = 1;
				}
			}

			/* Skip this processing during power/reset scan */
			if (!pushed_reset && !pushed_power && first_time_flag < 2) {
				/* shutdown timer event? */
				if (timer_flag == 1) {
					/* Decrement our powerdown timer */
					if (shutdown_timer > 0) {
						time_diff = (time_now - last_shutdown_ping);

						/* If time difference is more than a minute,
						 * force a re-calculation of shutdown time */
						if (refresh_rate + 60 > labs(time_diff)) {
							shutdown_timer -= time_diff;

							/* Within five minutes of shutdown? */
							if (shutdown_timer < FIVE_MINUTES) {
								if (first_time_flag) {
									first_time_flag = 0;

									/* Inform the EventScript */
									exec_cmd(FIVE_SHUTDOWN, shutdown_timer);

									/* Re-validate out time wake-up; do not
									 * perform if in extra time */
									if (!extraTime)
										set_avr_timer(1);
								}
							}
						}
						/* Large clock drift, either user set time
						 * or an ntp update, handle accordingly. */
						else {
							check_timer(2);
						}
					} else {
						/* Prevent re-entry and execute command */
						pushed_power = pressed_reset_flag = 2;
						exec_simple_cmd(TIMED_SHUTDOWN);
					}
				}

				/* Keep track of shutdown time remaining */
				last_shutdown_ping = time(NULL);

				/* Split loading, handle disk checks
				 * over a number of cycles, reduce CPU hog */
				switch (check_state) {
					/* Kick state machine */
				case 0:
					check_state = 1;
					break;

					/* Check for timer change through configuration file */
				case 1:
					check_timer(0);
					check_state = 2;
					break;

					/* Check the disk and ping AVR accordingly */
				case -2:

					/* Check the disk to see if full and output appropriate
					 * AVR command? */
				case 2:
					cmd = keep_alive;

					if ((current_status = check_disk())) {
						/* Execute some user code on disk full */
						if (first_warning) {
							first_warning = pester_message;
							exec_cmd(DISK_FULL, pct_used);
						}
					}

					/* Only update DISK LED on disk full change */
					if (disk_full != current_status) {
						/* LED status */
						cmd = 0x56;	/* 'V' */
						if (current_status)
							cmd++;
						else {
							first_warning = 0;
							exec_cmd(DISK_FULL, 0);
						}

						disk_full = current_status;
					}

					/* Ping AVR */
					write_to_uart(cmd);

					check_state = 3;
					break;

					/* Wait for next refresh kick */
				case 3:
					check_state = 0;
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
				if ((fault_time + fan_fault_seize) < time_now) {
					/* Run some user script on no fan restart message after
					 * FAN_FAULT_SEIZE time */
					exec_cmd(FAN_FAULT, 4);
					fan_fault = 5;
				}

				break;
				/* Fan sped up message received */
			case 6:
				/* Attempt to slow fan down again after 5 minutes */
				if ((fault_time + FIVE_MINUTES) < time_now) {
					write_to_uart(0x5C);	/* '\\' */
					fan_fault = 1;
				}

				break;
			}

			/* Check that the shutdown pause function (if activated) is still
			 * available, no then ping the delayed time */
			if ((power_press + SP_MONITOR_TIME) < time_now && first_time_flag > 1) {
				/* Inform the EventScript */
				exec_cmd(FIVE_SHUTDOWN, shutdown_timer/60);
				first_time_flag = 1;
				power_press = 0;
			}
		}
	}
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
	static const int PART_NAME_SIZE = 16;
	static const int BUFF_SIZE = 4096;

	static char FirstTime = 0;
	static char root_mountpt[PART_NAME_SIZE];
	static char work_mountpt[PART_NAME_SIZE];
	struct statfs mountfs;

	int pct_root = 0;	/* percentage of the root fs that is used */
	int pct_work = 0;	/* percentage of the work fs that is used */

	char buff[BUFF_SIZE];

	enum { UNSET = -1, ROOT, WORK } device_type;

	if (FirstTime < diskcheck_number) {	/* first, determine paths from mtab */
		FirstTime = 0;

		int file = open("/etc/mtab", O_RDONLY);
		if (file < 0)
			goto err_not_avail;

		if (read(file, buff, BUFF_SIZE - 1) > 0) {
			char *pos = strtok(buff, " \n");
			for (int i = 0; i < 60; i++) {
				device_type = UNSET;

				if (strcasecmp(pos, root_device) == 0)
					device_type = ROOT;
				else if (strcasecmp(pos, work_device) == 0)
					device_type = WORK;

				pos = strtok(NULL, " \n");
				if (!pos)
					break;

				/* Increment firsttime check, with bad restarts,
				 * /dev/hda3 may not be mounted yet (running a disk check) */
				switch (device_type) {
				case ROOT:
					sprintf(root_mountpt, "%s", pos);
					break;
				case WORK:
					sprintf(work_mountpt, "%s", pos);
					break;
				default:
					break;
				}
				FirstTime++;
			}
		}
		close(file);
	}

	/* Only test when DISKCHECK is enabled and partitions are defined */
	/* FIXME: Is this kind of test correct for any kind of filesystem? */
	if (max_pct > 0 && diskcheck_number > 0) {
		if (diskcheck_number == FirstTime) {
			if (strlen(root_mountpt) > 0) {
				if (statfs(root_mountpt, &mountfs) == -1)
					goto err_not_avail;
				pct_root = 100 - ((100.0 * mountfs.f_bavail) / mountfs.f_blocks);
			}

			if (strlen(work_mountpt) > 0) {
				if (statfs(work_mountpt, &mountfs) == -1)
					goto err_not_avail;
				pct_work = 100 - ((100.0 * mountfs.f_bavail) / mountfs.f_blocks);
			}
		}
	}

	pct_used = (pct_root > pct_work) ? pct_root : pct_work;
	return (pct_used > max_pct);

err_not_avail:
	write_to_uart(0x59); /* 'Y' */
	return 0;
}


/**
 * Parse configuration file.
 *
 * @param content A string with the contents of the config file
 * (usually, /etc/default/avr-evtd).
 */
static void parse_config(char *content)
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

	enum cmd_code_t {
		INVALID = -1,
		TIMER,
		SHUTDOWN,
		OFF,
		POWERON,
		ON,
		DISKCHECK,
		REFRESH,
		HOLD,
		SUN, MON, TUE, WED, THR, FRI, SAT,
		DISKNAG,
		FANSTOP,
		ROOT,
		WORK
	};



#define NCOMMANDS		(sizeof(command) / sizeof(const char*))

	char *pos;
	char *last;		/* Used by strtok_r to point to current token */
	int j;
	int hour;
	int minutes;
	int group = 0;
	int last_group = 0;
	int first_day = -1;
	int process_day = -1;
	event *pTimer;
	event *pOff;
	event *pOn;

	/* Parse our time requests */
	pos = strtok_r(content, ",=\n", &last);

	/* Destroy the macro timer objects, if any */
	destroy_timer(off_timer);
	destroy_timer(on_timer);

	/* Now create our timer objects for on and off events */
	pOn = on_timer = new event;
	pOff = off_timer = new event;

	/* Establish some defaults */
	pester_message = 0;
	timer_flag = 0;
	refresh_rate = 40;
	hold_cycle = 3;
	diskcheck_number = 0;

	/* To prevent looping */
	for (int i = 0; i < 200; i++) {
		int cmd = -1;

		if (pos[0] != COMMENT_PREFIX) {
			/* Could return groups, say MON-THR, need to
			 * strip '-' out */
			if (pos[3] == '-') {
				*(last - 1) = '=';	/* Plug the '0' with token parameter  */
				group = 1;
				last -= 8;
				pos = strtok_r(NULL, "-", &last);
			}

			/* Locate our expected commands */
			for (cmd = 0; cmd < (int) NCOMMANDS; cmd++)
				if (strcasecmp(pos, command[cmd]) == 0)
					break;

			pos = strtok_r(NULL, ",=\n", &last);
		} else {
			pos = strtok_r(NULL, "\n", &last);

			/* After the first remark we have ignored, make sure
			 * we detect a valid line and move the tokeniser
			 * pointer if none remark field */
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
		case TIMER:
			if (strcasecmp(pos, "ON") == 0)
				timer_flag = 1;
			break;

			/* Shutdown? */
		case SHUTDOWN:
			pTimer = pOff;
			hour = minutes = -1;
			goto process;

			/* Macro OFF? */
		case OFF:
			pTimer = pOff;
			hour = 24;
			minutes = 0;
			goto process;

			/* Power-on? */
		case POWERON:
			pTimer = pOn;
			hour = minutes = -1;
			goto process;

			/* Macro ON? */
		case ON:
			pTimer = pOn;
			hour = minutes = 0;
		process:
			if (!sscanf(pos, "%02d:%02d", &hour, &minutes))
				timer_flag = -1;

			/* Ensure time entry is valid */
			else if ((hour >= 0 && hour <= 24)
				 && (minutes >= 0 && minutes <= 59)) {
				/* Valid macro'd OFF/ON entry? */
				if (cmd == OFF || cmd == ON) {
					/* Group macro so create the other events */
					if (group != 0) {
						j = first_day - 1;
						/* Create the multiple
						 * entries for each day in
						 * range specified */
						while (j != process_day) {
							j++;
							if (j > 7)
								j = 0;
							pTimer->day = j;
							pTimer->time = (hour * 60) + minutes;
							pTimer->next = new event;
							pTimer = pTimer->next;
						}
					} else {
						pTimer->day = process_day;
						pTimer->time = (hour * 60) + minutes;
						pTimer->next = new event;
						pTimer = pTimer->next;
					}
				}

				/* Now handle the defaults */
				else if (cmd == SHUTDOWN)
					off_time = (hour * 60) + minutes;
				else if (cmd == POWERON)
					on_time = (hour * 60) + minutes;
			} else
				timer_flag = -1;

			/* Update our pointers */
			if (cmd < POWERON) // FIXME: Avoid ordered comparisons with enum
				pOff = pTimer;
			else
				pOn = pTimer;

			break;

			/* Disk check percentage? */
		case DISKCHECK:
			if (!sscanf(pos, "%5d", &max_pct))
				max_pct = -1;
			ensure_limits(max_pct, -1, 100);
			break;

			/* Refresh/re-scan time? */
		case REFRESH:
			if (!sscanf(pos, "%03d", &refresh_rate))
				refresh_rate = 40;
			ensure_limits(refresh_rate, 10, FIVE_MINUTES);
			break;

			/* Button hold-in time? */
		case HOLD:
			if (!sscanf(pos, "%02d", &hold_cycle))
				hold_cycle = HOLD_SECONDS;
			ensure_limits(hold_cycle, 2, 10);
			break;

			/* Macro days in week? */
		case SUN:
		case MON:
		case TUE:
		case WED:
		case THR:
		case FRI:
		case SAT:
			/* For groups, */
			process_day = cmd - SUN;
			/* Remove grouping flag for next definition */
			last_group += group;
			if (last_group > 2) {
				group = last_group = 0;
			}

			if (last_group == 1)
				first_day = process_day;

			break;

		case DISKNAG:
			if (strcasecmp(pos, "ON") == 0)
				pester_message = 1;
			break;
			/* Fan failure stop time before event trigger */
		case FANSTOP:
			if (strcasecmp(pos, "OFF") == 0)
				fan_fault_seize = 0;
			else {
				if (!sscanf(pos, "%02d", &fan_fault_seize))
					fan_fault_seize = FAN_SEIZE_TIME;
				ensure_limits(fan_fault_seize, 1, 60);
			}
			break;

		/* Specified partition names */
		case ROOT: /* root device */
		case WORK: /* work device */
			if (strlen(pos) <= 5) { // FIXME: make string length more flexible
				diskcheck_number++;
				char *tgt_device = (cmd == ROOT) ? root_device : work_device;
				sprintf(tgt_device, "/dev/%s", pos);
			}
			break;
		}
	}

	if (timer_flag < 0) {
		timer_flag = 0;
		report_error(3);
	}
}


/**
 * Destroys time objects
 *
 * @param e A pointer to the event structure containing the head of the
 * linked list of events to be deleted.  Note that the entire linked list
 * is destroyed, not just its first element.
 */
static void destroy_timer(event *e)
{
	event *aux;

	while (e) {
		aux = e->next;
		delete e;
		e = aux;
	}
}


/**
 * Scan macro objects for a valid event from @a time today
 */
static int find_next_today(long timeNow, event *cur, long *time)
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
static int find_next_day(event *cur, long *time, long *offset)
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
static void get_time(long time_now, event *pTimerLocate, long *time, long defaultTime)
{
	/* Ensure that macro timer object is valid */
	if (pTimerLocate && pTimerLocate->next != NULL) {
		long offset = 0;
		event *pTimer = pTimerLocate;
		/* Next event for today */
		char located = find_next_today(time_now, pTimer, time);

		/* Failed to find a time for today, look for the next
		 * power-up time */
		if (!located) {
			pTimer = pTimerLocate;
			located = find_next_day(pTimer, time, &offset);
		}

		/* Nothing for week-end, look at start */
		if (!located) {
			*time = pTimerLocate->time;
			offset = ((6 - last_day) + pTimerLocate->day) * TWENTYFOURHR;
		}

		*time += offset;

		if (offset > TWENTYFOURHR && defaultTime > 0)
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
	time_t ltime, ttime;
	struct tm *decode_time;
	char message[80];
	long mask = 0x800;
	long offTime, onTime;

	/* Timer enabled? */
	if (timer_flag) {
		/* Get time of day */
		time(&ltime);

		decode_time = localtime(&ltime);
		long current_time = (decode_time->tm_hour * 60) + decode_time->tm_min;
		last_day = decode_time->tm_wday;

		get_time(current_time, off_timer, &offTime, off_time);
		/* Correct search if switch-off is tommorrow */
		if (offTime > TWENTYFOURHR)
			get_time(current_time, on_timer, &onTime, on_time);
		else
			get_time(offTime, on_timer, &onTime, on_time);

		/* Protect for tomorrows setting */
		shutdown_timer = (offTime < current_time) ?
			(TWELVEHR + (offTime - (current_time - TWELVEHR))) * 60 :
			(offTime - current_time) * 60;

		/* Remeber the current seconds passed the minute */
		shutdown_timer -= decode_time->tm_sec;

		ttime = ltime + shutdown_timer;
		decode_time = localtime(&ttime);

		sprintf(message, "Timer is set with %02d/%02d %02d:%02d",
			decode_time->tm_mon + 1, decode_time->tm_mday,
			decode_time->tm_hour, decode_time->tm_min);

		/* Now setup the AVR with the power-on time */

		long wait_time;

		/* Correct to AVR oscillator */
		if (onTime < current_time) {
			wait_time = (TWELVEHR + (onTime - (current_time - TWELVEHR))) * 60;
			onTime = ((TWELVEHR + (onTime - (current_time - TWELVEHR))) * 100) / 112;
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
		    && (onTime - (shutdown_timer / 60)) > TIMER_RESOLUTION) {
			wait_time -= ((onTime - TIMER_RESOLUTION) * 672) / 10;
			report_error(2);
			/* Reset to timer resolution */
			onTime = TIMER_RESOLUTION;
		}

		ttime = ltime + wait_time;
		decode_time = localtime(&ttime);

		const static char *msg_kind[] = { "file update", "re-validation", "clock skew" };

		syslog(LOG_INFO, "%s-%02d/%02d %02d:%02d (Following timer %s)",
		       message, decode_time->tm_mon + 1, decode_time->tm_mday,
		       decode_time->tm_hour, decode_time->tm_min, msg_kind[type]);

		/* Now tell the AVR we are updating the 'on' time */
		write_to_uart(0x3E);	/* '>' */
		write_to_uart(0x3C);	/* '<' */
		write_to_uart(0x3A);	/* ':' */
		write_to_uart(0x38);	/* '8' */

		/* Bit pattern (12-bits) detailing time to wake */
		for (int i = 0; i < 12; i++) {
			char avr_cmd = (onTime & mask ? 0x21 : 0x20) + ((11 - i) * 2);
			mask >>= 1;

			/* Output to AVR */
			write_to_uart(avr_cmd);
		}

		/* Complete output and set LED state (power) to pulse */
		write_to_uart(0x3F);	/* '?' */
		keep_alive = 0x5B;	/* '[' */
	} else {		/* Inform AVR its not in timer mode */
		write_to_uart(0x3E);	/* '>' */
		keep_alive = 0x5A;	/* 'Z' */
	}

	write_to_uart(keep_alive);
}


/**
 * Check to see if the configuration file has changed since the last time we checked.
 *
 * @param type The value to be passed to avr_set_timer: with 0 when the
 * config file has to be read, 1 when the status has to be re-validated, and
 * 2 when there was a large clock drift.
 *
 */
static void check_timer(int type)
{
	char buff[4096];
	struct stat filestatus;

	/* Time from avr-evtd configuration file */
	if (command_line_update == 1) {
		/* File is missing so default to off and do not do this again */
		command_line_update = 2;

		if (stat(CONFIG_FILE_LOCATION, &filestatus) == 0) {
			if (filestatus.st_mtime != last_config_mtime) {
				int file = open(CONFIG_FILE_LOCATION, O_RDONLY);

				if (file) {
					if (read(file, buff, sizeof(buff) - 1) > 0) {
						command_line_update = 1;
						parse_config(buff);
						set_avr_timer(type);
					}
					close(file);
				}
			} else
				command_line_update = 1;

			last_config_mtime = filestatus.st_mtime;
		} else {
			/* The file could not be stat'ed */
		}
	}

	/* Ensure that if we have any configuration errors we at least set timer off */
	if (command_line_update == 2) {
		command_line_update = 3;
		set_avr_timer(type);
		report_error(1);
	}

}


int main(int argc, char *argv[])
{
	int probe = 0;		/* mode in which we open the serial port */
	int debug = 0;		/* determine if we are in debug mode or not */

	if (argc == 1) {
		usage();
	}

	--argc;
	++argv;

	/* Parse any options */
	while (argc >= 1 && '-' == (*argv)[0]) {
		switch ((*argv)[1]) {
		case 'd':
			--argc;
			++argv;
			if (argc >= 1) {
				sprintf(avr_device, "%s", *argv);
			} else {
				printf("Option -d requires an argument.\n\n");
				usage();
			}
			break;
		case 'i':
			probe = 1;
			break;
		case 'c':
			debug = 1;
			break;
		case 'v':
			printf(VERSION);
			exit(0);
		case 'e':
			in_em_mode = 1;
			break;
		case 'h':
			usage();
		default:
			printf("Option unknown: %s.\n\n", *argv);
			usage();
		}
		--argc;
		++argv;
	}

	if (!debug) {
		if (daemon(0, 0) != 0)	/* fork to background */
			exit(-1);
	} else
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
	if (open_serial(avr_device, probe))
		return -3;

	if (probe) {
		close(serialfd);
		return 0;
	}

	/* make child session leader */
	setsid();

	/* clear file creation mask */
	umask(0);

	/* Open logger for this daemon */
	openlog("avr-daemon", LOG_PID | LOG_NOWAIT | LOG_CONS, LOG_WARNING);
	syslog(LOG_INFO, "%s", VERSION);

	avr_evtd_main();

	return 0;
}
