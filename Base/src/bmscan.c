/*
 *
 *  bmscan - scan for active Broodminder devices
 *
 *	This code was stolen from bluez and hcitool, and radically simplified
 *	in the hopes that removing libtool and all the extra cruft will make it 
 *	easier to port and to understand.
 *
 *	Bill Cheswick, February 2017
 *
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <signal.h>
#include <math.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/hci.h"
#include "bluetooth/hci_lib.h"

#include "broodminder.h"
#include "dump.h"
#include "arg.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Unofficial value, might still change */
#define LE_LINK		0x80

#define FLAGS_AD_TYPE 0x01
#define FLAGS_LIMITED_MODE_BIT 0x01
#define FLAGS_GENERAL_MODE_BIT 0x02

#define EIR_FLAGS                   0x01  /* flags */
#define EIR_UUID16_SOME             0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL              0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME             0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL              0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME            0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL             0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT              0x08  /* shortened local name */
#define EIR_NAME_COMPLETE           0x09  /* complete local name */
#define EIR_TX_POWER                0x0A  /* transmit power level */
#define EIR_DEVICE_ID               0x10  /* device ID */

static volatile int signal_received = 0;

int reports_left = 0;	// report limit, or zero if not tested
int timeout = 60;	// number of seconds we will await input. zero if indefinite
int timed_out = 0;
int hci_fd = -1;
struct hci_filter of;

void
close_hci(int fd) {
	setsockopt(fd, SOL_HCI, HCI_FILTER, &of, sizeof(of));
}

static char *type2str(uint8_t type)
{
	switch (type) {
	case SCO_LINK:
		return "SCO";
	case ACL_LINK:
		return "ACL";
	case ESCO_LINK:
		return "eSCO";
	case LE_LINK:
		return "LE";
	default:
		return "Unknown";
	}
}

static void sigint_handler(int sig)
{
	signal_received = sig;
}

static void do_timeout(int sig) {
	close_hci(hci_fd);
	exit(0);
}

static void eir_parse_name(uint8_t *eir, size_t eir_len,
						char *buf, size_t buf_len)
{
	size_t offset;

	offset = 0;
	while (offset < eir_len) {
		uint8_t field_len = eir[0];
		size_t name_len;

		/* Check for the end of EIR */
		if (field_len == 0)
			break;

		if (offset + field_len > eir_len)
			goto failed;

		switch (eir[1]) {
		case EIR_NAME_SHORT:
		case EIR_NAME_COMPLETE:
			name_len = field_len - 1;
			if (name_len > buf_len)
				goto failed;

			memcpy(buf, &eir[2], name_len);
			return;
		}

		offset += field_len + 1;
		eir += field_len + 1;
	}

failed:
	snprintf(buf, buf_len, "(unknown)");
}

static int read_flags(uint8_t *flags, const uint8_t *data, size_t size)
{
	size_t offset;

	if (!flags || !data)
		return -EINVAL;

	offset = 0;
	while (offset < size) {
		uint8_t len = data[offset];
		uint8_t type;

		/* Check if it is the end of the significant part */
		if (len == 0)
			break;

		if (len + offset > size)
			break;

		type = data[offset + 1];

		if (type == FLAGS_AD_TYPE) {
			*flags = data[offset + 2];
			return 0;
		}

		offset += 1 + len;
	}

	return -ENOENT;
}

static int check_report_filter(uint8_t procedure, le_advertising_info *info)
{
	uint8_t flags;

	/* If no discovery procedure is set, all reports are treat as valid */
	if (procedure == 0)
		return 1;

	/* Read flags AD type value from the advertising report if it exists */
	if (read_flags(&flags, info->data, info->length))
		return 0;

	switch (procedure) {
	case 'l': /* Limited Discovery Procedure */
		if (flags & FLAGS_LIMITED_MODE_BIT)
			return 1;
		break;
	case 'g': /* General Discovery Procedure */
		if (flags & (FLAGS_LIMITED_MODE_BIT | FLAGS_GENERAL_MODE_BIT))
			return 1;
		break;
	default:
		fprintf(stderr, "Unknown discovery procedure\n");
	}

	return 0;
}

// XXX These are mystery constants: I cannot find their names

static const char *evttype2str(uint8_t type)
{
	switch (type) {
	case 0x00:
		return "ADV_IND - Connectable undirected advertising";
	case 0x01:
		return "ADV_DIRECT_IND - Connectable directed advertising";
	case 0x02:
		return "ADV_SCAN_IND - Scannable undirected advertising";
	case 0x03:
		return "ADV_NONCONN_IND - Non connectable undirected advertising";
	case 0x04:
		return "SCAN_RSP - Scan Response";
	default:
		return "Reserved";
	}
}

void
dump_bm_adv(bm_adv_resp *bm_adv) {
	double c;
	int f;
	u_short tr;

	if (bm_adv->bm_model == BM_SCALE)
		printf("scale   ");
	else if (bm_adv->bm_model == BM_SENSOR)
		printf("sensor  ");
	else
		printf("0x%.02    ", bm_adv->bm_model & 0xff);

	printf("v%d.%d", bm_adv->bm_devmajor, bm_adv->bm_devminor);
	switch (bm_adv->bm_devmajor) { 
	case 1:
		printf("  %3d%%  samples %d  temp %d",
			bm_adv->v1.bm_battery, 
			USHORT(bm_adv->v1.bm_samples),
			USHORT(bm_adv->v1.bm_temp));
		break;
	case 2:
		printf("  %3d%% %3dF %2d%%",
			bm_adv->v2.bm_battery, 
			bm_f(USHORT(bm_adv->v2.bm_temp)),
			bm_adv->v2.bm_humidity);
		printf("   #%d", 
			USHORT(bm_adv->v2.bm_samples));
		if (bm_adv->bm_model == BM_SCALE)
			printf("  wts: %6.2f %6.2f",
				bm_w((uint8_t *)&bm_adv->v2.bm_weight_left),
				bm_w((uint8_t *)&bm_adv->v2.bm_weight_right));
		break;
	default:
		printf("	unknown Broodminder version %d", bm_adv->bm_devmajor);
	}
	printf("\n");

	if (reports_left > 0) {
		reports_left--;
		if (reports_left == 0)
			exit(0);
	}

//do_dump_hex((uint8_t *)bm_adv, 18);
//	printf("\n");
}

static int print_advertising_devices(int dd, uint8_t filter_type)
{
	unsigned char buf[HCI_MAX_EVENT_SIZE], *ptr;
	struct hci_filter nf;
	struct sigaction sa;
	socklen_t olen;
	int len;

	olen = sizeof(of);
	if (getsockopt(dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
		printf("Could not get socket options\n");
		return -1;
	}

	hci_filter_clear(&nf);
	hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
	hci_filter_set_event(EVT_LE_META_EVENT, &nf);

	if (setsockopt(dd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
		printf("Could not set socket options\n");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);
	char name[30];

	setlinebuf(stdout);

	while (1) {
		hci_event_hdr *hci_e_hdr;
		evt_le_meta_event *meta;
		le_advertising_info *ad_info;
		bm_adv_resp *bm_adv;
		char addr[18];

		while ((len = read(dd, buf, sizeof(buf))) < 0) {
			if (timed_out || errno == EINTR && signal_received == SIGINT) {
				len = 0;
				goto done;
			}

			if (errno == EAGAIN || errno == EINTR)
				continue;
			goto done;
		}


		if (buf[0] != HCI_EVENT_PKT)
			continue;

		hci_e_hdr = (hci_event_hdr *)&buf[1];
		if (hci_e_hdr->evt != EVT_LE_META_EVENT)
			continue;

		meta = (evt_le_meta_event *) hci_e_hdr + sizeof(hci_event_hdr);
		if (meta->subevent != EVT_LE_ADVERTISING_REPORT) {
			printf("******not adv. report: %d\n", meta->subevent);
			continue;
		}

		ad_info = (le_advertising_info *)(meta->data + 1);
		if (!IS_BROODMINDER((uint8_t *)ad_info->data))
			continue;

		memset(name, 0, sizeof(name));
		ba2str(&ad_info->bdaddr, addr);
		eir_parse_name(ad_info->data, ad_info->length,
						name, sizeof(name) - 1);
		bm_adv = (bm_adv_resp *)(ad_info->data + 10);


		// These constants are a complete mystery to me

		switch (ad_info->evt_type) {
		case 3:			// non-connectable undirected advertising
			continue;	// not us

		case 0:			// connectable undirected advertising
//			printf("%s  %-10s  ", addr, name);
			printf("%s  ", addr);
			dump_bm_adv(bm_adv);
			break;

		case 4:		// SCAN_RSP
			//  09 09 34 32 3a 30 41 3a 39 46	42:0A:9F
			continue;
		default:
			printf("** %s  %-10s  ", addr, name);
			printf("(%d) %s  ", ad_info->evt_type, evttype2str(ad_info->evt_type));
			printf("len %3d:  ", ad_info->length);
			do_dump_hex((uint8_t *)ad_info, ad_info->length);
		}
	}

done:
	close_hci(dd);

	if (len < 0)
		return -1;

	return 0;
}

static int
usage(void) {
	fprintf(stderr, "usage: bmscan [-i hci-devid]\n");
	return 1;
}

int
main(int argc, char *argv[]) {
	int dev_id = -1;
	bdaddr_t ba;

	int err, opt, dd;
	uint8_t own_type = LE_PUBLIC_ADDRESS;
	uint8_t scan_type = 0x01;
	uint8_t filter_type = 0;
	uint8_t filter_policy = 0x00;
	uint16_t interval = htobs(0x0010);
	uint16_t window = htobs(0x0010);
	uint8_t filter_dup = 0x01;

	int timeout = 0;

	ARGBEGIN {	// XXX these ARGF-s are not tested: dangerous
	case 'i':
		dev_id = hci_devid(ARGF());
		if (dev_id < 0) {
			perror("Invalid device");
			exit(1);
		}
		break;
	case 'm':
		reports_left = atoi(ARGF());
		break;
	case 't':
		timeout = atoi(ARGF());
		break;
	default:
		return usage();
	} ARGEND;

	if (dev_id != -1 && hci_devba(dev_id, &ba) < 0) {
		perror("Device is not available");
		exit(2);
	}

	if (dev_id < 0)
		dev_id = hci_get_route(NULL);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		perror("Could not open device");
		exit(1);
	}

	err = hci_le_set_scan_parameters(dd, scan_type, interval, window,
						own_type, filter_policy, 10000);
	if (err < 0) {
		perror("Set scan parameters failed");
		exit(1);
	}

	if (timeout) {
		signal(SIGALRM, do_timeout);
		alarm(timeout);
	}

	err = hci_le_set_scan_enable(dd, 0x01, filter_dup, 10000);
	if (err < 0) {
		perror("Enable scan failed");
		exit(1);
	}

	err = print_advertising_devices(dd, filter_type);
	if (err < 0) {
		perror("Could not receive advertising events");
		exit(1);
	}

	hci_fd = dd;

	err = hci_le_set_scan_enable(dd, 0x00, filter_dup, 10000);
	if (err < 0) {
		perror("Disable scan failed");
		exit(1);
	}

	hci_close_dev(dd);

	return 0;
}
