/****************************************************************************
 *
 *   Copyright (c) 2012, 2013 PX4 Development Team. All rights reserved.
 *   Author: Simon Wilks <sjwilks@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file hott_telemetry_main.c
 * @author Simon Wilks <sjwilks@gmail.com>
 *
 * Graupner HoTT Telemetry implementation.
 *
 * The HoTT receiver polls each device at a regular interval at which point
 * a data packet can be returned if necessary.
 *
 */

#include <fcntl.h>
#include <nuttx/config.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>

#include "../comms.h"
#include "../messages.h"

static int thread_should_exit = false;		/**< Deamon exit flag */
static int thread_running = false;		/**< Deamon status flag */
static int deamon_task;				/**< Handle of deamon task / thread */
static const char daemon_name[] = "hott_telemetry";
static const char commandline_usage[] = "usage: hott_telemetry start|status|stop [-d <device>]";

/**
 * Deamon management function.
 */
__EXPORT int hott_telemetry_main(int argc, char *argv[]);

/**
 * Mainloop of daemon.
 */
int hott_telemetry_thread_main(int argc, char *argv[]);

static int recv_req_id(int uart, uint8_t *id);
static int send_data(int uart, uint8_t *buffer, size_t size);

int
recv_req_id(int uart, uint8_t *id)
{
	static const int timeout_ms = 1000;  // TODO make it a define
	struct pollfd fds[] = { { .fd = uart, .events = POLLIN } };

	uint8_t mode;

	if (poll(fds, 1, timeout_ms) > 0) {
		/* Get the mode: binary or text  */
		read(uart, &mode, sizeof(mode));

		/* if we have a binary mode request */
		if (mode != BINARY_MODE_REQUEST_ID) {
			return ERROR;
		}

		/* Read the device ID being polled */
		read(uart, id, sizeof(*id));
	} else {
		warnx("UART timeout on TX/RX port");
		return ERROR;
	}

	return OK;
}

int
recv_data(int uart, uint8_t *buffer, size_t *size, uint8_t *id)
{
	usleep(5000);

	static const int timeout_ms = 1000;
	struct pollfd fds[] = { { .fd = uart, .events = POLLIN } };

	
	// XXX should this poll be inside the while loop???
	if (poll(fds, 1, timeout_ms) > 0) {
		int i = 0;
		bool stop_byte_read = false;
		while (true)  {
			read(uart, &buffer[i], sizeof(buffer[i]));
			//printf("[%d]: %d\n", i, buffer[i]);

			if (stop_byte_read) {
				// process checksum
				*size = ++i;
				return OK;
			}
			// XXX can some other field not have the STOP BYTE value?
			if (buffer[i] == STOP_BYTE) {
				*id = buffer[1];
				stop_byte_read = true;
			}
			i++;
		}
	}
	return ERROR;
}

int
send_data(int uart, uint8_t *buffer, size_t size)
{
	usleep(POST_READ_DELAY_IN_USECS);

	uint16_t checksum = 0;
	for (size_t i = 0; i < size; i++) {
		if (i == size - 1) {
			/* Set the checksum: the first uint8_t is taken as the checksum. */
			buffer[i] = checksum & 0xff;

		} else {
			checksum += buffer[i];
		}

		write(uart, &buffer[i], sizeof(buffer[i]));

		/* Sleep before sending the next byte. */
		usleep(POST_WRITE_DELAY_IN_USECS);
	}

	/* A hack the reads out what was written so the next read from the receiver doesn't get it. */
	/* TODO: Fix this!! */
	uint8_t dummy[size];
	read(uart, &dummy, size);

	return OK;
}

int
hott_telemetry_thread_main(int argc, char *argv[])
{
	warnx("starting");

	thread_running = true;

	const char *device = "/dev/ttyS1";		/**< Default telemetry port: USART2 */

	/* read commandline arguments */
	for (int i = 0; i < argc && argv[i]; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) { //device set
			if (argc > i + 1) {
				device = argv[i + 1];

			} else {
				thread_running = false;
				errx(1, "missing parameter to -d\n%s", commandline_usage);
			}
		}
	}

	/* enable UART, writes potentially an empty buffer, but multiplexing is disabled */
	const int uart = open_uart(device);
	if (uart < 0) {
		errx(1, "Failed opening HoTT UART, exiting.");
		thread_running = false;
	}

	sub_messages_init();

	uint8_t buffer[MESSAGE_BUFFER_SIZE];
	size_t size = 0;
	uint8_t id = 0;
	bool connected = true;
	while (!thread_should_exit) {
		// Listen for and serve poll from the receiver.
		if (recv_req_id(uart, &id) == OK) {
			if (!connected) {
				connected = true;
				warnx("OK");
			}

			switch (id) {
			case EAM_SENSOR_ID:
				build_eam_response(buffer, &size);
				break;

			case GPS_SENSOR_ID:
				build_gps_response(buffer, &size);
				break;

			default:
				continue;	// Not a module we support.
			}

			send_data(uart, buffer, size);
		} else {
			connected = false;
			warnx("syncing");
		}
	}

	warnx("exiting");

	close(uart);

	thread_running = false;

	return 0;
}

/**
 * Process command line arguments and start the daemon.
 */
int
hott_telemetry_main(int argc, char *argv[])
{
	if (argc < 1) {
		errx(1, "missing command\n%s", commandline_usage);
	}

	if (!strcmp(argv[1], "start")) {

		if (thread_running) {
			warnx("deamon already running");
			exit(0);
		}

		thread_should_exit = false;
		deamon_task = task_spawn_cmd(daemon_name,
					 SCHED_DEFAULT,
					 SCHED_PRIORITY_MAX - 40,
					 2048,
					 hott_telemetry_thread_main,
					 (argv) ? (const char **)&argv[2] : (const char **)NULL);
		exit(0);
	}

	if (!strcmp(argv[1], "stop")) {
		thread_should_exit = true;
		exit(0);
	}

	if (!strcmp(argv[1], "status")) {
		if (thread_running) {
			warnx("daemon is running");

		} else {
			warnx("daemon not started");
		}

		exit(0);
	}

	errx(1, "unrecognized command\n%s", commandline_usage);
}
