/*
 *  Copyright (C) 2011 - 2024  Arnaud Quette <arnaud.quette@free.fr>
 *  Copyright (C) 2016 Michal Vyskocil <MichalVyskocil@eaton.com>
 *  Copyright (C) 2016 - 2021 Jim Klimov <EvgenyKlimov@eaton.com>
 *  Copyright (C) 2022 - 2024 Jim Klimov <jimklimov+nut@gmail.com>
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*! \file nut-scanner.c
    \brief A tool to detect NUT supported devices
    \author Arnaud Quette <arnaud.quette@free.fr>
    \author Michal Vyskocil <MichalVyskocil@eaton.com>
    \author Jim Klimov <EvgenyKlimov@eaton.com>
    \author Jim Klimov <jimklimov+nut@gmail.com>
*/

#include "common.h"	/* Must be first include to pull "config.h" */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "nut_version.h"
#include <unistd.h>
#include <string.h>

/* Headers related to getifaddrs() for `-m auto` on different platforms */
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#include <sys/types.h>
#ifndef WIN32
# include <arpa/inet.h>
# include <netinet/in.h>
# include <ifaddrs.h>
# include <netdb.h>
# include <sys/ioctl.h>
# include <net/if.h>
#else
/* Those 2 files for support of getaddrinfo, getnameinfo and freeaddrinfo
   on Windows 2000 and older versions */
/* // TODO: complete "-m auto" support
# include <ws2tcpip.h>
# include <wspiapi.h>
# ifndef AI_NUMERICSERV
#  define AI_NUMERICSERV NI_NUMERICSERV
# endif
# include "wincompat.h"
*/
#endif

#ifdef HAVE_PTHREAD
# include <pthread.h>
# ifdef HAVE_SEMAPHORE
#  include <semaphore.h>
# endif
# if (defined HAVE_PTHREAD_TRYJOIN) || (defined HAVE_SEMAPHORE)
#  include "nut_stdint.h"
#  ifdef HAVE_SYS_RESOURCE_H
#   include <sys/resource.h> /* for getrlimit() and struct rlimit */
#   include <errno.h>

/* 3 is reserved for known overhead (for NetXML at least)
 * following practical investigation summarized at
 *   https://github.com/networkupstools/nut/pull/1158
 * and probably means the usual stdin/stdout/stderr triplet
 */
#   define RESERVE_FD_COUNT 3
#  endif /* HAVE_SYS_RESOURCE_H */
# endif  /* HAVE_PTHREAD_TRYJOIN || HAVE_SEMAPHORE */
#endif   /* HAVE_PTHREAD */

#include "nut-scan.h"

#define ERR_BAD_OPTION	(-1)

static const char optstring[] = "?ht:T:s:e:E:c:l:u:W:X:w:x:p:b:B:d:L:CUSMOAm:QnNPqIVaD";

#ifdef HAVE_GETOPT_LONG
static const struct option longopts[] = {
	{ "timeout", required_argument, NULL, 't' },
	{ "thread", required_argument, NULL, 'T' },
	{ "start_ip", required_argument, NULL, 's' },
	{ "end_ip", required_argument, NULL, 'e' },
	{ "eaton_serial", required_argument, NULL, 'E' },
	{ "mask_cidr", required_argument, NULL, 'm' },
	{ "community", required_argument, NULL, 'c' },
	{ "secLevel", required_argument, NULL, 'l' },
	{ "secName", required_argument, NULL, 'u' },
	{ "authPassword", required_argument, NULL, 'W' },
	{ "privPassword", required_argument, NULL, 'X' },
	{ "authProtocol", required_argument, NULL, 'w' },
	{ "privProtocol", required_argument, NULL, 'x' },
	{ "username", required_argument, NULL, 'b' },
	{ "password", required_argument, NULL, 'B' },
	{ "authType", required_argument, NULL, 'd' },
	{ "cipher_suite_id", required_argument, NULL, 'L' },
	{ "port", required_argument, NULL, 'p' },
	{ "complete_scan", no_argument, NULL, 'C' },
	{ "usb_scan", no_argument, NULL, 'U' },
	{ "snmp_scan", no_argument, NULL, 'S' },
	{ "xml_scan", no_argument, NULL, 'M' },
	{ "oldnut_scan", no_argument, NULL, 'O' },	/* "old" NUT libupsclient.so scan */
	{ "avahi_scan", no_argument, NULL, 'A' },	/* "new" NUT scan where deployed */
	{ "nut_simulation_scan", no_argument, NULL, 'n' },
	{ "ipmi_scan", no_argument, NULL, 'I' },
	{ "disp_nut_conf_with_sanity_check", no_argument, NULL, 'Q' },
	{ "disp_nut_conf", no_argument, NULL, 'N' },
	{ "disp_parsable", no_argument, NULL, 'P' },
	{ "quiet", no_argument, NULL, 'q' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'V' },
	{ "available", no_argument, NULL, 'a' },
	{ "nut_debug_level", no_argument, NULL, 'D' },
	{ NULL, 0, NULL, 0 }
};
#else
#define getopt_long(a,b,c,d,e)	getopt(a,b,c)
#endif /* HAVE_GETOPT_LONG */

static nutscan_device_t *dev[TYPE_END];

static useconds_t timeout = DEFAULT_NETWORK_TIMEOUT * 1000 * 1000; /* in usec */
static char * port = NULL;
static char * serial_ports = NULL;
static int cli_link_detail_level = -1;

/* Track requested IP ranges (from CLI or auto-discovery) */
typedef struct ip_range_s {
	char * start_ip;
	char * end_ip;
	struct ip_range_s * next;
} ip_range_t;
static ip_range_t * ip_ranges = NULL;
static ip_range_t * ip_ranges_last = NULL;
static size_t ip_ranges_count = 0;

static size_t add_ip_range(char * start_ip, char * end_ip)
{
	ip_range_t *p;

	if (!start_ip && !end_ip) {
		upsdebugx(5, "%s: skip, no addresses were provided", __func__);
		return ip_ranges_count;
	}

	if (start_ip == NULL) {
		upsdebugx(5, "%s: only end address was provided, setting start to same: %s",
			 __func__, end_ip);
		start_ip = end_ip;
	}
	if (end_ip == NULL) {
		upsdebugx(5, "%s: only start address was provided, setting end to same: %s",
			 __func__, start_ip);
		end_ip = start_ip;
	}

	p = xcalloc(1, sizeof(ip_range_t));

	p->start_ip = start_ip;
	p->end_ip = end_ip;
	p->next = NULL;

	if (!ip_ranges) {
		ip_ranges = p;
	}

	if (ip_ranges_last) {
		ip_ranges_last->next = p;
	}
	ip_ranges_last = p;
	ip_ranges_count++;

	upsdebugx(1, "Recorded IP address range #%" PRIuSIZE ": [%s .. %s]",
		ip_ranges_count, start_ip, end_ip);

	return ip_ranges_count;
}

static void free_ip_ranges(void)
{
	ip_range_t *p = ip_ranges;

	while (p) {
		ip_ranges = p->next;

		/* Only free the strings once, if they pointed to same */
		if (p->start_ip == p->end_ip && p->start_ip) {
			free(p->start_ip);
		} else {
			if (p->start_ip)
				free(p->start_ip);
			if (p->end_ip)
				free(p->end_ip);
		}

		free(p);
		p = ip_ranges;
	}

	ip_ranges_last = NULL;
	ip_ranges_count = 0;
}

#ifdef HAVE_PTHREAD
static pthread_t thread[TYPE_END];

static void * run_usb(void *arg)
{
	nutscan_usb_t scanopts, *scanopts_ptr = &scanopts;

	if (!arg) {
		/* null => use library defaults; should not happen here anyway */
		scanopts_ptr = NULL;
	} else {
		/* 0: do not report bus/device/busport details
		 * 1: report bus and busport, if available
		 * 2: report bus/device/busport details
		 * 3: like (2) and report bcdDevice (limited use and benefit)
		 */
		int link_detail_level = *((int*)arg);

		switch (link_detail_level) {
			case 0:
				scanopts.report_bus = 0;
				scanopts.report_busport = 0;
				scanopts.report_device = 0;
				scanopts.report_bcdDevice = 0;
				break;

			case 1:
				scanopts.report_bus = 1;
				scanopts.report_busport = 1;
				scanopts.report_device = 0;
				scanopts.report_bcdDevice = 0;
				break;

			case 2:
				scanopts.report_bus = 1;
				scanopts.report_busport = 1;
				scanopts.report_device = 1;
				scanopts.report_bcdDevice = 0;
				break;

			case 3:
				scanopts.report_bus = 1;
				scanopts.report_busport = 1;
				scanopts.report_device = 1;
				scanopts.report_bcdDevice = 1;
				break;

			default:
				upsdebugx(1, "%s: using library default link_detail_level settings", __func__);
				scanopts_ptr = NULL;
		}
	}

	dev[TYPE_USB] = nutscan_scan_usb(scanopts_ptr);
	return NULL;
}
#endif	/* HAVE_PTHREAD */

static void * run_snmp(void * arg)
{
	nutscan_snmp_t * sec = (nutscan_snmp_t *)arg;
	nutscan_device_t * dev_ret;
	ip_range_t *p = ip_ranges;

	upsdebugx(2, "Entering %s for %" PRIuSIZE " IP address range(s)",
		__func__, ip_ranges_count);

	dev[TYPE_SNMP] = NULL;
	while (p) {
		dev_ret = nutscan_scan_snmp(p->start_ip, p->end_ip, timeout, sec);
		if (!dev[TYPE_SNMP]) {
			dev[TYPE_SNMP] = dev_ret;
		} else {
			dev[TYPE_SNMP] = nutscan_rewind_device(
				nutscan_add_device_to_device(dev_ret, dev[TYPE_SNMP]));
		}
		p = p->next;
	}

	upsdebugx(2, "Finished %s loop", __func__);
	return NULL;
}

static void * run_xml(void * arg)
{
	nutscan_xml_t * sec = (nutscan_xml_t *)arg;
	nutscan_device_t * dev_ret;
	ip_range_t *p = ip_ranges;

	upsdebugx(2, "Entering %s for %" PRIuSIZE " IP address range(s)",
		__func__, ip_ranges_count);

	if (!p) {
		/* Probe broadcast */
		dev[TYPE_XML] = nutscan_scan_xml_http_range(NULL, NULL, timeout, sec);

		upsdebugx(2, "Finished %s query", __func__);
		return NULL;
	}

	dev[TYPE_XML] = NULL;
	while (p) {
		dev_ret = nutscan_scan_xml_http_range(p->start_ip, p->end_ip, timeout, sec);
		if (!dev[TYPE_XML]) {
			dev[TYPE_XML] = dev_ret;
		} else {
			dev[TYPE_XML] = nutscan_rewind_device(
				nutscan_add_device_to_device(dev_ret, dev[TYPE_XML]));
		}
		p = p->next;
	}

	upsdebugx(2, "Finished %s loop", __func__);
	return NULL;
}

static void * run_nut_old(void *arg)
{
	nutscan_device_t * dev_ret;
	ip_range_t *p = ip_ranges;
	NUT_UNUSED_VARIABLE(arg);

	upsdebugx(2, "Entering %s for %" PRIuSIZE " IP address range(s)",
		__func__, ip_ranges_count);

	dev[TYPE_NUT] = NULL;
	while (p) {
		dev_ret = nutscan_scan_nut(p->start_ip, p->end_ip, port, timeout);
		if (!dev[TYPE_NUT]) {
			dev[TYPE_NUT] = dev_ret;
		} else {
			dev[TYPE_NUT] = nutscan_rewind_device(
				nutscan_add_device_to_device(dev_ret, dev[TYPE_NUT]));
		}
		p = p->next;
	}

	upsdebugx(2, "Finished %s loop", __func__);
	return NULL;
}

static void * run_nut_simulation(void *arg)
{
	NUT_UNUSED_VARIABLE(arg);

	dev[TYPE_NUT_SIMULATION] = nutscan_scan_nut_simulation();
	return NULL;
}

static void * run_avahi(void *arg)
{
	NUT_UNUSED_VARIABLE(arg);

	dev[TYPE_AVAHI] = nutscan_scan_avahi(timeout);
	return NULL;
}

static void * run_ipmi(void * arg)
{
	nutscan_ipmi_t * sec = (nutscan_ipmi_t *)arg;
	nutscan_device_t * dev_ret;
	ip_range_t *p = ip_ranges;

	upsdebugx(2, "Entering %s for %" PRIuSIZE " IP address range(s)",
		__func__, ip_ranges_count);

	if (!p) {
		/* Probe local device */
		dev[TYPE_IPMI] = nutscan_scan_ipmi(NULL, NULL, sec);

		upsdebugx(2, "Finished %s query", __func__);
		return NULL;
	}

	dev[TYPE_IPMI] = NULL;
	while (p) {
		dev_ret = nutscan_scan_ipmi(p->start_ip, p->end_ip, sec);
		if (!dev[TYPE_IPMI]) {
			dev[TYPE_IPMI] = dev_ret;
		} else {
			dev[TYPE_IPMI] = nutscan_rewind_device(
				nutscan_add_device_to_device(dev_ret, dev[TYPE_IPMI]));
		}
		p = p->next;
	}

	upsdebugx(2, "Finished %s loop", __func__);
	return NULL;
}

static void * run_eaton_serial(void *arg)
{
	char * arg_serial_ports = (char *)arg;

	dev[TYPE_EATON_SERIAL] = nutscan_scan_eaton_serial(arg_serial_ports);
	return NULL;
}

static void show_usage(void)
{
/* NOTE: This code uses `nutscan_avail_*` global vars from nutscan-init.c */
	puts("nut-scanner : utility for detection of available power devices.\n");

	nut_report_config_flags();

	puts("OPTIONS:");
	printf("  -C, --complete_scan: Scan all available devices except serial ports (default).\n");
	if (nutscan_avail_usb) {
		printf("  -U, --usb_scan: Scan USB devices. Specify twice or more to report different\n"
			"                  detail levels of (change-prone) physical properties.\n"
			"                  This usage can be combined with '-C' or other scan types.\n");
	} else {
		printf("* Options for USB devices scan not enabled: library not detected.\n");
	}
	if (nutscan_avail_snmp) {
		printf("  -S, --snmp_scan: Scan SNMP devices using built-in mapping definitions.\n");
	} else {
		printf("* Options for SNMP devices scan not enabled: library not detected.\n");
	}
	if (nutscan_avail_xml_http) {
		printf("  -M, --xml_scan: Scan XML/HTTP devices.\n");
	} else {
		printf("* Options for XML/HTTP devices scan not enabled: library not detected.\n");
	}
	printf("  -O, --oldnut_scan: Scan NUT devices (old method via libupsclient).\n");
	if (nutscan_avail_avahi) {
		printf("  -A, --avahi_scan: Scan NUT devices (new avahi method).\n");
	} else {
		printf("* Options for NUT devices (new avahi method) scan not enabled: library not detected.\n");
	}
	printf("  -n, --nut_simulation_scan: Scan for NUT simulated devices (.dev files in $CONFPATH).\n");
	if (nutscan_avail_ipmi) {
		printf("  -I, --ipmi_scan: Scan IPMI devices.\n");
	} else {
		printf("* Options for IPMI devices scan not enabled: library not detected.\n");
	}

	printf("  -E, --eaton_serial <serial ports list>: Scan serial Eaton devices (XCP, SHUT and Q1).\n");

#if (defined HAVE_PTHREAD) && (defined HAVE_PTHREAD_TRYJOIN)
	printf("  -T, --thread <max number of threads>: Limit the amount of scanning threads running simultaneously (default: %" PRIuSIZE ").\n", max_threads);
#else
	printf("  -T, --thread <max number of threads>: Limit the amount of scanning threads running simultaneously (not implemented in this build: no pthread support)\n");
#endif

	printf("\nNote: many scanning options depend on further loadable libraries.\n");
	/* Note: if debug is enabled, this is prefixed with timestamps */
	upsdebugx_report_search_paths(0, 0);

	printf("\nNetwork specific options:\n");
	printf("  -t, --timeout <timeout in seconds>: network operation timeout (default %d).\n", DEFAULT_NETWORK_TIMEOUT);
	printf("  -s, --start_ip <IP address>: First IP address to scan.\n");
	printf("  -e, --end_ip <IP address>: Last IP address to scan.\n");
	printf("  -m, --mask_cidr <IP address/mask>: Give a range of IP using CIDR notation.\n");
	printf("  -m, --mask_cidr auto: Detect local IP address(es) and scan corresponding subnet(s).\n");
#ifdef WIN32
	printf("                        (Currently not implemented for this platform)\n");
#endif
	printf("  -m, --mask_cidr auto4/auto6: Likewise, limiting to IPv4 or IPv6 interfaces.\n");
	printf("                        Only the first auto* request would be honoured.\n");
	printf("NOTE: IP address range specifications can be repeated, to scan several.\n");
	printf("Specifying a single first or last address before starting another range\n");
	printf("leads to scanning just that one address as the range.\n");

	if (nutscan_avail_snmp) {
		printf("\nSNMP v1 specific options:\n");
		printf("  -c, --community <community name>: Set SNMP v1 community name (default = public)\n");

		printf("\nSNMP v3 specific options:\n");
		printf("  -l, --secLevel <security level>: Set the securityLevel used for SNMPv3 messages (allowed values: noAuthNoPriv, authNoPriv, authPriv)\n");
		printf("  -u, --secName <security name>: Set the securityName used for authenticated SNMPv3 messages (mandatory if you set secLevel. No default)\n");

		/* Construct help for AUTHPROTO */
		{ int comma = 0;
		NUT_UNUSED_VARIABLE(comma); /* potentially, if no protocols are available */
		printf("  -w, --authProtocol <authentication protocol>: Set the authentication protocol (");
#if (defined WITH_SNMP) && (defined NUT_HAVE_LIBNETSNMP_usmHMACMD5AuthProtocol)
/* Note: NUT_HAVE_LIBNETSNMP_* macros are not AC_DEFINE'd when libsnmp was
 * completely not detected at configure time, so "#if" is not a pedantically
 * correct test (unknown macro may default to "0" but is not guaranteed to).
 */
# if NUT_HAVE_LIBNETSNMP_usmHMACMD5AuthProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"MD5"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_usmHMACSHA1AuthProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"SHA"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_usmHMAC192SHA256AuthProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"SHA256"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_usmHMAC256SHA384AuthProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"SHA384"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_usmHMAC384SHA512AuthProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"SHA512"
			);
# endif
		printf("%s%s",
			(comma ? "" : "none supported"),
			") used for authenticated SNMPv3 messages (default=MD5 if available)\n"
			);
		} /* Construct help for AUTHPROTO */

		printf("  -W, --authPassword <authentication pass phrase>: Set the authentication pass phrase used for authenticated SNMPv3 messages (mandatory if you set secLevel to authNoPriv or authPriv)\n");

		/* Construct help for PRIVPROTO */
		{ int comma = 0;
		NUT_UNUSED_VARIABLE(comma); /* potentially, if no protocols are available */
		printf("  -x, --privProtocol <privacy protocol>: Set the privacy protocol (");
# if NUT_HAVE_LIBNETSNMP_usmDESPrivProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"DES"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_usmAESPrivProtocol || NUT_HAVE_LIBNETSNMP_usmAES128PrivProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"AES"
			);
# endif
# if NUT_HAVE_LIBNETSNMP_DRAFT_BLUMENTHAL_AES_04
#  if NUT_HAVE_LIBNETSNMP_usmAES192PrivProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"AES192"
			);
#  endif
#  if NUT_HAVE_LIBNETSNMP_usmAES256PrivProtocol
		printf("%s%s",
			(comma++ ? ", " : ""),
			"AES256"
			);
#  endif
# endif /* NUT_HAVE_LIBNETSNMP_DRAFT_BLUMENTHAL_AES_04 */
#endif /* built WITH_SNMP */
		printf("%s%s",
			(comma ? "" : "none supported"),
			") used for encrypted SNMPv3 messages (default=DES if available)\n"
			);
		} /* Construct help for PRIVPROTO */

		printf("  -X, --privPassword <privacy pass phrase>: Set the privacy pass phrase used for encrypted SNMPv3 messages (mandatory if you set secLevel to authPriv)\n");
	}

	if (nutscan_avail_ipmi) {
		printf("\nIPMI over LAN specific options:\n");
		printf("  -b, --username <username>: Set the username used for authenticating IPMI over LAN connections (mandatory for IPMI over LAN. No default)\n");
		/* Specify  the  username  to  use  when authenticating with the remote host.  If not specified, a null (i.e. anonymous) username is assumed. The user must have
		 * at least ADMIN privileges in order for this tool to operate fully. */
		printf("  -B, --password <password>: Specify the password to use when authenticationg with the remote host (mandatory for IPMI over LAN. No default)\n");
		/* Specify the password to use when authenticationg with the remote host.  If not specified, a null password is assumed. Maximum password length is 16 for IPMI
		 * 1.5 and 20 for IPMI 2.0. */
		printf("  -d, --authType <authentication type>: Specify the IPMI 1.5 authentication type to use (NONE, STRAIGHT_PASSWORD_KEY, MD2, and MD5) with the remote host (default=MD5)\n");
		printf("  -L, --cipher_suite_id <cipher suite id>: Specify the IPMI 2.0 cipher suite ID to use, for authentication, integrity, and confidentiality (default=3)\n");
	}

	printf("\nNUT specific options:\n");
	printf("  -p, --port <port number>: Port number of remote NUT upsd\n");
	printf("\ndisplay specific options:\n");
	printf("  -Q, --disp_nut_conf_with_sanity_check: Display result in the ups.conf format with sanity-check warnings as comments (default)\n");
	printf("  -N, --disp_nut_conf: Display result in the ups.conf format\n");
	printf("  -P, --disp_parsable: Display result in a parsable format\n");
	printf("\nMiscellaneous options:\n");
	printf("  -h, --help: display this help text\n");
	printf("  -V, --version: Display NUT version\n");
	printf("  -a, --available: Display available bus that can be scanned\n");
	printf("  -q, --quiet: Display only scan result. No information on currently scanned bus is displayed.\n");
	printf("  -D, --nut_debug_level: Raise the debugging level.  Use this multiple times to see more details.\n");
}

int main(int argc, char *argv[])
{
	nutscan_snmp_t snmp_sec;
	nutscan_ipmi_t ipmi_sec;
	nutscan_xml_t  xml_sec;
	int opt_ret;
	char *start_ip = NULL, *end_ip = NULL;
	int auto_nets = 0;
	int allow_all = 0;
	int allow_usb = 0;
	int allow_snmp = 0;
	int allow_xml = 0;
	int allow_oldnut = 0;
	int allow_nut_simulation = 0;
	int allow_avahi = 0;
	int allow_ipmi = 0;
	int allow_eaton_serial = 0; /* MUST be requested explicitly! */
	int quiet = 0; /* The debugging level for certain upsdebugx() progress messages; 0 = print always, quiet==1 is to require at least one -D */
	void (*display_func)(nutscan_device_t * device);
	int ret_code = EXIT_SUCCESS;
#ifdef HAVE_PTHREAD
# ifdef HAVE_SEMAPHORE
	sem_t	*current_sem;
# endif
#endif
#if (defined HAVE_PTHREAD) && ( (defined HAVE_PTHREAD_TRYJOIN) || (defined HAVE_SEMAPHORE) ) && (defined HAVE_SYS_RESOURCE_H)
	struct rlimit nofile_limit;

	/* Limit the max scanning thread count by the amount of allowed open
	 * file descriptors (which caller can change with `ulimit -n NUM`),
	 * following practical investigation summarized at
	 *   https://github.com/networkupstools/nut/pull/1158
	 * Resource-Limit code inspired by example from:
	 *   https://stackoverflow.com/questions/4076848/how-to-do-the-equivalent-of-ulimit-n-400-from-within-c/4077000#4077000
	 */

	/* Get max number of files. */
	if (getrlimit(RLIMIT_NOFILE, &nofile_limit) != 0) {
		/* Report error, keep hardcoded default */
		fprintf(stderr, "getrlimit() failed with errno=%d, keeping default job limits\n", errno);
		nofile_limit.rlim_cur = 0;
		nofile_limit.rlim_max = 0;
	} else {
		if (nofile_limit.rlim_cur > 0
		&&  nofile_limit.rlim_cur > RESERVE_FD_COUNT
		&&  (uintmax_t)max_threads > (uintmax_t)(nofile_limit.rlim_cur - RESERVE_FD_COUNT)
		&&  (uintmax_t)(nofile_limit.rlim_cur) < (uintmax_t)SIZE_MAX
		) {
			max_threads = (size_t)nofile_limit.rlim_cur;
			if (max_threads > (RESERVE_FD_COUNT + 1)) {
				max_threads -= RESERVE_FD_COUNT;
			}
		}
	}
#endif /* HAVE_PTHREAD && ( HAVE_PTHREAD_TRYJOIN || HAVE_SEMAPHORE ) && HAVE_SYS_RESOURCE_H */

	memset(&snmp_sec, 0, sizeof(snmp_sec));
	memset(&ipmi_sec, 0, sizeof(ipmi_sec));
	memset(&xml_sec, 0, sizeof(xml_sec));

	/* Set the default values for IPMI */
	ipmi_sec.authentication_type = IPMI_AUTHENTICATION_TYPE_MD5;
	ipmi_sec.ipmi_version = IPMI_1_5; /* default to IPMI 1.5, if not otherwise specified */
	ipmi_sec.cipher_suite_id = 3; /* default to HMAC-SHA1; HMAC-SHA1-96; AES-CBC-128 */
	ipmi_sec.privilege_level = IPMI_PRIVILEGE_LEVEL_ADMIN; /* should be sufficient */

	/* Set the default values for XML HTTP (run_xml()) */
	xml_sec.port_http = 80;
	xml_sec.port_udp = 4679;
	xml_sec.usec_timeout = 0; /* Override with the "timeout" common setting later */
	xml_sec.peername = NULL;

	/* Parse command line options -- First loop: only get debug level */
	/* Suppress error messages, for now -- leave them to the second loop. */
	opterr = 0;
	while ((opt_ret = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
		if (opt_ret == 'D')
			nut_debug_level++;
	}

	nutscan_init();

	/* Default, see -Q/-N/-P below */
	display_func = nutscan_display_ups_conf_with_sanity_check;

	/* Parse command line options -- Second loop: everything else */
	/* Restore error messages... */
	opterr = 1;
	/* ...and index of the item to be processed by getopt(). */
	optind = 1;
	/* Note: the getopts print an error message about unknown arguments
	 * or arguments which need a second token and that is missing now */
	while ((opt_ret = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {

		switch(opt_ret) {
			case 't':
				timeout = (useconds_t)atol(optarg) * 1000 * 1000; /*in usec*/
				if (timeout <= 0) {
					fprintf(stderr,
						"Illegal timeout value, using default %ds\n",
						DEFAULT_NETWORK_TIMEOUT);
					timeout = DEFAULT_NETWORK_TIMEOUT * 1000 * 1000;
				}
				break;
			case 's':
				if (start_ip) {
					/* Save whatever we have, either
					 * this one address or an earlier
					 * known range with its end */
					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}

				start_ip = strdup(optarg);
				if (end_ip != NULL) {
					/* Already we know two addresses, save them */
					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}
				break;
			case 'e':
				if (end_ip) {
					/* Save whatever we have, either
					 * this one address or an earlier
					 * known range with its start */
					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}

				end_ip = strdup(optarg);
				if (start_ip != NULL) {
					/* Already we know two addresses, save them */
					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}
				break;
			case 'E':
				serial_ports = strdup(optarg);
				allow_eaton_serial = 1;
				break;
			case 'm':
				if (start_ip || end_ip) {
					/* Save whatever we have, either
					 * this one address or an earlier
					 * known range with its start or end */
					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}

				if (!strcmp(optarg, "auto") || !strcmp(optarg, "auto4") || !strcmp(optarg, "auto6")) {
					if (auto_nets) {
						fprintf(stderr, "Duplicate request for connected subnet scan ignored\n");
					} else {
#ifndef WIN32
						/* TODO: Refactor into a method, reduce indentation? */
						/* Inspired by https://stackoverflow.com/a/63789267/4715872 */
						struct ifaddrs *ifap;
#endif

						if (!strcmp(optarg, "auto")) {
							auto_nets = 46;
						} else if (!strcmp(optarg, "auto4")) {
							auto_nets = 4;
						} else if (!strcmp(optarg, "auto6")) {
							auto_nets = 6;
						}

#ifndef WIN32
						if (getifaddrs(&ifap) < 0) {
							fatalx(EXIT_FAILURE,
								"Failed to getifaddrs() for connected subnet scan: %s\n",
								strerror(errno));
						} else {
							struct ifaddrs *ifa;
							char msg[LARGEBUF];
							/* Note: INET6_ADDRSTRLEN is large enough for IPv4 too,
							 * and is smaller than LARGEBUF to avoid snprintf()
							 * warnings that the result might not fit. */
							char addr[INET6_ADDRSTRLEN];
							char mask[INET6_ADDRSTRLEN];
							int masklen = 0;

							for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
								if (ifa->ifa_addr) {
									memset(msg, 0, sizeof(msg));
									memset(addr, 0, sizeof(addr));
									memset(mask, 0, sizeof(mask));
									masklen = -1;

									if (ifa->ifa_addr->sa_family == AF_INET6) {
										uint8_t i, j;

										/* Ensure proper alignment */
										struct sockaddr_in6 sm;
										memcpy (&sm, ifa->ifa_netmask, sizeof(struct sockaddr_in6));

										masklen = 0;
										for (j = 0; j < 16; j++) {
											i = sm.sin6_addr.s6_addr[j];
											while (i) {
												masklen += i & 1;
												i >>= 1;
											}
										}

										getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in6), addr, sizeof(addr), NULL, 0, NI_NUMERICHOST);
										getnameinfo(ifa->ifa_netmask, sizeof(struct sockaddr_in6), mask, sizeof(mask), NULL, 0, NI_NUMERICHOST);
										snprintf(msg, sizeof(msg), "Interface: %s\tAddress: %s\tMask: %s (len: %i)\tFlags: %08" PRIxMAX, ifa->ifa_name, addr, mask, masklen, (uintmax_t)ifa->ifa_flags);
									} else if (ifa->ifa_addr->sa_family == AF_INET) {
										in_addr_t i;

										/* Ensure proper alignment */
										struct sockaddr_in sa, sm;
										memcpy (&sa, ifa->ifa_addr, sizeof(struct sockaddr_in));
										memcpy (&sm, ifa->ifa_netmask, sizeof(struct sockaddr_in));
										snprintf(addr, sizeof(addr), "%s", inet_ntoa(sa.sin_addr));
										snprintf(mask, sizeof(mask), "%s", inet_ntoa(sm.sin_addr));

										i = sm.sin_addr.s_addr;
										masklen = 0;
										while (i) {
											masklen += i & 1;
											i >>= 1;
										}
										snprintf(msg, sizeof(msg), "Interface: %s\tAddress: %s\tMask: %s (len: %i)\tFlags: %08" PRIxMAX, ifa->ifa_name, addr, mask, masklen, (uintmax_t)ifa->ifa_flags);
/*
									} else {
										snprintf(msg, sizeof(msg), "Addr family: %" PRIuMAX, (intmax_t)ifa->ifa_addr->sa_family);
*/
									}

									if (ifa->ifa_addr->sa_family == AF_INET6 || ifa->ifa_addr->sa_family == AF_INET) {
										if (ifa->ifa_flags & IFF_LOOPBACK)
											snprintfcat(msg, sizeof(msg), " IFF_LOOPBACK");
										if (ifa->ifa_flags & IFF_UP)
											snprintfcat(msg, sizeof(msg), " IFF_UP");
										if (ifa->ifa_flags & IFF_RUNNING)
											snprintfcat(msg, sizeof(msg), " IFF_RUNNING");
										if (ifa->ifa_flags & IFF_BROADCAST)
											snprintfcat(msg, sizeof(msg), " IFF_BROADCAST(is assigned)");

										upsdebugx(5, "Discovering getifaddrs(): %s", msg);

										/* TODO: also rule out "link-local" address ranges
										 * so we do not issue billions of worthless scans.
										 * FIXME: IPv6 may also be a problem, see
										 * https://github.com/networkupstools/nut/issues/2512
										 */
										if (!(ifa->ifa_flags & IFF_LOOPBACK)
										&&   (ifa->ifa_flags & IFF_UP)
										&&   (ifa->ifa_flags & IFF_RUNNING)
										&&   (ifa->ifa_flags & IFF_BROADCAST)
										&&  (auto_nets == 46
										  || (auto_nets == 4 && ifa->ifa_addr->sa_family == AF_INET)
										  || (auto_nets == 6 && ifa->ifa_addr->sa_family == AF_INET6) )
										) {
											char cidr[LARGEBUF];

											if (snprintf(cidr, sizeof(cidr), "%s/%i", addr, masklen) < 0) {
												fatalx(EXIT_FAILURE, "Could not construct a CIDR string from discovered address/mask");
											}

											upsdebugx(5, "Processing CIDR net/mask: %s", cidr);
											nutscan_cidr_to_ip(cidr, &start_ip, &end_ip);
											upsdebugx(5, "Extracted IP address range from CIDR net/mask: %s => %s", start_ip, end_ip);

											add_ip_range(start_ip, end_ip);
											start_ip = NULL;
											end_ip = NULL;
										}
									}	/* else AF_UNIX or a dozen other types we do not care about here */
								}
							}
							freeifaddrs(ifap);
						}
#else	/* WIN32 */
						/* https://stackoverflow.com/questions/122208/how-can-i-get-the-ip-address-of-a-local-computer */
						upsdebugx(0, "Local address detection feature is not completed on Windows, please call back later");
#endif
					}
				} else {
					/* not `-m auto` => is `-m cidr` */
					upsdebugx(5, "Processing CIDR net/mask: %s", optarg);
					nutscan_cidr_to_ip(optarg, &start_ip, &end_ip);
					upsdebugx(5, "Extracted IP address range from CIDR net/mask: %s => %s", start_ip, end_ip);

					add_ip_range(start_ip, end_ip);
					start_ip = NULL;
					end_ip = NULL;
				}
				break;
			case 'D':
				/* nothing to do, here */
				break;
			case 'c':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.community = strdup(optarg);
				break;
			case 'l':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.secLevel = strdup(optarg);
				break;
			case 'u':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.secName = strdup(optarg);
				break;
			case 'W':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.authPassword = strdup(optarg);
				break;
			case 'X':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.privPassword = strdup(optarg);
				break;
			case 'w':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.authProtocol = strdup(optarg);
				break;
			case 'x':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				snmp_sec.privProtocol = strdup(optarg);
				break;
			case 'S':
				if (!nutscan_avail_snmp) {
					goto display_help;
				}
				allow_snmp = 1;
				break;
			case 'b':
				if (!nutscan_avail_ipmi) {
					goto display_help;
				}
				ipmi_sec.username = strdup(optarg);
				break;
			case 'B':
				if (!nutscan_avail_ipmi) {
					goto display_help;
				}
				ipmi_sec.password = strdup(optarg);
				break;
			case 'd':
				if (!nutscan_avail_ipmi) {
					goto display_help;
				}
				if (!strcmp(optarg, "NONE")) {
					ipmi_sec.authentication_type = IPMI_AUTHENTICATION_TYPE_NONE;
				}
				else if (!strcmp(optarg, "STRAIGHT_PASSWORD_KEY")) {
					ipmi_sec.authentication_type = IPMI_AUTHENTICATION_TYPE_STRAIGHT_PASSWORD_KEY;
				}
				else if (!strcmp(optarg, "MD2")) {
					ipmi_sec.authentication_type = IPMI_AUTHENTICATION_TYPE_MD2;
				}
				else if (!strcmp(optarg, "MD5")) {
					ipmi_sec.authentication_type = IPMI_AUTHENTICATION_TYPE_MD5;
				}
				else {
					fprintf(stderr,
						"Unknown authentication type (%s). Defaulting to MD5\n",
						optarg);
				}
				break;
			case 'L':
				if (!nutscan_avail_ipmi) {
					goto display_help;
				}
				ipmi_sec.cipher_suite_id = atoi(optarg);
				/* Force IPMI 2.0! */
				ipmi_sec.ipmi_version = IPMI_2_0;
				break;
			case 'p':
				port = strdup(optarg);
				break;
			case 'T': {
#if (defined HAVE_PTHREAD) && ( (defined HAVE_PTHREAD_TRYJOIN) || (defined HAVE_SEMAPHORE) )
				char* endptr;
				long val = strtol(optarg, &endptr, 10);
				/* With endptr we check that no chars were left in optarg
				 * (that is, pointed-to char -- if reported -- is '\0')
				 */
				if ((!endptr || !*endptr)
				&& val > 0
				&& (uintmax_t)val < (uintmax_t)SIZE_MAX
				) {
# ifdef HAVE_SYS_RESOURCE_H
					if (nofile_limit.rlim_cur > 0
					&&  nofile_limit.rlim_cur > RESERVE_FD_COUNT
					&& (uintmax_t)nofile_limit.rlim_cur < (uintmax_t)SIZE_MAX
					&& (uintmax_t)val > (uintmax_t)(nofile_limit.rlim_cur - RESERVE_FD_COUNT)
					) {
						upsdebugx(1, "Detected soft limit for "
							"file descriptor count is %" PRIuMAX,
							(uintmax_t)nofile_limit.rlim_cur);
						upsdebugx(1, "Detected hard limit for "
							"file descriptor count is %" PRIuMAX,
							(uintmax_t)nofile_limit.rlim_max);

						max_threads = (size_t)nofile_limit.rlim_cur;
						if (max_threads > (RESERVE_FD_COUNT + 1)) {
							max_threads -= RESERVE_FD_COUNT;
						}

						fprintf(stderr,
							"WARNING: Requested max scanning "
							"thread count %s (%ld) exceeds the "
							"current file descriptor count limit "
							"(minus reservation), constraining "
							"to %" PRIuSIZE "\n",
							optarg, val, max_threads);
					} else
# endif /* HAVE_SYS_RESOURCE_H */
						max_threads = (size_t)val;
				} else {
					fprintf(stderr,
						"WARNING: Requested max scanning "
						"thread count %s (%ld) is out of range, "
						"using default %" PRIuSIZE "\n",
						optarg, val, max_threads);
				}
#else
				fprintf(stderr,
					"WARNING: Max scanning thread count option "
					"is not supported in this build, ignored\n");
#endif /* HAVE_PTHREAD && ways to limit the thread count */
				}
				break;
			case 'C':
				allow_all = 1;
				break;
			case 'U':
				if (!nutscan_avail_usb) {
					goto display_help;
				}
				allow_usb = 1;
				/* NOTE: Starts as -1, so the first -U sets it to 0
				 * (minimal detail); further -U can bump it */
				if (cli_link_detail_level < 3)
					cli_link_detail_level++;
				break;
			case 'M':
				if (!nutscan_avail_xml_http) {
					goto display_help;
				}
				allow_xml = 1;
				break;
			case 'O':
				allow_oldnut = 1;
				break;
			case 'A':
				if (!nutscan_avail_avahi) {
					goto display_help;
				}
				allow_avahi = 1;
				break;
			case 'n':
				allow_nut_simulation = 1;
				break;
			case 'I':
				if (!nutscan_avail_ipmi) {
					goto display_help;
				}
				allow_ipmi = 1;
				break;
			case 'Q':
				display_func = nutscan_display_ups_conf_with_sanity_check;
				break;
			case 'N':
				display_func = nutscan_display_ups_conf;
				break;
			case 'P':
				display_func = nutscan_display_parsable;
				break;
			case 'q':
				quiet = 1;
				break;
			case 'V':
				printf("Network UPS Tools - %s\n", NUT_VERSION_MACRO);
				nut_report_config_flags();
				exit(EXIT_SUCCESS);
			case 'a':
				printf("OLDNUT\n");
				if (nutscan_avail_usb) {
					printf("USB\n");
				}
				if (nutscan_avail_snmp) {
					printf("SNMP\n");
				}
				if (nutscan_avail_xml_http) {
					printf("XML\n");
				}
				if (nutscan_avail_avahi) {
					printf("AVAHI\n");
				}
				if (nutscan_avail_ipmi) {
					printf("IPMI\n");
				}
				printf("EATON_SERIAL\n");
				exit(EXIT_SUCCESS);
			case '?':
				ret_code = ERR_BAD_OPTION;
				goto display_help;
				/* Fall through to usage and error exit */
			case 'h':
			default:
display_help:
				show_usage();
				if ((opt_ret != 'h') || (ret_code != EXIT_SUCCESS))
					fprintf(stderr, "\n\n"
						"WARNING: Some error has occurred while processing 'nut-scanner' command-line\n"
						"arguments, see more details above the usage help text.\n\n");
				return ret_code;
		}
	}

#ifdef HAVE_PTHREAD
# ifdef HAVE_SEMAPHORE
	/* FIXME: Currently sem_init already done on nutscan-init for lib need.
	   We need to destroy it before re-init. We currently can't change "sem value"
	   on lib (need to be thread safe). */
	current_sem = nutscan_semaphore();
	sem_destroy(current_sem);
#ifdef HAVE_PRAGMAS_FOR_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic push
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic ignored "-Wunreachable-code"
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif
	/* Different platforms, different sizes, none fits all... */
	if (SIZE_MAX > UINT_MAX && max_threads > UINT_MAX) {
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef HAVE_PRAGMAS_FOR_GCC_DIAGNOSTIC_IGNORED_UNREACHABLE_CODE
#pragma GCC diagnostic pop
#endif
		fprintf(stderr, "\n\n"
			"WARNING: Limiting max_threads to range acceptable for sem_init()\n\n");
		max_threads = UINT_MAX - 1;
	}
	sem_init(current_sem, 0, (unsigned int)max_threads);
# endif
#endif /* HAVE_PTHREAD */

	if (start_ip != NULL || end_ip != NULL) {
		/* Something did not cancel out above */
		add_ip_range(start_ip, end_ip);
		start_ip = NULL;
		end_ip = NULL;
	}

	if (!allow_usb && !allow_snmp && !allow_xml && !allow_oldnut && !allow_nut_simulation &&
		!allow_avahi && !allow_ipmi && !allow_eaton_serial
	) {
		allow_all = 1;
	}

	if (allow_all) {
		allow_usb = 1;
		/* NOTE: Starts as -1, so when we scan everything - set
		 * it to 0 (minimal detail); further -U can bump it */
		if (cli_link_detail_level < 0)
			cli_link_detail_level++;

		allow_snmp = 1;
		allow_xml = 1;
		allow_oldnut = 1;
		allow_nut_simulation = 1;
		allow_avahi = 1;
		allow_ipmi = 1;
		/* BEWARE: allow_all does not include allow_eaton_serial! */
	}

/* TODO/discuss : Should the #else...#endif code below for lack of pthreads
 * during build also serve as a fallback for pthread failure at runtime?
 */
	if (allow_usb && nutscan_avail_usb) {
		upsdebugx(quiet, "Scanning USB bus.");
#ifdef HAVE_PTHREAD
		if (pthread_create(&thread[TYPE_USB], NULL, run_usb, &cli_link_detail_level)) {
			upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
			nutscan_avail_usb = 0;
		}
#else
		upsdebugx(1, "USB SCAN: no pthread support, starting nutscan_scan_usb...");
		/* Not calling run_usb() here, as it re-processes the arg */
		dev[TYPE_USB] = nutscan_scan_usb(&cli_link_detail_level);
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "USB SCAN: not requested or supported, SKIPPED");
	}

	if (allow_snmp && nutscan_avail_snmp) {
		if (!ip_ranges_count) {
			upsdebugx(quiet, "No IP range(s) requested, skipping SNMP");
			nutscan_avail_snmp = 0;
		}
		else {
			upsdebugx(quiet, "Scanning SNMP bus.");
#ifdef HAVE_PTHREAD
			upsdebugx(1, "SNMP SCAN: starting pthread_create with run_snmp...");
			if (pthread_create(&thread[TYPE_SNMP], NULL, run_snmp, &snmp_sec)) {
				upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
				nutscan_avail_snmp = 0;
			}
#else
			upsdebugx(1, "SNMP SCAN: no pthread support, starting nutscan_scan_snmp...");
			/* dev[TYPE_SNMP] = nutscan_scan_snmp(start_ip, end_ip, timeout, &snmp_sec); */
			run_snmp(&snmp_sec);
#endif /* HAVE_PTHREAD */
		}
	} else {
		upsdebugx(1, "SNMP SCAN: not requested or supported, SKIPPED");
	}

	if (allow_xml && nutscan_avail_xml_http) {
		/* NOTE: No check for ip_ranges_count,
		 * NetXML default scan is broadcast
		 * so it just runs (if requested and
		 * supported).
		 */
		upsdebugx(quiet, "Scanning XML/HTTP bus.");
		xml_sec.usec_timeout = timeout;
#ifdef HAVE_PTHREAD
		upsdebugx(1, "XML/HTTP SCAN: starting pthread_create with run_xml...");
		if (pthread_create(&thread[TYPE_XML], NULL, run_xml, &xml_sec)) {
			upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
			nutscan_avail_xml_http = 0;
		}
#else
		upsdebugx(1, "XML/HTTP SCAN: no pthread support, starting nutscan_scan_xml_http_range()...");
		/* dev[TYPE_XML] = nutscan_scan_xml_http_range(start_ip, end_ip, timeout, &xml_sec); */
		run_xml(&xml_sec);
		}
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "XML/HTTP SCAN: not requested or supported, SKIPPED");
	}

	if (allow_oldnut && nutscan_avail_nut) {
		if (!ip_ranges_count) {
			upsdebugx(quiet, "No IP range(s) requested, skipping NUT bus (old libupsclient connect method)");
			nutscan_avail_nut = 0;
		}
		else {
			upsdebugx(quiet, "Scanning NUT bus (old libupsclient connect method).");
#ifdef HAVE_PTHREAD
			upsdebugx(1, "NUT bus (old) SCAN: starting pthread_create with run_nut_old...");
			if (pthread_create(&thread[TYPE_NUT], NULL, run_nut_old, NULL)) {
				upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
				nutscan_avail_nut = 0;
			}
#else
			upsdebugx(1, "NUT bus (old) SCAN: no pthread support, starting nutscan_scan_nut...");
			/*dev[TYPE_NUT] = nutscan_scan_nut(start_ip, end_ip, port, timeout);*/
			run_nut_old(NULL);
#endif /* HAVE_PTHREAD */
		}
	} else {
		upsdebugx(1, "NUT bus (old) SCAN: not requested or supported, SKIPPED");
	}

	if (allow_nut_simulation && nutscan_avail_nut_simulation) {
		upsdebugx(quiet, "Scanning NUT simulation devices.");
#ifdef HAVE_PTHREAD
		upsdebugx(1, "NUT simulation devices SCAN: starting pthread_create with run_nut_simulation...");
		if (pthread_create(&thread[TYPE_NUT_SIMULATION], NULL, run_nut_simulation, NULL)) {
			upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
			nutscan_avail_nut_simulation = 0;
		}
#else
		upsdebugx(1, "NUT simulation devices SCAN: no pthread support, starting nutscan_scan_nut_simulation...");
		/* dev[TYPE_NUT_SIMULATION] = nutscan_scan_nut_simulation(); */
		run_nut_simulation(NULL);
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "NUT simulation devices SCAN: not requested or supported, SKIPPED");
	}

	if (allow_avahi && nutscan_avail_avahi) {
		upsdebugx(quiet, "Scanning NUT bus (avahi method).");
#ifdef HAVE_PTHREAD
		upsdebugx(1, "NUT bus (avahi) SCAN: starting pthread_create with run_avahi...");
		if (pthread_create(&thread[TYPE_AVAHI], NULL, run_avahi, NULL)) {
			upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
			nutscan_avail_avahi = 0;
		}
#else
		upsdebugx(1, "NUT bus (avahi) SCAN: no pthread support, starting nutscan_scan_avahi...");
		/* dev[TYPE_AVAHI] = nutscan_scan_avahi(timeout); */
		run_avahi(NULL);
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "NUT bus (avahi) SCAN: not requested or supported, SKIPPED");
	}

	if (allow_ipmi && nutscan_avail_ipmi) {
		/* NOTE: No check for ip_ranges_count,
		 * IPMI default scan is local device
		 * so it just runs (if requested and
		 * supported).
		 */
		upsdebugx(quiet, "Scanning IPMI bus.");
#ifdef HAVE_PTHREAD
		upsdebugx(1, "IPMI SCAN: starting pthread_create with run_ipmi...");
		if (pthread_create(&thread[TYPE_IPMI], NULL, run_ipmi, &ipmi_sec)) {
			upsdebugx(1, "pthread_create returned an error; disabling this scan mode");
			nutscan_avail_ipmi = 0;
		}
#else
		upsdebugx(1, "IPMI SCAN: no pthread support, starting nutscan_scan_ipmi...");
		/* dev[TYPE_IPMI] = nutscan_scan_ipmi(start_ip, end_ip, &ipmi_sec); */
		run_ipmi(&ipmi_sec);
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "IPMI SCAN: not requested or supported, SKIPPED");
	}

	/* Eaton serial scan */
	if (allow_eaton_serial) {
		upsdebugx(quiet, "Scanning serial bus for Eaton devices.");
#ifdef HAVE_PTHREAD
		upsdebugx(1, "SERIAL SCAN: starting pthread_create with run_eaton_serial (return not checked!)...");
		pthread_create(&thread[TYPE_EATON_SERIAL], NULL, run_eaton_serial, serial_ports);
		/* FIXME: check return code */
		/* upsdebugx(1, "pthread_create returned an error; disabling this scan mode"); */
		/* nutscan_avail_eaton_serial(?) = 0; */
#else
		upsdebugx(1, "SERIAL SCAN: no pthread support, starting nutscan_scan_eaton_serial...");
		/* dev[TYPE_EATON_SERIAL] = nutscan_scan_eaton_serial (serial_ports); */
		run_eaton_serial(serial_ports);
#endif /* HAVE_PTHREAD */
	} else {
		upsdebugx(1, "SERIAL SCAN: not requested or supported, SKIPPED");
	}

#ifdef HAVE_PTHREAD
	if (allow_usb && nutscan_avail_usb && thread[TYPE_USB]) {
		upsdebugx(1, "USB SCAN: join back the pthread");
		pthread_join(thread[TYPE_USB], NULL);
	}
	if (allow_snmp && nutscan_avail_snmp && thread[TYPE_SNMP]) {
		upsdebugx(1, "SNMP SCAN: join back the pthread");
		pthread_join(thread[TYPE_SNMP], NULL);
	}
	if (allow_xml && nutscan_avail_xml_http && thread[TYPE_XML]) {
		upsdebugx(1, "XML/HTTP SCAN: join back the pthread");
		pthread_join(thread[TYPE_XML], NULL);
	}
	if (allow_oldnut && nutscan_avail_nut && thread[TYPE_NUT]) {
		upsdebugx(1, "NUT bus (old) SCAN: join back the pthread");
		pthread_join(thread[TYPE_NUT], NULL);
	}
	if (allow_nut_simulation && nutscan_avail_nut_simulation && thread[TYPE_NUT_SIMULATION]) {
		upsdebugx(1, "NUT simulation devices SCAN: join back the pthread");
		pthread_join(thread[TYPE_NUT_SIMULATION], NULL);
	}
	if (allow_avahi && nutscan_avail_avahi && thread[TYPE_AVAHI]) {
		upsdebugx(1, "NUT bus (avahi) SCAN: join back the pthread");
		pthread_join(thread[TYPE_AVAHI], NULL);
	}
	if (allow_ipmi && nutscan_avail_ipmi && thread[TYPE_IPMI]) {
		upsdebugx(1, "IPMI SCAN: join back the pthread");
		pthread_join(thread[TYPE_IPMI], NULL);
	}
	if (allow_eaton_serial && thread[TYPE_EATON_SERIAL]) {
		upsdebugx(1, "SERIAL SCAN: join back the pthread");
		pthread_join(thread[TYPE_EATON_SERIAL], NULL);
	}
#endif /* HAVE_PTHREAD */

	upsdebugx(1, "SCANS DONE: display results");

	upsdebugx(1, "SCANS DONE: display results: USB");
	display_func(dev[TYPE_USB]);
	upsdebugx(1, "SCANS DONE: free resources: USB");
	nutscan_free_device(dev[TYPE_USB]);

	upsdebugx(1, "SCANS DONE: display results: SNMP");
	display_func(dev[TYPE_SNMP]);
	upsdebugx(1, "SCANS DONE: free resources: SNMP");
	nutscan_free_device(dev[TYPE_SNMP]);

	upsdebugx(1, "SCANS DONE: display results: XML/HTTP");
	display_func(dev[TYPE_XML]);
	upsdebugx(1, "SCANS DONE: free resources: XML/HTTP");
	nutscan_free_device(dev[TYPE_XML]);

	upsdebugx(1, "SCANS DONE: display results: NUT bus (old)");
	display_func(dev[TYPE_NUT]);
	upsdebugx(1, "SCANS DONE: free resources: NUT bus (old)");
	nutscan_free_device(dev[TYPE_NUT]);

	upsdebugx(1, "SCANS DONE: display results: NUT simulation devices");
	display_func(dev[TYPE_NUT_SIMULATION]);
	upsdebugx(1, "SCANS DONE: free resources: NUT simulation devices");
	nutscan_free_device(dev[TYPE_NUT_SIMULATION]);

	upsdebugx(1, "SCANS DONE: display results: NUT bus (avahi)");
	display_func(dev[TYPE_AVAHI]);
	upsdebugx(1, "SCANS DONE: free resources: NUT bus (avahi)");
	nutscan_free_device(dev[TYPE_AVAHI]);

	upsdebugx(1, "SCANS DONE: display results: IPMI");
	display_func(dev[TYPE_IPMI]);
	upsdebugx(1, "SCANS DONE: free resources: IPMI");
	nutscan_free_device(dev[TYPE_IPMI]);

	upsdebugx(1, "SCANS DONE: display results: SERIAL");
	display_func(dev[TYPE_EATON_SERIAL]);
	upsdebugx(1, "SCANS DONE: free resources: SERIAL");
	nutscan_free_device(dev[TYPE_EATON_SERIAL]);

#ifdef HAVE_PTHREAD
# ifdef HAVE_SEMAPHORE
	sem_destroy(nutscan_semaphore());
# endif
#endif

	upsdebugx(1, "SCANS DONE: free common scanner resources");
	nutscan_free();
	free_ip_ranges();

	upsdebugx(1, "SCANS DONE: EXIT_SUCCESS");
	return EXIT_SUCCESS;
}
