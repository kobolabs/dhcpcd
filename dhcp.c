/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <netinet/in.h>
#define __FAVOR_BSD /* Nasty hack so we can use BSD semantics for UDP */
#include <netinet/udp.h>
#undef __FAVOR_BSD
#include <net/if_arp.h>
#include <arpa/inet.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "common.h"
#include "dhcpcd.h"
#include "dhcp.h"
#include "logger.h"
#include "socket.h"

#ifndef STAILQ_CONCAT
#define	STAILQ_CONCAT(head1, head2) do {				\
	if (!STAILQ_EMPTY((head2))) {					\
		*(head1)->stqh_last = (head2)->stqh_first;		\
		(head1)->stqh_last = (head2)->stqh_last;		\
		STAILQ_INIT((head2));					\
	}								\
} while (0)
#endif

struct message {
	int value;
	const char *name;
};

static struct message dhcp_messages[] = {
	{ DHCP_DISCOVER, "DHCP_DISCOVER" },
	{ DHCP_OFFER,    "DHCP_OFFER" },
	{ DHCP_REQUEST,  "DHCP_REQUEST" },
	{ DHCP_DECLINE,  "DHCP_DECLINE" },
	{ DHCP_ACK,      "DHCP_ACK" },
	{ DHCP_NAK,      "DHCP_NAK" },
	{ DHCP_RELEASE,  "DHCP_RELEASE" },
	{ DHCP_INFORM,   "DHCP_INFORM" },
	{ -1, NULL }
};

static const char *
dhcp_message(int type)
{
	struct message *d;

	for (d = dhcp_messages; d->name; d++)
		if (d->value == type)
			return d->name;

	return NULL;
}

static uint16_t
checksum(unsigned char *addr, uint16_t len)
{
	uint32_t sum = 0;
	union
	{
		unsigned char *addr;
		uint16_t *i;
	} p;
	uint16_t nleft = len;
	uint8_t a = 0;

	p.addr = addr;
	while (nleft > 1) {
		sum += *p.i++;
		nleft -= 2;
	}

	if (nleft == 1) {
		memcpy(&a, p.i, 1);
		sum += ntohs(a) << 8;
	}

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return ~sum;
}

static void
make_dhcp_packet(struct udp_dhcp_packet *packet,
		 const unsigned char *data, size_t length,
		 struct in_addr source, struct in_addr dest)
{
	struct ip *ip = &packet->ip;
	struct udphdr *udp = &packet->udp;

	/* OK, this is important :)
	 * We copy the data to our packet and then create a small part of the
	 * ip structure and an invalid ip_len (basically udp length).
	 * We then fill the udp structure and put the checksum
	 * of the whole packet into the udp checksum.
	 * Finally we complete the ip structure and ip checksum.
	 * If we don't do the ordering like so then the udp checksum will be
	 * broken, so find another way of doing it! */

	memcpy(&packet->dhcp, data, length);

	ip->ip_p = IPPROTO_UDP;
	ip->ip_src.s_addr = source.s_addr;
	if (dest.s_addr == 0)
		ip->ip_dst.s_addr = INADDR_BROADCAST;
	else
		ip->ip_dst.s_addr = dest.s_addr;

	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_ulen = htons(sizeof(*udp) + length);
	ip->ip_len = udp->uh_ulen;
	udp->uh_sum = checksum((unsigned char *)packet, sizeof(*packet));

	ip->ip_v = IPVERSION;
	ip->ip_hl = 5;
	ip->ip_id = 0;
	ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = htons (sizeof(*ip) + sizeof(*udp) + length);
	ip->ip_id = 0;
	ip->ip_off = htons(IP_DF); /* Don't fragment */
	ip->ip_ttl = IPDEFTTL;

	ip->ip_sum = checksum((unsigned char *)ip, sizeof(*ip));
}

int
valid_dhcp_packet(unsigned char *data)
{
	union
	{
		unsigned char *data;
		struct udp_dhcp_packet *packet;
	} d;
	uint16_t bytes;
	uint16_t ipsum;
	uint16_t iplen;
	uint16_t udpsum;
	struct in_addr source;
	struct in_addr dest;
	int retval = 0;

	d.data = data;
	bytes = ntohs(d.packet->ip.ip_len);
	ipsum = d.packet->ip.ip_sum;
	iplen = d.packet->ip.ip_len;
	udpsum = d.packet->udp.uh_sum;

	d.data = data;
	d.packet->ip.ip_sum = 0;
	if (ipsum != checksum((unsigned char *)&d.packet->ip,
			      sizeof(d.packet->ip)))
	{
		logger(LOG_DEBUG, "bad IP header checksum, ignoring");
		retval = -1;
		goto eexit;
	}

	memcpy(&source, &d.packet->ip.ip_src, sizeof(d.packet->ip.ip_src));
	memcpy(&dest, &d.packet->ip.ip_dst, sizeof(d.packet->ip.ip_dst));
	memset(&d.packet->ip, 0, sizeof(d.packet->ip));
	d.packet->udp.uh_sum = 0;

	d.packet->ip.ip_p = IPPROTO_UDP;
	memcpy(&d.packet->ip.ip_src, &source, sizeof(d.packet->ip.ip_src));
	memcpy(&d.packet->ip.ip_dst, &dest, sizeof(d.packet->ip.ip_dst));
	d.packet->ip.ip_len = d.packet->udp.uh_ulen;
	if (udpsum && udpsum != checksum(d.data, bytes)) {
		logger(LOG_ERR, "bad UDP checksum, ignoring");
		retval = -1;
	}

eexit:
	d.packet->ip.ip_sum = ipsum;
	d.packet->ip.ip_len = iplen;
	d.packet->udp.uh_sum = udpsum;

	return retval;
}

ssize_t
send_message(const struct interface *iface, const struct dhcp *dhcp,
	     uint32_t xid, char type, const struct options *options)
{
	struct udp_dhcp_packet *packet;
	struct dhcp_message *message;
	unsigned char *m;
	unsigned char *p;
	unsigned char *n_params = NULL;
	size_t l;
	struct in_addr from;
	struct in_addr to;
	time_t up = uptime() - iface->start_uptime;
	uint32_t ul;
	uint16_t sz;
	size_t message_length;
	ssize_t retval;

	memset (&from, 0, sizeof(from));
	memset (&to, 0, sizeof(to));

	if (type == DHCP_RELEASE)
		to.s_addr = dhcp->serveraddress.s_addr;

	message = xzalloc(sizeof (*message));
	m = (unsigned char *)message;
	p = (unsigned char *)&message->options;

	if ((type == DHCP_INFORM ||
	     type == DHCP_RELEASE ||
	     type == DHCP_REQUEST) &&
	    !IN_LINKLOCAL(ntohl(iface->previous_address.s_addr)))
	{
		message->ciaddr = iface->previous_address.s_addr;
		from.s_addr = iface->previous_address.s_addr;

		/* Just incase we haven't actually configured the address yet */
		if (type == DHCP_INFORM && iface->previous_address.s_addr == 0)
			message->ciaddr = dhcp->address.s_addr;

		/* Zero the address if we're currently on a different subnet */
		if (type == DHCP_REQUEST &&
		    iface->previous_netmask.s_addr != dhcp->netmask.s_addr)
			message->ciaddr = from.s_addr = 0;

		if (from.s_addr != 0)
			to.s_addr = dhcp->serveraddress.s_addr;
	}

	message->op = DHCP_BOOTREQUEST;
	message->hwtype = iface->family;
	switch (iface->family) {
	case ARPHRD_ETHER:
	case ARPHRD_IEEE802:
		message->hwlen = ETHER_ADDR_LEN;
		memcpy(&message->chaddr, &iface->hwaddr,
		       ETHER_ADDR_LEN);
		break;
	case ARPHRD_IEEE1394:
	case ARPHRD_INFINIBAND:
		message->hwlen = 0;
		if (message->ciaddr == 0)
			message->flags = htons(BROADCAST_FLAG);
		break;
	default:
		logger (LOG_ERR, "dhcp: unknown hardware type %d",
			iface->family);
	}

	if (up < 0 || up > (time_t)UINT16_MAX)
		message->secs = htons((uint16_t)UINT16_MAX);
	else
		message->secs = htons(up);
	message->xid = xid;
	message->cookie = htonl(MAGIC_COOKIE);

	*p++ = DHCP_MESSAGETYPE; 
	*p++ = 1;
	*p++ = type;

	if (type == DHCP_REQUEST) {
		*p++ = DHCP_MAXMESSAGESIZE;
		*p++ = 2;
		sz = get_mtu(iface->name);
		if (sz < MTU_MIN) {
			if (set_mtu(iface->name, MTU_MIN) == 0)
				sz = MTU_MIN;
		}
		sz = htons(sz);
		memcpy(p, &sz, 2);
		p += 2;
	}

	*p++ = DHCP_CLIENTID;
	*p++ = iface->clientid_len;
	memcpy(p, iface->clientid, iface->clientid_len);
	p+= iface->clientid_len;

	if (type != DHCP_DECLINE && type != DHCP_RELEASE) {
		if (options->userclass_len > 0) {
			*p++ = DHCP_USERCLASS;
			*p++ = options->userclass_len;
			memcpy(p, &options->userclass, options->userclass_len);
			p += options->userclass_len;
		}

		if (*options->classid > 0) {
			*p++ = DHCP_CLASSID;
			*p++ = l = strlen(options->classid);
			memcpy(p, options->classid, l);
			p += l;
		}
	}

	if (type == DHCP_DISCOVER || type == DHCP_REQUEST) {
#define PUTADDR(_type, _val) { \
	*p++ = _type; \
	*p++ = 4; \
	memcpy(p, &_val.s_addr, 4); \
	p += 4; \
}
		if (IN_LINKLOCAL(ntohl (dhcp->address.s_addr)))
			logger(LOG_ERR, "cannot request a link local address");
		else {
			if (dhcp->address.s_addr &&
			    dhcp->address.s_addr !=
			    iface->previous_address.s_addr)
			{
				PUTADDR(DHCP_ADDRESS, dhcp->address);
				if (dhcp->serveraddress.s_addr)
					PUTADDR(DHCP_SERVERIDENTIFIER,
						dhcp->serveraddress);
			}
		}
#undef PUTADDR

		if (options->leasetime != 0) {
			*p++ = DHCP_LEASETIME;
			*p++ = 4;
			ul = htonl(options->leasetime);
			memcpy(p, &ul, 4);
			p += 4;
		}
	}

	if (type == DHCP_DISCOVER ||
	    type == DHCP_INFORM ||
	    type == DHCP_REQUEST)
	{
		if (options->hostname[0]) {
			if (options->fqdn == FQDN_DISABLE) {
				*p++ = DHCP_HOSTNAME;
				*p++ = l = strlen(options->hostname);
				memcpy(p, options->hostname, l);
				p += l;
			} else {
				/* Draft IETF DHC-FQDN option (81) */
				*p++ = DHCP_FQDN;
				*p++ = (l = strlen(options->hostname)) + 3;
				/* Flags: 0000NEOS
				 * S: 1 => Client requests Server to update
				 *         a RR in DNS as well as PTR
				 * O: 1 => Server indicates to client that
				 *         DNS has been updated
				 * E: 1 => Name data is DNS format
				 * N: 1 => Client requests Server to not
				 *         update DNS
				 */
				*p++ = options->fqdn & 0x9;
				*p++ = 0; /* from server for PTR RR */
				*p++ = 0; /* from server for A RR if S=1 */
				memcpy(p, options->hostname, l);
				p += l;
			}
		}

		*p++ = DHCP_PARAMETERREQUESTLIST;
		n_params = p;
		*p++ = 0;
		if (type != DHCP_INFORM) {
			*p++ = DHCP_RENEWALTIME;
			*p++ = DHCP_REBINDTIME;
		}
		*p++ = DHCP_NETMASK;
		*p++ = DHCP_BROADCAST;

		/* -S means request CSR and MSCSR
		 * -SS means only request MSCSR incase DHCP message
		 *  is too big */
		if (options->domscsr < 2)
			*p++ = DHCP_CSR;
		if (options->domscsr > 0)
			*p++ = DHCP_MSCSR;
		/* RFC 3442 states classless static routes should be
		 * before routers and static routes as classless static
		 * routes override them both */
		*p++ = DHCP_STATICROUTE;
		*p++ = DHCP_ROUTERS;
		*p++ = DHCP_HOSTNAME;
		*p++ = DHCP_DNSSEARCH;
		*p++ = DHCP_DNSDOMAIN;
		*p++ = DHCP_DNSSERVER;
#ifdef ENABLE_NIS
		*p++ = DHCP_NISDOMAIN;
		*p++ = DHCP_NISSERVER;
#endif
#ifdef ENABLE_NTP
		*p++ = DHCP_NTPSERVER;
#endif
		*p++ = DHCP_MTU;
#ifdef ENABLE_INFO
		*p++ = DHCP_ROOTPATH;
		*p++ = DHCP_SIPSERVER;
#endif
		*n_params = p - n_params - 1;
	}
	*p++ = DHCP_END;

#ifdef BOOTP_MESSAGE_LENTH_MIN
	/* Some crappy DHCP servers think they have to obey the BOOTP minimum
	 * message length.
	 * They are wrong, but we should still cater for them. */
	while (p - m < BOOTP_MESSAGE_LENTH_MIN)
		*p++ = DHCP_PAD;
#endif

	message_length = p - m;

	packet = xzalloc(sizeof(*packet));
	make_dhcp_packet(packet, (unsigned char *)message, message_length,
			 from, to);
	free(message);

	logger(LOG_DEBUG, "sending %s with xid 0x%x",dhcp_message(type), xid);
	retval = send_packet(iface, ETHERTYPE_IP, (unsigned char *)packet,
			     message_length +
			     sizeof(packet->ip) + sizeof(packet->udp));
	free(packet);
	return retval;
}

/* Decode an RFC3397 DNS search order option into a space
 * seperated string. Returns length of string (including 
 * terminating zero) or zero on error. out may be NULL
 * to just determine output length. */
static unsigned int
decode_search(const unsigned char *p, int len, char *out)
{
	const unsigned char *r, *q = p;
	unsigned int count = 0, l, hops;
	unsigned int ltype;

	while (q - p < len) {
		r = NULL;
		hops = 0;
		while ((l = *q++)) {
			ltype = l & 0xc0;
			if (ltype == 0x80 || ltype == 0x40)
				return 0;
			else if (ltype == 0xc0) { /* pointer */
				l = (l & 0x3f) << 8;
				l |= *q++;
				/* save source of first jump. */
				if (!r)
					r = q;
				hops++;
				if (hops > 255)
					return 0;
				q = p + l;
				if (q - p >= len)
					return 0;
			} else {
				/* straightforward name segment, add with '.' */
				count += l + 1;
				if (out) {
					memcpy(out, q, l);
					out += l;
					*out++ = '.';
				}
				q += l;
			}
		}
		/* change last dot to space */
		if (out)
			*(out - 1) = ' ';
		if (r)
			q = r;
	}

	/* change last space to zero terminator */
	if (out)
		*(out - 1) = 0;

	return count;  
}

/* Add our classless static routes to the routes variable
 * and return the last route set */
static struct route_head *
decode_CSR(const unsigned char *p, int len)
{
	const unsigned char *q = p;
	unsigned int cidr;
	unsigned int ocets;
	struct route_head *routes = NULL;
	struct rt *route;

	/* Minimum is 5 -first is CIDR and a router length of 4 */
	if (len < 5)
		return NULL;

	while (q - p < len) {
		if (! routes) {
			routes = xmalloc(sizeof (*routes));
			STAILQ_INIT(routes);
		}

		route = xzalloc(sizeof(*route));

		cidr = *q++;
		if (cidr > 32) {
			logger(LOG_ERR,
			       "invalid CIDR of %d in classless static route",
			       cidr);
			free_route(routes);
			return NULL;
		}
		ocets = (cidr + 7) / 8;

		if (ocets > 0) {
			memcpy(&route->destination.s_addr, q, (size_t)ocets);
			q += ocets;
		}

		/* Now enter the netmask */
		if (ocets > 0) {
			memset(&route->netmask.s_addr, 255, (size_t)ocets - 1);
			memset((unsigned char *)&route->netmask.s_addr +
			       (ocets - 1),
			       (256 - (1 << (32 - cidr) % 8)), 1);
		}

		/* Finally, snag the router */
		memcpy(&route->gateway.s_addr, q, 4);
		q += 4;

		STAILQ_INSERT_TAIL(routes, route, entries);
	}

	return routes;
}

void
free_address(struct address_head *addresses)
{
	struct address *p;
	struct address *n;

	if (!addresses)
		return;
	p = STAILQ_FIRST(addresses);
	while (p) {
		n = STAILQ_NEXT(p, entries); 
		free(p);
		p = n;
	}
	free(addresses);
}

void
free_dhcp(struct dhcp *dhcp)
{
	if (! dhcp)
		return;

	free_route(dhcp->routes);
	free(dhcp->hostname);
	free_address(dhcp->dnsservers);
	free(dhcp->dnsdomain);
	free(dhcp->dnssearch);
	free_address(dhcp->ntpservers);
	free(dhcp->nisdomain);
	free_address(dhcp->nisservers);
	free(dhcp->rootpath);
	free(dhcp->sipservers);
	if (dhcp->fqdn) {
		free(dhcp->fqdn->name);
		free(dhcp->fqdn);
	}
}

void
free_route(struct route_head *routes)
{
	struct rt *p;
	struct rt *n;

	if (!routes)
		return;
	p = STAILQ_FIRST(routes);
	while (p) {
		n = STAILQ_NEXT(p, entries);
		free(p);
		p = n;
	}
	free(routes);
}

#ifdef ENABLE_INFO
static char *
decode_sipservers(const unsigned char *data, int length)
{
	char *sip = NULL;
	char *p;
	const char encoding = *data++;
	struct in_addr addr;
	size_t len;

	length--;
	switch (encoding) {
	case 0:
		if ((len = decode_search(data, length, NULL)) > 0) {
			sip = xmalloc(len);
			decode_search(data, length, sip);
		}
		break;

	case 1:
		if (length == 0 || length % 4 != 0) {
			logger (LOG_ERR,
				"invalid length %d for option 120",
				length + 1);
			break;
		}
		len = ((length / 4) * (4 * 4)) + 1;
		sip = p = xmalloc(len);
		while (length != 0) {
			memcpy(&addr.s_addr, data, 4);
			data += 4;
			p += snprintf (p, len - (p - sip),
				       "%s ", inet_ntoa (addr));
			length -= 4;
		}
		*--p = '\0';
		break;

	default:
		logger (LOG_ERR, "unknown sip encoding %d", encoding);
		break;
	}

	return sip;
}
#endif

/* This calculates the netmask that we should use for static routes.
 * This IS different from the calculation used to calculate the netmask
 * for an interface address. */
static uint32_t
route_netmask(uint32_t ip_in)
{
	/* used to be unsigned long - check if error */
	uint32_t p = ntohl(ip_in);
	uint32_t t;

	if (IN_CLASSA(p))
		t = ~IN_CLASSA_NET;
	else {
		if (IN_CLASSB(p))
			t = ~IN_CLASSB_NET;
		else {
			if (IN_CLASSC(p))
				t = ~IN_CLASSC_NET;
			else
				t = 0;
		}
	}

	while (t & p)
		t >>= 1;

	return (htonl(~t));
}

static struct route_head *
decode_routes(const unsigned char *data, int length)
{
	int i;
	struct route_head *head = NULL;
	struct rt *route;
	
	for (i = 0; i < length; i += 8) {
		if (! head) {
			head = xmalloc(sizeof(*head));
			STAILQ_INIT(head);
		}
		route = xzalloc(sizeof(*route));
		memcpy(&route->destination.s_addr, data + i, 4);
		memcpy(&route->gateway.s_addr, data + i + 4, 4);
		route->netmask.s_addr =
			route_netmask(route->destination.s_addr);
		STAILQ_INSERT_TAIL(head, route, entries);
	}

	return head;
}

static struct route_head *
decode_routers(const unsigned char *data, int length)
{
	int i;
	struct route_head *head = NULL;
	struct rt *route;

	for (i = 0; i < length; i += 4) {
		if (! head) {
			head = xmalloc(sizeof(*head));
			STAILQ_INIT(head);
		}
		route = xzalloc(sizeof (*route));
		memcpy(&route->gateway.s_addr, data + i, 4);
		STAILQ_INSERT_TAIL(head, route, entries);
	}

	return head;
}

static bool
add_addr(struct address_head **addresses,
	 const unsigned char *data, size_t length, char option)
{
	size_t i;
	struct address *address;

	for (i = 0; i < length; i += 4) {
		/* Sanity check */
		if (i + 4 > length) {
			logger(LOG_ERR, "invalid length %zu for option %i",
			       length, option);
			return false;
		}

		if (*addresses == NULL) {
			*addresses = xmalloc(sizeof(**addresses));
			STAILQ_INIT(*addresses);
		}
		address = xzalloc(sizeof(*address));
		memcpy(&address->address.s_addr, data + i, 4);
		STAILQ_INSERT_TAIL(*addresses, address, entries);
	}

	return true;
}

static bool
get_string(char **ptr, const unsigned char *data, size_t len)
{
	if (*ptr)
		free(*ptr);
	*ptr = xmalloc(len + 1);
	memcpy(*ptr, data, len);
	(*ptr)[len] = '\0';
	return true;
}

static bool
get_value(void *ptr, const unsigned char *data, size_t len,
	  char option, size_t lencheck)
{
	if (lencheck && len != lencheck) {
		logger(LOG_ERR, "invalid length %zu for option %i",
		       len, option);
		return false;
	}

	memcpy(ptr, data, len);
	return true;
}

int
parse_dhcpmessage(struct dhcp *dhcp, const struct dhcp_message *message)
{
	const unsigned char *p = message->options;
	const unsigned char *end = p; /* Add size later for gcc-3 issue */
	unsigned char option;
	unsigned char length;
	unsigned int len = 0;
	int retval = -1;
	struct timeval tv;
	struct route_head *routers = NULL;
	struct route_head *routes = NULL;
	struct route_head *csr = NULL;
	struct route_head *mscsr = NULL;
	bool in_overload = false;
	bool parse_sname = false;
	bool parse_file = false;

	end += sizeof(message->options);

	if (gettimeofday(&tv, NULL) == -1) {
		logger(LOG_ERR, "gettimeofday: %s", strerror(errno));
		return -1;
	}

	dhcp->address.s_addr = message->yiaddr;
	dhcp->leasedfrom = tv.tv_sec;
	dhcp->frominfo = false;
	dhcp->address.s_addr = message->yiaddr;
	strlcpy(dhcp->servername, (char *)message->servername,
		sizeof(dhcp->servername));

/* Handy macros to make the get_* functions easier to use */
#define GET_UINT8(var)    get_value(&(var), p, length, option, sizeof(uint8_t))
#define GET_UINT16(var)   get_value(&(var), p, length, option, sizeof(uint16_t))
#define GET_UINT32(var)   get_value(&(var), p, length, option, sizeof(uint32_t))
#define GET_UINT16_H(var) if (GET_UINT16(var)) var = ntohs(var)
#define GET_UINT32_H(var) if (GET_UINT32(var)) var = ntohl(var)
#define GET_STR(var)      get_string(&(var), p, length)
#define GET_ADDR(var)     add_addr(&var, p, length, option)

#define LEN_ERR \
	{ \
		logger (LOG_ERR, "invalid length %d for option %d", \
			length, option); \
		p += length; \
		continue; \
	}
#define LENGTH(_length)     if (length != _length)   LEN_ERR;
#define MIN_LENGTH(_length) if (length < _length)    LEN_ERR;
#define MULT_LENGTH(_mult)  if (length % _mult != 0) LEN_ERR;

parse_start:
	while (p < end) {
		option = *p++;
		if (!option)
			continue;
		if (option == DHCP_END)
			goto eexit;

		length = *p++;
		if (option != DHCP_PAD && length == 0) {
			logger(LOG_ERR, "option %d has zero length, skipping",
			       option);
			continue;
		}
		if (p + length >= end) {
			logger(LOG_ERR, "dhcp option exceeds message length");
			retval = -1;
			goto eexit;
		}

		switch (option) {
		case DHCP_MESSAGETYPE:
			retval = (int)*p;
			break;
		case DHCP_ADDRESS:
			GET_UINT32(dhcp->address.s_addr);
			break;
		case DHCP_NETMASK:
			GET_UINT32(dhcp->netmask.s_addr);
			break;
		case DHCP_BROADCAST:
			GET_UINT32(dhcp->broadcast.s_addr);
			break;
		case DHCP_SERVERIDENTIFIER:
			GET_UINT32(dhcp->serveraddress.s_addr);
			break;
		case DHCP_LEASETIME:
			GET_UINT32_H(dhcp->leasetime);
			break;
		case DHCP_RENEWALTIME:
			GET_UINT32_H(dhcp->renewaltime);
			break;
		case DHCP_REBINDTIME:
			GET_UINT32_H(dhcp->rebindtime);
			break;
		case DHCP_MTU:
			GET_UINT16_H (dhcp->mtu);
			/* Minimum legal mtu is 68 accoridng to
			 * RFC 2132. In practise it's 576 which is the
			 * minimum maximum message size. */
			if (dhcp->mtu < MTU_MIN) {
				logger(LOG_DEBUG,
				       "MTU %d is too low, minimum is %d; ignoring",
				       dhcp->mtu, MTU_MIN);
				dhcp->mtu = 0;
			}
			break;
		case DHCP_HOSTNAME:
			GET_STR(dhcp->hostname);
			break;
		case DHCP_DNSDOMAIN:
			GET_STR(dhcp->dnsdomain);
			break;
		case DHCP_MESSAGE:
			GET_STR(dhcp->message);
			break;
#ifdef ENABLE_INFO
		case DHCP_ROOTPATH:
			GET_STR(dhcp->rootpath);
			break;
#endif
#ifdef ENABLE_NIS
		case DHCP_NISDOMAIN:
			GET_STR(dhcp->nisdomain);
			break;
#endif
		case DHCP_DNSSERVER:
			GET_ADDR(dhcp->dnsservers);
			break;
#ifdef ENABLE_NTP
		case DHCP_NTPSERVER:
			GET_ADDR(dhcp->ntpservers);
			break;
#endif
#ifdef ENABLE_NIS
		case DHCP_NISSERVER:
			GET_ADDR(dhcp->nisservers);
			break;
#endif

		case DHCP_DNSSEARCH:
			MIN_LENGTH(1);
			free(dhcp->dnssearch);
			len = decode_search(p, length, NULL);
			if (len > 0) {
				dhcp->dnssearch = xmalloc(len);
				decode_search(p, length, dhcp->dnssearch);
			}
			break;
		case DHCP_CSR:
			MIN_LENGTH(5);
			free_route(csr);
			csr = decode_CSR(p, length);
			break;
		case DHCP_MSCSR:
			MIN_LENGTH(5);
			free_route(mscsr);
			mscsr = decode_CSR(p, length);
			break;
#ifdef ENABLE_INFO
		case DHCP_SIPSERVER:
			free(dhcp->sipservers);
			dhcp->sipservers = decode_sipservers(p, length);
			break;
#endif
		case DHCP_STATICROUTE:
			MULT_LENGTH(8);
			free_route(routes);
			routes = decode_routes(p, length);
			break;
		case DHCP_ROUTERS:
			MULT_LENGTH(4);
			free_route(routers);
			routers = decode_routers(p, length);
			break;
		case DHCP_OPTIONSOVERLOADED:
			LENGTH(1);
			/* The overloaded option in an overloaded option
			 * should be ignored, overwise we may get an
			 * infinite loop */
			if (!in_overload) {
				if (*p & 1)
					parse_file = true;
				if (*p & 2)
					parse_sname = true;
			}
			break;
		case DHCP_FQDN:
			/* We ignore replies about FQDN */
			break;
		default:
			logger (LOG_DEBUG,
			       	"no facility to parse DHCP code %u", option);
				break;
		}
		p += length;
	}

eexit:
	/* We may have options overloaded, so go back and grab them */
	if (parse_file) {
		parse_file = false;
		p = message->bootfile;
		end = p + sizeof(message->bootfile);
		in_overload = true;
		goto parse_start;
	} else if (parse_sname) {
		parse_sname = false;
		p = message->servername;
		end = p + sizeof(message->servername);
		memset(dhcp->servername, 0, sizeof(dhcp->servername));
		in_overload = true;
		goto parse_start;
	}

	/* Fill in any missing fields */
	if (!dhcp->netmask.s_addr)
		dhcp->netmask.s_addr = get_netmask(dhcp->address.s_addr);
	if (!dhcp->broadcast.s_addr)
		dhcp->broadcast.s_addr = dhcp->address.s_addr |
			~dhcp->netmask.s_addr;

	/* If we have classess static routes then we discard
	 * static routes and routers according to RFC 3442 */
	if (csr) {
		dhcp->routes = csr;
		free_route(mscsr);
		free_route(routers);
		free_route(routes);
	} else if (mscsr) {
		dhcp->routes = mscsr;
		free_route(routers);
		free_route(routes);
	} else {
		/* Ensure that we apply static routes before routers */
		if (! routes)
			routes = routers;
		else if (routers)
			STAILQ_CONCAT(routes, routers);
		dhcp->routes = routes;
	}

	return retval;
}
