/*
 * dhcpcd - DHCP client daemon
 * Copyright (c) 2006-2013 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef DHCP_H
#define DHCP_H

#include <arpa/inet.h>
#include <netinet/in.h>

#include <limits.h>
#include <stdint.h>

#include "dhcp-common.h"

/* UDP port numbers for DHCP */
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

#define MAGIC_COOKIE        0x63825363
#define BROADCAST_FLAG      0x8000

/* DHCP message OP code */
#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2

/* DHCP message type */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7
#define DHCP_INFORM         8

/* Constants taken from RFC 2131. */
#define T1			0.5
#define T2			0.875
#define DHCP_BASE		4
#define DHCP_MAX		64
#define DHCP_RAND_MIN		-1
#define DHCP_RAND_MAX		1
#define DHCP_ARP_FAIL		2

/* number of usecs in a second. */
#define USECS_SECOND		1000000
/* As we use timevals, we should use the usec part for
 * greater randomisation. */
#define DHCP_RAND_MIN_U		DHCP_RAND_MIN * USECS_SECOND
#define DHCP_RAND_MAX_U		DHCP_RAND_MAX * USECS_SECOND
#define PROBE_MIN_U		PROBE_MIN * USECS_SECOND
#define PROBE_MAX_U		PROBE_MAX * USECS_SECOND

/* DHCP options */
enum DHO {
	DHO_PAD                    = 0,
	DHO_SUBNETMASK             = 1,
	DHO_ROUTER                 = 3,
	DHO_DNSSERVER              = 6,
	DHO_HOSTNAME               = 12,
	DHO_DNSDOMAIN              = 15,
	DHO_MTU                    = 26,
	DHO_BROADCAST              = 28,
	DHO_STATICROUTE            = 33,
	DHO_NISDOMAIN              = 40,
	DHO_NISSERVER              = 41,
	DHO_NTPSERVER              = 42,
	DHO_VENDOR                 = 43,
	DHO_IPADDRESS              = 50,
	DHO_LEASETIME              = 51,
	DHO_OPTIONSOVERLOADED      = 52,
	DHO_MESSAGETYPE            = 53,
	DHO_SERVERID               = 54,
	DHO_PARAMETERREQUESTLIST   = 55,
	DHO_MESSAGE                = 56,
	DHO_MAXMESSAGESIZE         = 57,
	DHO_RENEWALTIME            = 58,
	DHO_REBINDTIME             = 59,
	DHO_VENDORCLASSID          = 60,
	DHO_CLIENTID               = 61,
	DHO_USERCLASS              = 77,  /* RFC 3004 */
	DHO_RAPIDCOMMIT            = 80,  /* RFC 4039 */
	DHO_FQDN                   = 81,
	DHO_DNSSEARCH              = 119, /* RFC 3397 */
	DHO_CSR                    = 121, /* RFC 3442 */
	DHO_SIXRD                  = 212, /* RFC 5969 */
	DHO_MSCSR                  = 249, /* MS code for RFC 3442 */
	DHO_END                    = 255
};

/* FQDN values - lsnybble used in flags
 * hsnybble to create order
 * and to allow 0x00 to mean disable
 */
enum FQDN {
	FQDN_DISABLE    = 0x00,
	FQDN_NONE       = 0x18,
	FQDN_PTR        = 0x20,
	FQDN_BOTH       = 0x31
};

/* Sizes for DHCP options */
#define DHCP_CHADDR_LEN         16
#define SERVERNAME_LEN          64
#define BOOTFILE_LEN            128
#define DHCP_UDP_LEN            (14 + 20 + 8)
#define DHCP_FIXED_LEN          (DHCP_UDP_LEN + 226)
#define DHCP_OPTION_LEN         (MTU_MAX - DHCP_FIXED_LEN)

/* Some crappy DHCP servers require the BOOTP minimum length */
#define BOOTP_MESSAGE_LENTH_MIN 300

/* Don't import common.h as that defines __unused which causes problems
 * on some Linux systems which define it as part of a structure */
#if __GNUC__ > 2 || defined(__INTEL_COMPILER)
# ifndef __packed
#  define __packed   __attribute__((__packed__))
# endif
#else
# ifndef __packed
#  define __packed
# endif
#endif

struct dhcp_message {
	uint8_t op;           /* message type */
	uint8_t hwtype;       /* hardware address type */
	uint8_t hwlen;        /* hardware address length */
	uint8_t hwopcount;    /* should be zero in client message */
	uint32_t xid;            /* transaction id */
	uint16_t secs;           /* elapsed time in sec. from boot */
	uint16_t flags;
	uint32_t ciaddr;         /* (previously allocated) client IP */
	uint32_t yiaddr;         /* 'your' client IP address */
	uint32_t siaddr;         /* should be zero in client's messages */
	uint32_t giaddr;         /* should be zero in client's messages */
	uint8_t chaddr[DHCP_CHADDR_LEN];  /* client's hardware address */
	uint8_t servername[SERVERNAME_LEN];    /* server host name */
	uint8_t bootfile[BOOTFILE_LEN];    /* boot file name */
	uint32_t cookie;
	uint8_t options[DHCP_OPTION_LEN]; /* message options - cookie */
} __packed;

struct dhcp_lease {
	struct in_addr addr;
	struct in_addr net;
	struct in_addr brd;
	uint32_t leasetime;
	uint32_t renewaltime;
	uint32_t rebindtime;
	struct in_addr server;
	time_t leasedfrom;
	struct timeval boundtime;
	uint8_t frominfo;
	uint32_t cookie;
};

enum DHS {
	DHS_INIT,
	DHS_DISCOVER,
	DHS_REQUEST,
	DHS_BOUND,
	DHS_RENEW,
	DHS_REBIND,
	DHS_REBOOT,
	DHS_INFORM,
	DHS_RENEW_REQUESTED,
	DHS_INIT_IPV4LL,
	DHS_PROBE
};

struct dhcp_state {
	enum DHS state;
	struct dhcp_message *sent;
	struct dhcp_message *offer;
	struct dhcp_message *new;
	struct dhcp_message *old;
	struct dhcp_lease lease;
	const char *reason;
	time_t interval;
	time_t nakoff;
	uint32_t xid;
	int socket;
	int probes;
	int claims;
	int conflicts;
	time_t defend;
	struct in_addr fail;
	size_t arping_index;

	int raw_fd;
	int udp_fd;
	int arp_fd;
	size_t buffer_size, buffer_len, buffer_pos;
	unsigned char *buffer;

	struct in_addr addr;
	struct in_addr net;
	struct in_addr dst;

	char leasefile[PATH_MAX];
	time_t start_uptime;

	unsigned char *clientid;
};

#define D_STATE(ifp)							       \
	((struct dhcp_state *)(ifp)->if_data[IF_DATA_DHCP])
#define D_CSTATE(ifp)							       \
	((const struct dhcp_state *)(ifp)->if_data[IF_DATA_DHCP])

#include "dhcpcd.h"
#include "if-options.h"
#include "net.h"

#ifdef INET
extern struct dhcp_opt *dhcp_opts;
extern size_t dhcp_opts_len;

char *decode_rfc3361(int dl, const uint8_t *data);
ssize_t decode_rfc3442(char *out, ssize_t len, int pl, const uint8_t *p);
ssize_t decode_rfc5969(char *out, ssize_t len, int pl, const uint8_t *p);

void dhcp_printoptions(void);
char *get_option_string(const struct dhcp_message *, uint8_t);
int get_option_addr(struct in_addr *, const struct dhcp_message *, uint8_t);
#define is_bootp(m) (m &&						\
	    !IN_LINKLOCAL(htonl((m)->yiaddr)) &&			\
	    get_option_uint8(NULL, m, DHO_MESSAGETYPE) == -1)
struct rt_head *get_option_routes(struct interface *,
    const struct dhcp_message *);
ssize_t dhcp_env(char **, const char *, const struct dhcp_message *,
    const struct interface *);

uint32_t dhcp_xid(const struct interface *);
struct dhcp_message *dhcp_message_new(const struct in_addr *addr,
    const struct in_addr *mask);
int dhcp_message_add_addr(struct dhcp_message *, uint8_t, struct in_addr);
ssize_t make_message(struct dhcp_message **, const struct interface *,
    uint8_t);
int valid_dhcp_packet(unsigned char *);

ssize_t write_lease(const struct interface *, const struct dhcp_message *);
struct dhcp_message *read_lease(const struct interface *);
void get_lease(struct dhcp_lease *, const struct dhcp_message *);

void dhcp_handleifa(int, struct interface *,
    const struct in_addr *, const struct in_addr *, const struct in_addr *);

void dhcp_drop(struct interface *, const char *);
void dhcp_start(struct interface *);
void dhcp_stop(struct interface *);
void dhcp_decline(struct interface *);
void dhcp_discover(void *);
void dhcp_inform(struct interface *);
void dhcp_bind(void *);
void dhcp_reboot_newopts(struct interface *, int);
void dhcp_close(struct interface *);
void dhcp_free(struct interface *);
int dhcp_dump(const char *);
#else
#define dhcp_printoptions
#define dhcp_drop(a, b)
#define dhcp_start(a) {}
#define dhcp_reboot(a, b) b = b
#define dhcp_reboot_newopts(a, b)
#define dhcp_close(a)
#define dhcp_free(a)
#define dhcp_dump(a) -1
#endif

#endif
