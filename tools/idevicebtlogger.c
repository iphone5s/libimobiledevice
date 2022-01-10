/*
 * idevicebt_packet_logger.c
 * Capture bt HCI packet log to pcap
 *
 * Copyright (c) 2021 Geoffrey Kruse, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define TOOL_NAME "idevicebtlogger"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <fcntl.h>

#ifdef WIN32
#include <windows.h>
#define sleep(x) Sleep(x*1000)
#else
#include <arpa/inet.h>
#endif


#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/bt_packet_logger.h>
#include <pcap.h>

#define DLT_BLUETOOTH_HCI_H4_WITH_PHDR 201
#define LIBPCAP_BT_PHDR_SENT    0x00000000
#define LIBPCAP_BT_PHDR_RECV    htonl(0x00000001)

static int quit_flag = 0;
static int exit_on_disconnect = 0;

static char* udid = NULL;
static idevice_t device = NULL;
static bt_packet_logger_client_t bt_packet_logger = NULL;
static int use_network = 0;
static char* out_filename = NULL;
static char* log_format_string = NULL;
static pcap_dumper_t * pcap_dumper = NULL;
static int packetlogger_fd = -1;

static enum {
	LOG_FORMAT_PACKETLOGGER,
	LOG_FORMAT_PCAP
} log_format = LOG_FORMAT_PACKETLOGGER;

typedef enum {
	HCI_COMMAND = 0x00,
	HCI_EVENT = 0x01,
	SENT_ACL_DATA = 0x02,
	RECV_ACL_DATA = 0x03
} PacketLoggerPacketType;

/**
 * Callback from the packet logger service to handle packets and log to PacketLoggger format
 */
static void bt_packet_logger_callback_packetlogger(uint8_t * data, uint16_t len, void *user_data)
{
	write(packetlogger_fd, data, len);
}

/**
 * Callback from the packet logger service to handle packets and log to pcap
 */
static void bt_packet_logger_callback_pcap(uint8_t * data, uint16_t len, void *user_data)
{
	bt_packet_logger_header_t * header = (bt_packet_logger_header_t *)data;
	uint16_t offset = sizeof(bt_packet_logger_header_t);

	// size + sizeof(uint32_t) to account for the direction pseudo header
	struct pcap_pkthdr pcap_header;
	pcap_header.caplen = ntohl(header->length) + sizeof(uint32_t);
	pcap_header.len = len - sizeof(bt_packet_logger_header_t) + sizeof(uint32_t);
	pcap_header.ts.tv_sec = ntohl(header->ts_secs);
	pcap_header.ts.tv_usec = ntohl(header->ts_usecs);

	// Sanity check incoming data and drop packet if its unreasonable.
	if(pcap_header.len > BT_MAX_PACKET_SIZE || pcap_header.caplen > BT_MAX_PACKET_SIZE) {
		fprintf(stderr, "WARNING: Packet length exceeded max size, corruption likely.\n ");
		return;
	}

	uint8_t packet_type = data[offset];
	uint8_t hci_h4_type = 0xff;
	uint32_t direction;

	switch(packet_type) {
		case HCI_EVENT:
			hci_h4_type = 0x04;
			direction = LIBPCAP_BT_PHDR_RECV;
			break;

		case HCI_COMMAND:
			hci_h4_type = 0x01;
			direction = LIBPCAP_BT_PHDR_SENT;
			break;

		case SENT_ACL_DATA:
			hci_h4_type = 0x02;
			direction = LIBPCAP_BT_PHDR_SENT;
			break;

		case RECV_ACL_DATA:
			hci_h4_type = 0x02;
			direction = LIBPCAP_BT_PHDR_RECV;
			break;

		default:
			// unknown packet logger type, just pass it on
			hci_h4_type = packet_type;
			direction = LIBPCAP_BT_PHDR_RECV;
			break;
	}
	if(hci_h4_type != 0xff) {
		data[offset] = hci_h4_type;
		// we know we are sizeof(bt_packet_logger_header_t) into the buffer passed in to
		// this function.  We need to add the uint32_t pseudo header to the front of the packet
		// so adjust the offset back by sizeof(uint32_t) and write it to the buffer.  This avoids
		// having to memcpy things around.
		offset -= sizeof(uint32_t);
		*(uint32_t*)&data[offset] = direction;
		pcap_dump((unsigned char*)pcap_dumper, &pcap_header, &data[offset]);
		pcap_dump_flush(pcap_dumper);
	}
}

/**
 * Disable HCI log capture
 */
static void stop_logging(void)
{
	fflush(NULL);

	if (bt_packet_logger) {
		bt_packet_logger_client_free(bt_packet_logger);
		bt_packet_logger = NULL;
	}

	if (device) {
		idevice_free(device);
		device = NULL;
	}
}

/**
 * Enable HCI log capture
 */
static int start_logging(void)
{
	idevice_error_t ret = idevice_new_with_options(&device, udid, (use_network) ? IDEVICE_LOOKUP_NETWORK : IDEVICE_LOOKUP_USBMUX);
	if (ret != IDEVICE_E_SUCCESS) {
		fprintf(stderr, "Device with udid %s not found!?\n", udid);
		return -1;
	}

	lockdownd_client_t lockdown = NULL;
	lockdownd_error_t lerr = lockdownd_client_new_with_handshake(device, &lockdown, TOOL_NAME);
	if (lerr != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "ERROR: Could not connect to lockdownd: %d\n", lerr);
		idevice_free(device);
		device = NULL;
		return -1;
	}

	/* start bt_packet_logger service */
	bt_packet_logger_client_start_service(device, &bt_packet_logger, TOOL_NAME);

	/* start capturing bt_packet_logger */
	void (*callback)(uint8_t * data, uint16_t len, void *user_data);
	switch (log_format){
		case LOG_FORMAT_PCAP:
			callback = bt_packet_logger_callback_pcap;
			break;
		case LOG_FORMAT_PACKETLOGGER:
			callback = bt_packet_logger_callback_packetlogger;
			break;
		default:
			assert(0);
			return 0;
	}
	bt_packet_logger_error_t serr = bt_packet_logger_start_capture(bt_packet_logger, callback, NULL);
	if (serr != BT_PACKET_LOGGER_E_SUCCESS) {
		fprintf(stderr, "ERROR: Unable to start capturing bt_packet_logger.\n");
		bt_packet_logger_client_free(bt_packet_logger);
		bt_packet_logger = NULL;
		idevice_free(device);
		device = NULL;
		return -1;
	}

	fprintf(stdout, "[connected:%s]\n", udid);
	fflush(stdout);

	return 0;
}

/**
 * Callback for device events
 */
static void device_event_cb(const idevice_event_t* event, void* userdata)
{
	if (use_network && event->conn_type != CONNECTION_NETWORK) {
		return;
	} else if (!use_network && event->conn_type != CONNECTION_USBMUXD) {
		return;
	}
	if (event->event == IDEVICE_DEVICE_ADD) {
		if (!bt_packet_logger) {
			if (!udid) {
				udid = strdup(event->udid);
			}
			if (strcmp(udid, event->udid) == 0) {
				if (start_logging() != 0) {
					fprintf(stderr, "Could not start logger for udid %s\n", udid);
				}
			}
		}
	} else if (event->event == IDEVICE_DEVICE_REMOVE) {
		if (bt_packet_logger && (strcmp(udid, event->udid) == 0)) {
			stop_logging();
			fprintf(stdout, "[disconnected:%s]\n", udid);
			if (exit_on_disconnect) {
				quit_flag++;
			}
		}
	}
}

/**
 * signal handler function for cleaning up properly
 */
static void clean_exit(int sig)
{
	fprintf(stderr, "\nExiting...\n");
	quit_flag++;
}

/**
 * print usage information
 */
static void print_usage(int argc, char **argv, int is_error)
{
	char *name = NULL;
	name = strrchr(argv[0], '/');
	fprintf(is_error ? stderr : stdout, "Usage: %s [OPTIONS] <FILE>\n", (name ? name + 1: argv[0]));
	fprintf(is_error ? stderr : stdout,
		"\n" \
		"Capture HCI packets from a connected device.\n" \
		"\n" \
		"OPTIONS:\n" \
		"  -u, --udid UDID     target specific device by UDID\n" \
		"  -n, --network       connect to network device\n" \
		"  -f, --format FORMAT logging format: packetlogger (default) or pcap\n" \
		"  -x, --exit          exit when device disconnects\n" \
		"  -h, --help          prints usage information\n" \
		"  -d, --debug         enable communication debugging\n" \
		"  -v, --version       prints version information\n" \
		"\n" \
		"Homepage:    <" PACKAGE_URL ">\n"
		"Bug Reports: <" PACKAGE_BUGREPORT ">\n"
	);
}

/**
 * Program entry
 */
int main(int argc, char *argv[])
{
	int c = 0;
	const struct option longopts[] = {
		{ "debug", no_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ "udid", required_argument, NULL, 'u' },
		{ "format", required_argument, NULL, 'f' },
		{ "network", no_argument, NULL, 'n' },
		{ "exit", no_argument, NULL, 'x' },
		{ "version", no_argument, NULL, 'v' },
		{ NULL, 0, NULL, 0}
	};

	signal(SIGINT, clean_exit);
	signal(SIGTERM, clean_exit);
#ifndef WIN32
	signal(SIGQUIT, clean_exit);
	signal(SIGPIPE, SIG_IGN);
#endif

	while ((c = getopt_long(argc, argv, "dhu:f:nxv", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			idevice_set_debug_level(1);
			break;
		case 'u':
			if (!*optarg) {
				fprintf(stderr, "ERROR: UDID must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			}
			free(udid);
			udid = strdup(optarg);
			break;
		case 'f':
			if (!*optarg) {
				fprintf(stderr, "ERROR: FORMAT must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			}
			free(log_format_string);
			log_format_string = strdup(optarg);
			break;
		case 'n':
			use_network = 1;
			break;
		case 'x':
			exit_on_disconnect = 1;
			break;
		case 'h':
			print_usage(argc, argv, 0);
			return 0;
		case 'v':
			printf("%s %s\n", TOOL_NAME, PACKAGE_VERSION);
			return 0;
		default:
			print_usage(argc, argv, 1);
			return 2;
		}
	}

	if (optind < argc) {
		out_filename = argv[optind];
		printf("Output File: %s\n", out_filename);
	}
	else {
		print_usage(argc, argv, 1);
		return 2;
	}

	if (log_format_string != NULL){
		if (strcmp("packetlogger", log_format_string) == 0){
			log_format = LOG_FORMAT_PACKETLOGGER;
		} else if (strcmp("pcap", log_format_string) == 0){
			log_format = LOG_FORMAT_PCAP;
		} else {
			printf("Unknown logging format: '%s'\n", log_format_string);
			print_usage(argc, argv, 1);
			return 2;
		}
	}

	int num = 0;
	idevice_info_t *devices = NULL;
	idevice_get_device_list_extended(&devices, &num);
	idevice_device_list_extended_free(devices);
	int oflags;
	if (num == 0) {
		if (!udid) {
			fprintf(stderr, "No device found. Plug in a device or pass UDID with -u to wait for device to be available.\n");
			return -1;
		} else {
			fprintf(stderr, "Waiting for device with UDID %s to become available...\n", udid);
		}
	}

	switch (log_format){
		case LOG_FORMAT_PCAP:
			printf("Output Format: PCAP\n");
			pcap_dumper = pcap_dump_open(pcap_open_dead(DLT_BLUETOOTH_HCI_H4_WITH_PHDR, BT_MAX_PACKET_SIZE), out_filename);
			break;
		case LOG_FORMAT_PACKETLOGGER:
			printf("Output Format: PacketLogger\n");
		    oflags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef WIN32
		    default_oflags |= O_BINARY;
#endif
		    packetlogger_fd = open(out_filename, oflags);
		    if (packetlogger_fd < 0){
		        fprintf(stderr, "Failed to open file %s, errno = %d\n", out_filename, errno);
		        return -2;
		    }		
	    	break;
		default:
			assert(0);
			return -2;
	}
	idevice_event_subscribe(device_event_cb, NULL);

	while (!quit_flag) {
		sleep(1);
	}

	idevice_event_unsubscribe();
	stop_logging();

	if (pcap_dumper) {
		pcap_dump_close(pcap_dumper);
	}
	if (packetlogger_fd >= 0){
		close(packetlogger_fd);
	}

	free(udid);

	return 0;
}