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

/* TODO: We should decline dupliate addresses detected */

#include <sys/stat.h>
#include <sys/utsname.h>

#include <netinet/in.h>
#ifdef __linux__
#  define _LINUX_IN6_H
#  include <linux/ipv6.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define ELOOP_QUEUE 3

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "dhcp6.h"
#include "duid.h"
#include "eloop.h"
#include "ipv6ns.h"
#include "ipv6rs.h"
#include "platform.h"
#include "script.h"

#ifndef __UNCONST
#define __UNCONST(a) ((void *)(unsigned long)(const void *)(a))
#endif

/* DHCPCD Project has been assigned an IANA PEN of 40712 */
#define DHCPCD_IANA_PEN 40712

/* Unsure if I want this */
//#define VENDOR_SPLIT

static int sock = -1;
static struct sockaddr_in6 alldhcp, from;
static struct msghdr sndhdr;
static struct iovec sndiov[2];
static unsigned char *sndbuf;
static struct msghdr rcvhdr;
static struct iovec rcviov[2];
static unsigned char *rcvbuf;
static unsigned char ansbuf[1500];
static unsigned char *duid;
static uint16_t duid_len;
static char ntopbuf[INET6_ADDRSTRLEN];
static char *status;
static size_t status_len;

struct dhcp6_op {
	uint16_t type;
	const char *name;
};

static const struct dhcp6_op dhcp6_ops[] = {
	{ DHCP6_SOLICIT, "SOLICIT6" },
	{ DHCP6_ADVERTISE, "ADVERTISE6" },
	{ DHCP6_REQUEST, "REQUEST6" },
	{ DHCP6_REPLY, "REPLY6" },
	{ DHCP6_RENEW, "RENEW6" },
	{ DHCP6_REBIND, "REBIND6" },
	{ DHCP6_CONFIRM, "CONFIRM6" },
	{ DHCP6_INFORMATION_REQ, "INFORM6" },
	{ DHCP6_RELEASE, "RELEASE6" },
	{ 0, NULL }
};

#define IPV6A	ADDRIPV6 | ARRAY
const struct dhcp_opt dhcp6_opts[] = {
	{ D6_OPTION_CLIENTID,		BINHEX,		"client_id" },
	{ D6_OPTION_SERVERID,		BINHEX,		"server_id" },
	{ D6_OPTION_IA_ADDR,		IPV6A,		"ia_addr" },
	{ D6_OPTION_PREFERENCE,		UINT8,		"preference" },
	{ D6_OPTION_RAPID_COMMIT,	0,		"rapid_commit" },
	{ D6_OPTION_UNICAST,		ADDRIPV6,	"unicast" },
	{ D6_OPTION_STATUS_CODE,	SCODE,		"status_code" },
	{ D6_OPTION_SIP_SERVERS_NAME,	RFC3397,	"sip_servers_names" },
	{ D6_OPTION_SIP_SERVERS_ADDRESS,IPV6A,	"sip_servers_addresses" },
	{ D6_OPTION_DNS_SERVERS,	IPV6A,		"name_servers" },
	{ D6_OPTION_DOMAIN_LIST,	RFC3397,	"domain_search" },
	{ D6_OPTION_NIS_SERVERS,	IPV6A,		"nis_servers" },
	{ D6_OPTION_NISP_SERVERS,	IPV6A,		"nisp_servers" },
	{ D6_OPTION_NIS_DOMAIN_NAME,	RFC3397,	"nis_domain_name" },
	{ D6_OPTION_NISP_DOMAIN_NAME,	RFC3397,	"nisp_domain_name" },
	{ D6_OPTION_SNTP_SERVERS,	IPV6A,		"sntp_servers" },
	{ D6_OPTION_INFO_REFRESH_TIME,	UINT32,		"info_refresh_time" },
	{ D6_OPTION_BCMS_SERVER_D,	RFC3397,	"bcms_server_d" },
	{ D6_OPTION_BCMS_SERVER_A,	IPV6A,		"bcms_server_a" },
	{ D6_OPTION_POSIX_TIMEZONE,	STRING,		"posix_timezone" },
	{ D6_OPTION_TZDB_TIMEZONE,	STRING,		"tzdb_timezone" },
	{ 0, 0, NULL }
};

#if DEBUG_MEMORY
static void
dhcp6_cleanup(void)
{

	free(sndbuf);
	free(rcvbuf);
	free(duid);
	free(status);
}
#endif

void
dhcp6_printoptions(void)
{
	const struct dhcp_opt *opt;

	for (opt = dhcp6_opts; opt->option; opt++)
		if (opt->var)
			printf("%05d %s\n", opt->option, opt->var);
}

static int
dhcp6_init(void)
{
	int len;

#if DEBUG_MEMORY
	atexit(dhcp6_cleanup);
#endif

	memset(&alldhcp, 0, sizeof(alldhcp));
	alldhcp.sin6_family = AF_INET6;
	alldhcp.sin6_port = htons(DHCP6_SERVER_PORT);
#ifdef SIN6_LEN
	alldhcp.sin6_len = sizeof(alldhcp);
#endif
	if (inet_pton(AF_INET6, ALLDHCP, &alldhcp.sin6_addr.s6_addr) != 1)
		return -1;

	len = CMSG_SPACE(sizeof(struct in6_pktinfo));
	sndbuf = calloc(1, len);
	if (sndbuf == NULL)
		return -1;
	sndhdr.msg_namelen = sizeof(struct sockaddr_in6);
	sndhdr.msg_iov = sndiov;
	sndhdr.msg_iovlen = 1;
	sndhdr.msg_control = sndbuf;
	sndhdr.msg_controllen = len;

	rcvbuf = calloc(1, len);
	if (rcvbuf == NULL) {
		free(sndbuf);
		sndbuf = NULL;
		return -1;
	}
	rcvhdr.msg_name = &from;
	rcvhdr.msg_namelen = sizeof(from);
	rcvhdr.msg_iov = rcviov;
	rcvhdr.msg_iovlen = 1;
	rcvhdr.msg_control = rcvbuf;
	rcvhdr.msg_controllen = len;
	rcviov[0].iov_base = ansbuf;
	rcviov[0].iov_len = sizeof(ansbuf);

	return 0;
}

#ifdef DHCPCD_IANA_PEN
static size_t
dhcp6_makevendor(struct dhcp6_option *o)
{
	size_t len;
	uint8_t *p;
	uint16_t u16;
	uint32_t u32;
	size_t vlen;
#ifdef VENDOR_SPLIT
	const char *platform;
	size_t plen, unl, url, uml, pl;
	struct utsname utn;
#endif

	len = sizeof(uint32_t); /* IANA PEN */

#ifdef VENDOR_SPLIT
	plen = strlen(PACKAGE);
	vlen = strlen(VERSION);
	len += sizeof(uint16_t) + plen + 1 + vlen;
	if (uname(&utn) == 0) {
		unl = strlen(utn.sysname);
		url = strlen(utn.release);
		uml = strlen(utn.machine);
		platform = hardware_platform();
		pl = strlen(platform);
		len += sizeof(uint16_t) + unl + 1 + url;
		len += sizeof(uint16_t) + uml;
		len += sizeof(uint16_t) + pl;
	} else
		unl = 0;
#else
	vlen = strlen(vendor);
	len += sizeof(uint16_t) + vlen;
#endif

	if (o) {
		o->code = htons(D6_OPTION_VENDOR);
		o->len = htons(len);
		p = D6_OPTION_DATA(o);
		u32 = htonl(DHCPCD_IANA_PEN);
		memcpy(p, &u32, sizeof(u32));
		p += sizeof(u32);
#ifdef VENDOR_SPLIT
		u16 = htons(plen + 1 + vlen);
		memcpy(p, &u16, sizeof(u16));
		p += sizeof(u16);
		memcpy(p, PACKAGE, plen);
		p += plen;
		*p++ = '-';
		memcpy(p, VERSION, vlen);
		p += vlen;
		if (unl > 0) {
			u16 = htons(unl + 1 + url);
			memcpy(p, &u16, sizeof(u16));
			p += sizeof(u16);
			memcpy(p, utn.sysname, unl);
			p += unl;
			*p++ = '-';
			memcpy(p, utn.release, url);
			p += url;
			u16 = htons(uml);
			memcpy(p, &u16, sizeof(u16));
			p += sizeof(u16);
			memcpy(p, utn.machine, uml);
			p += uml;
			u16 = htons(pl);
			memcpy(p, &u16, sizeof(u16));
			p += sizeof(u16);
			memcpy(p, platform, pl);
		}
#else
		u16 = htons(vlen);
		memcpy(p, &u16, sizeof(u16));
		p += sizeof(u16);
		memcpy(p, vendor, vlen);
#endif
	}

	return len;
}
#endif

static const struct dhcp6_option *
dhcp6_findoption(int code, const uint8_t *d, ssize_t len)
{
	const struct dhcp6_option *o;

	code = htons(code);
	for (o = (const struct dhcp6_option *)d;
	    len > (ssize_t)sizeof(*o);
	    o = D6_CNEXT_OPTION(o))
	{
		len -= sizeof(*o) + ntohs(o->len);
		if (len < 0) {
			errno = EINVAL;
			return NULL;
		}
		if (o->code == code)
			return o;
	}

	errno = ESRCH;
	return NULL;
}

static const struct dhcp6_option *
dhcp6_getoption(int code, const struct dhcp6_message *m, ssize_t len)
{

	len -= sizeof(*m);
	return dhcp6_findoption(code,
	    (const uint8_t *)D6_CFIRST_OPTION(m), len);
}

static int
dhcp6_updateelapsed(struct interface *ifp, struct dhcp6_message *m, ssize_t len)
{
	struct dhcp6_state *state;
	const struct dhcp6_option *co;
	struct dhcp6_option *o;
	time_t up;
	uint16_t u16;

	co = dhcp6_getoption(D6_OPTION_ELAPSED, m, len);
	if (co == NULL)
		return -1;

	o = __UNCONST(co);
	state = D6_STATE(ifp);
	up = uptime() - state->start_uptime;
	if (up < 0 || up > (time_t)UINT16_MAX)
		up = (time_t)UINT16_MAX;
	u16 = htons(up);
	memcpy(D6_OPTION_DATA(o), &u16, sizeof(u16));
	return 0;
}

static void
dhcp6_newxid(const struct interface *ifp, struct dhcp6_message *m)
{
	uint32_t xid;

	if (ifp->options->options & DHCPCD_XID_HWADDR &&
	    ifp->hwlen >= sizeof(xid))
		/* The lower bits are probably more unique on the network */
		memcpy(&xid, (ifp->hwaddr + ifp->hwlen) - sizeof(xid),
		    sizeof(xid));
	else
		xid = arc4random();

	m->xid[0] = (xid >> 16) & 0xff;
	m->xid[1] = (xid >> 8) & 0xff;
	m->xid[2] = xid & 0xff;
}

static int
dhcp6_makemessage(struct interface *ifp)
{
	struct dhcp6_state *state;
	struct dhcp6_message *m;
	struct dhcp6_option *o, *so;
	const struct dhcp6_option *si;
	ssize_t len, ml;
	size_t l;
	uint8_t u8;
	uint16_t *u16;
	const struct if_options *ifo;
	const struct dhcp_opt *opt;
	uint8_t IA, *p;
	uint32_t u32;
	const struct ipv6_addr *ap;

	state = D6_STATE(ifp);
	if (state->send) {
		free(state->send);
		state->send = NULL;
	}

	/* Work out option size first */
	ifo = ifp->options;
	len = 0;
	si = NULL;
	if (state->state != DH6S_RELEASE) {
		for (opt = dhcp6_opts; opt->option; opt++) {
			if (opt->type & REQUEST ||
			    has_option_mask(ifo->requestmask6, opt->option))
				len += sizeof(*u16);
		}
		if (len == 0)
			len = sizeof(*u16) * 2;
		len += sizeof(*o);
	}

	len += sizeof(*state->send);
	len += sizeof(*o) + duid_len;
	len += sizeof(*o) + sizeof(uint16_t); /* elapsed */
#ifdef DHCPCD_IANA_PEN
	len += sizeof(*o) + dhcp6_makevendor(NULL);
#endif

	/* IA */
	m = NULL;
	ml = 0;
	switch(state->state) {
	case DH6S_REQUEST:
		m = state->recv;
		ml = state->recv_len;
		/* FALLTHROUGH */
	case DH6S_RELEASE:
		/* FALLTHROUGH */
	case DH6S_RENEW:
		if (m == NULL) {
			m = state->new;
			ml = state->new_len;
		}
		si = dhcp6_getoption(D6_OPTION_SERVERID, m, ml);
		len += sizeof(*si) + ntohs(si->len);
		/* FALLTHROUGH */
	case DH6S_REBIND:
		/* FALLTHROUGH */
	case DH6S_CONFIRM:
		if (m == NULL) {
			m = state->new;
			ml = state->new_len;
		}
		TAILQ_FOREACH(ap, &state->addrs, next) {
			if (ifo->ia_type == D6_OPTION_IA_PD)
				len += sizeof(*o) + sizeof(u8) +
				    sizeof(u32) + sizeof(u32) +
				    sizeof(ap->prefix.s6_addr);
			else
				len += sizeof(*o) + sizeof(ap->addr.s6_addr) +
				    sizeof(u32) + sizeof(u32);
		}
		/* FALLTHROUGH */
	case DH6S_INIT: /* FALLTHROUGH */
	case DH6S_DISCOVER:
		len += ifo->iaid_len * (sizeof(*o) + (sizeof(u32) * 3));
		IA = 1;
		break;
	default:
		IA = 0;
	}

	if (m == NULL) {
		m = state->new;
		ml = state->new_len;
	}

	state->send = malloc(len);
	if (state->send == NULL)
		return -1;

	state->send_len = len;
	switch(state->state) {
		break;
	case DH6S_INIT: /* FALLTHROUGH */
	case DH6S_DISCOVER:
		state->send->type = DHCP6_SOLICIT;
		break;
	case DH6S_REQUEST:
		state->send->type = DHCP6_REQUEST;
		break;
	case DH6S_CONFIRM:
		state->send->type = DHCP6_CONFIRM;
		break;
	case DH6S_REBIND:
		state->send->type = DHCP6_REBIND;
		break;
	case DH6S_RENEW:
		state->send->type = DHCP6_RENEW;
		break;
	case DH6S_INFORM:
		state->send->type = DHCP6_INFORMATION_REQ;
		break;
	case DH6S_RELEASE:
		state->send->type = DHCP6_RELEASE;
		break;
	default:
		errno = EINVAL;
		free(state->send);
		state->send = NULL;
		return -1;
	}

	dhcp6_newxid(ifp, state->send);

	o = D6_FIRST_OPTION(state->send);
	o->code = htons(D6_OPTION_CLIENTID);
	o->len = htons(duid_len);
	memcpy(D6_OPTION_DATA(o), duid, duid_len);

	if (si) {
		o = D6_NEXT_OPTION(o);
		memcpy(o, si, sizeof(*si) + ntohs(si->len));
	}

	o = D6_NEXT_OPTION(o);
	o->code = htons(D6_OPTION_ELAPSED);
	o->len = htons(sizeof(uint16_t));
	p = D6_OPTION_DATA(o);
	memset(p, 0, sizeof(u16));

#ifdef DHCPCD_IANA_PEN
	o = D6_NEXT_OPTION(o);
	dhcp6_makevendor(o);
#endif

	for (l = 0; IA && l < ifo->iaid_len; l++) {
		o = D6_NEXT_OPTION(o);
		o->code = htons(ifo->ia_type);
		o->len = htons(sizeof(u32) + sizeof(u32) + sizeof(u32));
		p = D6_OPTION_DATA(o);
		memcpy(p, ifo->iaid[l].iaid, sizeof(u32));
		p += sizeof(u32);
		memset(p, 0, sizeof(u32) + sizeof(u32));
		TAILQ_FOREACH(ap, &state->addrs, next) {
			if (memcmp(ifo->iaid[l].iaid, ap->iaid, sizeof(u32)))
				continue;
			so = D6_NEXT_OPTION(o);
			if (ifo->ia_type == D6_OPTION_IA_PD) {
				so->code = htons(D6_OPTION_IAPREFIX);
				so->len = htons(sizeof(ap->prefix.s6_addr) +
				    sizeof(u32) + sizeof(u32) + sizeof(u8));
				p = D6_OPTION_DATA(so);
				u32 = htonl(ap->prefix_pltime);
				memcpy(p, &u32, sizeof(u32));
				p += sizeof(u32);
				u32 = htonl(ap->prefix_vltime);
				memcpy(p, &u32, sizeof(u32));
				p += sizeof(u32);
				u8 = ap->prefix_len;
				memcpy(p, &u8, sizeof(u8));
				p += sizeof(u8);
				memcpy(p, &ap->prefix.s6_addr,
				    sizeof(ap->prefix.s6_addr));
				/* Avoid a shadowed declaration warning by
				 * moving our addition outside of the htons
				 * macro */
				u32 = ntohs(o->len) + sizeof(*so)
				    + ntohs(so->len);
				o->len = htons(u32);
			} else {
				so->code = htons(D6_OPTION_IA_ADDR);
				so->len = htons(sizeof(ap->addr.s6_addr) +
				    sizeof(u32) + sizeof(u32));
				p = D6_OPTION_DATA(so);
				memcpy(p, &ap->addr.s6_addr,
				    sizeof(ap->addr.s6_addr));
				p += sizeof(ap->addr.s6_addr);
				u32 = htonl(ap->prefix_pltime);
				memcpy(p, &u32, sizeof(u32));
				p += sizeof(u32);
				u32 = htonl(ap->prefix_vltime);
				memcpy(p, &u32, sizeof(u32));
				/* Avoid a shadowed declaration warning by
				 * moving our addition outside of the htons
				 * macro */
				u32 = ntohs(o->len) + sizeof(*so)
				    + ntohs(so->len);
				o->len = htons(u32);
			}
		}
	}

	if (state->send->type !=  DHCP6_RELEASE) {
		o = D6_NEXT_OPTION(o);
		o->code = htons(D6_OPTION_ORO);
		o->len = 0;
		u16 = (uint16_t *)(void *)D6_OPTION_DATA(o);
		for (opt = dhcp6_opts; opt->option; opt++) {
			if (opt->type & REQUEST ||
			    has_option_mask(ifo->requestmask6, opt->option))
			{
				*u16++ = htons(opt->option);
				o->len += sizeof(*u16);
			}
		}
		if (o->len == 0) {
			*u16++ = htons(D6_OPTION_DNS_SERVERS);
			*u16++ = htons(D6_OPTION_DOMAIN_LIST);
			o->len = sizeof(*u16) * 2;
		}
		o->len = htons(o->len);
	}

	return 0;
}

static const char *
dhcp6_get_op(uint16_t type)
{
	const struct dhcp6_op *d;

	for (d = dhcp6_ops; d->name; d++)
		if (d->type == type)
			return d->name;
	return NULL;
}

static void
dhcp6_freedrop_addrs(struct interface *ifp, int drop)
{
	struct dhcp6_state *state;
	struct ipv6_addr *ap;

	state = D6_STATE(ifp);
	while ((ap = TAILQ_FIRST(&state->addrs))) {
		TAILQ_REMOVE(&state->addrs, ap, next);
		if (ap->dadcallback)
			eloop_q_timeout_delete(0, NULL, ap->dadcallback);
		/* Only drop the address if no other RAs have assigned it.
		 * This is safe because the RA is removed from the list
		 * before we are called. */
		if (drop && ap->flags & IPV6_AF_ONLINK &&
		    !dhcp6_addrexists(ap) &&
		    !ipv6rs_addrexists(ap))
		{
			syslog(LOG_INFO, "%s: deleting address %s",
			    ifp->name, ap->saddr);
			if (del_address6(ifp, ap) == -1 &&
			    errno != EADDRNOTAVAIL)
				syslog(LOG_ERR, "del_address6 %m");
		}
		free(ap);
	}
	if (drop)
		ipv6_buildroutes();
}

static int
dhcp6_sendmessage(struct interface *ifp, void (*callback)(void *))
{
	struct dhcp6_state *state;
	struct sockaddr_in6 to;
	struct cmsghdr *cm;
	struct in6_pktinfo pi;
	struct timeval RTprev;
	double rnd;
	suseconds_t ms;
	uint8_t neg;

	state = D6_STATE(ifp);
	if (!callback)
		syslog(LOG_DEBUG, "%s: sending %s with xid 0x%02x%02x%02x",
		    ifp->name,
		    dhcp6_get_op(state->send->type),
		    state->send->xid[0],
		    state->send->xid[1],
		    state->send->xid[2]);
	else {
		if (state->RTC == 0) {
			RTprev.tv_sec = state->IRT;
			RTprev.tv_usec = 0;
			state->RT.tv_sec = state->IRT;
			state->RT.tv_usec = 0;
		} else {
			RTprev = state->RT;
			timeradd(&state->RT, &state->RT, &state->RT);
		}

		rnd = DHCP6_RAND_MIN;
		rnd += arc4random() % (DHCP6_RAND_MAX - DHCP6_RAND_MIN);
		rnd /= 1000;
		neg = (rnd < 0.0);
		if (neg)
			rnd = -rnd;
		tv_to_ms(ms, &RTprev);
		ms *= rnd;
		ms_to_tv(&RTprev, ms);
		if (neg)
			timersub(&state->RT, &RTprev, &state->RT);
		else
			timeradd(&state->RT, &RTprev, &state->RT);

		if (state->RT.tv_sec > state->MRT) {
			RTprev.tv_sec = state->MRT;
			RTprev.tv_usec = 0;
			state->RT.tv_sec = state->MRT;
			state->RT.tv_usec = 0;
			tv_to_ms(ms, &RTprev);
			ms *= rnd;
			ms_to_tv(&RTprev, ms);
			if (neg)
				timersub(&state->RT, &RTprev, &state->RT);
			else
				timeradd(&state->RT, &RTprev, &state->RT);
		}

		syslog(LOG_DEBUG,
		    "%s: sending %s (xid 0x%02x%02x%02x),"
		    " next in %0.2f seconds",
		    ifp->name, dhcp6_get_op(state->send->type),
		    state->send->xid[0],
		    state->send->xid[1],
		    state->send->xid[2],
		    timeval_to_double(&state->RT));
	}

	/* Update the elapsed time */
	dhcp6_updateelapsed(ifp, state->send, state->send_len);

	to = alldhcp;
	to.sin6_scope_id = ifp->index;
	sndhdr.msg_name = (caddr_t)&to;
	sndhdr.msg_iov[0].iov_base = (caddr_t)state->send;
	sndhdr.msg_iov[0].iov_len = state->send_len;

	/* Set the outbound interface */
	cm = CMSG_FIRSTHDR(&sndhdr);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(pi));
	memset(&pi, 0, sizeof(pi));
	pi.ipi6_ifindex = ifp->index;
	memcpy(CMSG_DATA(cm), &pi, sizeof(pi));

	if (sendmsg(sock, &sndhdr, 0) == -1) {
		syslog(LOG_ERR, "%s: %s: sendmsg: %m", ifp->name, __func__);
		ifp->options->options &= ~DHCPCD_IPV6;
		dhcp6_drop(ifp, "EXPIRE6");
		return -1;
	}

	state->RTC++;
	if (callback) {
		if (state->MRC == 0 || state->RTC < state->MRC)
			eloop_timeout_add_tv(&state->RT, callback, ifp);
		else if (state->MRC != 0 && state->MRCcallback)
			eloop_timeout_add_tv(&state->RT, state->MRCcallback,
			    ifp);
		else
			syslog(LOG_WARNING, "%s: sent %d times with no reply",
			    ifp->name, state->RTC);
	}
	return 0;
}

static void
dhcp6_sendinform(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendinform);
}

static void
dhcp6_senddiscover(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_senddiscover);
}

static void
dhcp6_sendrequest(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendrequest);
}

static void
dhcp6_sendrebind(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendrebind);
}

static void
dhcp6_sendrenew(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendrenew);
}

static void
dhcp6_sendconfirm(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendconfirm);
}

/*
static void
dhcp6_sendrelease(void *arg)
{

	dhcp6_sendmessage(arg, dhcp6_sendrelease);
}
*/

static void
dhcp6_startrenew(void *arg)
{
	struct interface *ifp;
	struct dhcp6_state *state;

	ifp = arg;
	state = D6_STATE(ifp);
	state->state = DH6S_RENEW;
	state->start_uptime = uptime();
	state->RTC = 0;
	state->IRT = REN_TIMEOUT;
	state->MRT = REN_MAX_RT;
	state->MRC = 0;

	if (dhcp6_makemessage(ifp) == -1)
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
	else
		dhcp6_sendrenew(ifp);
}

static void
dhcp6_startrebind(void *arg)
{
	struct interface *ifp;
	struct dhcp6_state *state;

	ifp = arg;
	eloop_timeout_delete(dhcp6_sendrenew, ifp);
	state = D6_STATE(ifp);
	if (state->state == DH6S_RENEW)
		syslog(LOG_WARNING, "%s: failed to renew, rebinding",
		    ifp->name);
	state->state = DH6S_REBIND;
	state->RTC = 0;
	state->MRC = 0;

	/* RFC 3633 section 12.1 */
	if (ifp->options->ia_type == D6_OPTION_IA_PD) {
	    state->IRT = CNF_TIMEOUT;
	    state->MRT = CNF_MAX_RT;
	} else {
	    state->IRT = REB_TIMEOUT;
	    state->MRT = REB_MAX_RT;
	}

	if (dhcp6_makemessage(ifp) == -1)
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
	else
		dhcp6_sendrebind(ifp);
}

static void
dhcp6_startdiscover(void *arg)
{
	struct interface *ifp;
	struct dhcp6_state *state;

	ifp = arg;
	state = D6_STATE(ifp);
	if (state->state == DH6S_REBIND)
		syslog(LOG_ERR, "%s: failed to renew, soliciting",
		    ifp->name);
	state->state = DH6S_DISCOVER;
	state->start_uptime = uptime();
	state->RTC = 0;
	state->IRT = SOL_TIMEOUT;
	state->MRT = SOL_MAX_RT;
	state->MRC = 0;

	eloop_timeout_delete(NULL, ifp);
	free(state->new);
	state->new = NULL;
	state->new_len = 0;

	/* XXX remove this line when we fix discover stamping on assigned */
	dhcp6_freedrop_addrs(ifp, 0);

	if (dhcp6_makemessage(ifp) == -1)
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
	else
		dhcp6_senddiscover(ifp);
}

static void
dhcp6_failconfirm(void *arg)
{
	struct interface *ifp;

	ifp = arg;
	syslog(LOG_ERR, "%s: failed to confirm prior address", ifp->name);
	/* Section 18.1.2 says that we SHOULD use the last known
	 * IP address(s) and lifetimes if we didn't get a reply.
	 * I disagree with this. */
	if (ifp->options->options & DHCPCD_IPV6)
		dhcp6_startdiscover(ifp);
}

static void
dhcp6_failrequest(void *arg)
{
	struct interface *ifp;

	ifp = arg;
	syslog(LOG_ERR, "%s: failed to request address", ifp->name);
	/* Section 18.1.1 says that client local policy dictates
	 * what happens if a REQUEST fails.
	 * Of the possible scenarios listed, moving back to the
	 * DISCOVER phase makes more sense for us. */
	dhcp6_startdiscover(ifp);
}

static void
dhcp6_startrequest(struct interface *ifp)
{
	struct dhcp6_state *state;

	eloop_timeout_delete(dhcp6_senddiscover, ifp);
	state = D6_STATE(ifp);
	state->state = DH6S_REQUEST;
	state->RTC = 0;
	state->IRT = REQ_TIMEOUT;
	state->MRT = REQ_MAX_RT;
	state->MRC = REQ_MAX_RC;
	state->MRCcallback = dhcp6_failrequest;

	if (dhcp6_makemessage(ifp) == -1) {
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
		return;
	}
	dhcp6_sendrequest(ifp);
}

static void
dhcp6_startconfirm(struct interface *ifp)
{
	struct dhcp6_state *state;

	state = D6_STATE(ifp);
	state->state = DH6S_CONFIRM;
	state->start_uptime = uptime();
	state->RTC = 0;
	state->IRT = CNF_TIMEOUT;
	state->MRT = CNF_MAX_RT;
	state->MRC = 0;

	if (dhcp6_makemessage(ifp) == -1) {
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
		return;
	}
	dhcp6_sendconfirm(ifp);
	eloop_timeout_add_sec(CNF_MAX_RD, dhcp6_failconfirm, ifp);
}

static void
dhcp6_startinform(struct interface *ifp)
{
	struct dhcp6_state *state;

	state = D6_STATE(ifp);
	state->state = DH6S_INFORM;
	state->start_uptime = uptime();
	state->RTC = 0;
	state->IRT = INF_TIMEOUT;
	state->MRT = INF_MAX_RT;
	state->MRC = 0;

	if (dhcp6_makemessage(ifp) == -1)
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
	else
		dhcp6_sendinform(ifp);
}

static void
dhcp6_startexpire(void *arg)
{
	struct interface *ifp;
	const struct dhcp6_state *state;

	ifp = arg;
	eloop_timeout_delete(dhcp6_sendrebind, ifp);

	syslog(LOG_ERR, "%s: DHCPv6 lease expired", ifp->name);
	dhcp6_freedrop_addrs(ifp, 1);
	script_runreason(ifp, "EXPIRE6");
	state = D6_CSTATE(ifp);
	unlink(state->leasefile);
	dhcp6_startdiscover(ifp);
}

static void
dhcp6_startrelease(struct interface *ifp)
{
	struct dhcp6_state *state;

	state = D6_STATE(ifp);
	if (state->state != DH6S_BOUND)
		return;

	state->state = DH6S_RELEASE;
	state->start_uptime = uptime();
	state->RTC = 0;
	state->IRT = REL_TIMEOUT;
	state->MRT = 0;
	state->MRC = REL_MAX_RC;
	//state->MRCcallback = dhcp6_failrelease;
	state->MRCcallback = NULL;

	if (dhcp6_makemessage(ifp) == -1)
		syslog(LOG_ERR, "%s: dhcp6_makemessage: %m", ifp->name);
	else
		/* XXX: We should loop a few times
		 * Luckily RFC3315 section 18.1.6 says this is optional */
		//dhcp6_sendrelease(ifp);
		dhcp6_sendmessage(ifp, NULL);
}

static int dhcp6_getstatus(const struct dhcp6_option *o)
{
	char *nstatus;
	const struct dhcp6_status *s;
	size_t len;

	len = ntohs(o->len);
	if (len < sizeof(uint16_t)) {
		syslog(LOG_ERR, "status truncated");
		return -1;
	}
	if (ntohs(o->code) != D6_OPTION_STATUS_CODE) {
		/* unlikely */
		syslog(LOG_ERR, "not a status");
		return -1;
	}
	s = (const struct dhcp6_status *)o;
	len = ntohs(s->len) - sizeof(s->len);
	if (status == NULL || len + 1 > status_len) {
		status_len = len;
		nstatus = realloc(status, status_len + 1);
		if (nstatus == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			free(status);
		}
		status = nstatus;
	}
	if (status) {
		memcpy(status, (const char *)s + sizeof(*s), len);
		status[len] = '\0';
	}
	return ntohs(s->status);
}

static int
dhcp6_checkstatusok(const struct interface *ifp,
    const struct dhcp6_message *m, const uint8_t *p, size_t len)
{
	const struct dhcp6_option *o;

	if (p)
		o = dhcp6_findoption(D6_OPTION_STATUS_CODE, p, len);
	else
		o = dhcp6_getoption(D6_OPTION_STATUS_CODE, m, len);
	if (o && dhcp6_getstatus(o) != D6_STATUS_OK) {
		syslog(LOG_ERR, "%s: DHCPv6 REPLY: %s", ifp->name, status);
		return -1;
	}
	return 0;
}

static struct ipv6_addr *
dhcp6_findaddr(const struct in6_addr *a, struct interface *ifp)
{
	const struct dhcp6_state *state;
	struct ipv6_addr *ap;

	state = D6_CSTATE(ifp);
	if (state) {
		TAILQ_FOREACH(ap, &state->addrs, next) {
			if (IN6_ARE_ADDR_EQUAL(&ap->addr, a))
				return ap;
		}
	}
	return NULL;
}

int
dhcp6_addrexists(const struct ipv6_addr *a)
{
	struct interface *ifp;

	TAILQ_FOREACH(ifp, ifaces, next) {
		if (dhcp6_findaddr(&a->addr, ifp))
			return 1;
	}
	return 0;
}

static void
dhcp6_dadcallback(void *arg)
{
	struct ipv6_addr *ap = arg;
	struct interface *ifp;
	struct dhcp6_state *state;
	int wascompleted;

	wascompleted = (ap->flags & IPV6_AF_DADCOMPLETED);
	ipv6ns_cancelprobeaddr(ap);
	ap->flags |= IPV6_AF_DADCOMPLETED;
	if (ap->flags & IPV6_AF_DUPLICATED)
		/* XXX FIXME
		 * We should decline the address */
		syslog(LOG_WARNING, "%s: DAD detected %s",
		    ap->iface->name, ap->saddr);
#ifdef IPV6_SEND_DAD
	else
		ipv6_addaddr(ap);
#endif

	if (!wascompleted) {
		ifp = ap->iface;
		state = D6_STATE(ifp);
		if (state->state == DH6S_BOUND ||
		    state->state == DH6S_DELEGATED)
		{
			TAILQ_FOREACH(ap, &state->addrs, next) {
				if ((ap->flags & IPV6_AF_DADCOMPLETED) == 0) {
					wascompleted = 1;
					break;
				}
			}
			if (!wascompleted) {
				syslog(LOG_DEBUG, "%s: DHCPv6 DAD completed",
				    ifp->name);
				script_runreason(ifp, state->reason);
				daemonise();
			}
		}
	}
}

static int
dhcp6_findna(struct interface *ifp, const uint8_t *iaid,
    const uint8_t *d, size_t l)
{
	struct dhcp6_state *state;
	const struct dhcp6_option *o;
	const uint8_t *p;
	struct in6_addr in6;
	struct ipv6_addr *a;
	const struct ipv6_addr *pa;
	char iabuf[INET6_ADDRSTRLEN];
	const char *ia;
	int i;
	uint32_t u32;

	i = 0;
	state = D6_STATE(ifp);
	while ((o = dhcp6_findoption(D6_OPTION_IA_ADDR, d, l))) {
		l -= ((const uint8_t *)o - d);
		d += ((const uint8_t *)o - d);
		u32 = htons(o->len);
		l -= sizeof(*o) + u32;
		d += sizeof(*o) + u32;
		if (u32 < 24) {
			errno = EINVAL;
			syslog(LOG_ERR, "%s: IA Address option truncated",
			    ifp->name);
			continue;
		}
		p = D6_COPTION_DATA(o);
		memcpy(&in6.s6_addr, p, sizeof(in6.s6_addr));
		p += sizeof(in6.s6_addr);
		a = dhcp6_findaddr(&in6, ifp);
		if (a == NULL) {
			a = calloc(1, sizeof(*a));
			if (a == NULL) {
				syslog(LOG_ERR, "%s: %m", __func__);
				break;
			}
			a->iface = ifp;
			a->flags = IPV6_AF_NEW | IPV6_AF_ONLINK;
			a->dadcallback = dhcp6_dadcallback;
			memcpy(a->iaid, iaid, sizeof(a->iaid));
			memcpy(&a->addr.s6_addr, &in6.s6_addr,
			    sizeof(in6.s6_addr));
		}
		pa = ipv6rs_findprefix(a);
		if (pa) {
			memcpy(&a->prefix, &pa->prefix,
			    sizeof(a->prefix));
			a->prefix_len = pa->prefix_len;
		} else {
			a->prefix_len = 64;
			if (ipv6_makeprefix(&a->prefix, &a->addr, 64) == -1) {
				syslog(LOG_ERR, "%s: %m", __func__);
				free(a);
				continue;
			}
		}
		memcpy(&u32, p, sizeof(u32));
		a->prefix_pltime = ntohl(u32);
		p += sizeof(u32);
		memcpy(&u32, p, sizeof(u32));
		a->prefix_vltime = ntohl(u32);
		if (a->prefix_pltime < state->lowpl)
		    state->lowpl = a->prefix_pltime;
		if (a->prefix_vltime > state->expire)
		    state->expire = a->prefix_vltime;
		ia = inet_ntop(AF_INET6, &a->addr.s6_addr,
		    iabuf, sizeof(iabuf));
		snprintf(a->saddr, sizeof(a->saddr),
		    "%s/%d", ia, a->prefix_len);
		if (a->flags & IPV6_AF_STALE)
			a->flags &= ~IPV6_AF_STALE;
		else
			TAILQ_INSERT_TAIL(&state->addrs, a, next);
		i++;
	}
	return i;
}

static int
dhcp6_findpd(struct interface *ifp, const uint8_t *iaid,
    const uint8_t *d, size_t l)
{
	struct dhcp6_state *state;
	const struct dhcp6_option *o;
	const uint8_t *p;
	struct ipv6_addr *a;
	char iabuf[INET6_ADDRSTRLEN];
	const char *ia;
	int i;
	uint8_t u8;
	uint32_t u32;

	i = 0;
	state = D6_STATE(ifp);
	while ((o = dhcp6_findoption(D6_OPTION_IAPREFIX, d, l))) {
		l -= ((const uint8_t *)o - d);
		d += ((const uint8_t *)o - d);
		u32 = htons(o->len);
		l -= sizeof(*o) + u32;
		d += sizeof(*o) + u32;
		if (u32 < 25) {
			errno = EINVAL;
			syslog(LOG_ERR, "%s: IA Prefix option truncated",
			    ifp->name);
			continue;
		}
		a = calloc(1, sizeof(*a));
		if (a == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			break;
		}
		a->iface = ifp;
		a->flags = IPV6_AF_NEW | IPV6_AF_ONLINK;
		a->dadcallback = dhcp6_dadcallback;
		memcpy(a->iaid, iaid, sizeof(a->iaid));
		p = D6_COPTION_DATA(o);
		memcpy(&u32, p, sizeof(u32));
		a->prefix_pltime = ntohl(u32);
		p += sizeof(u32);
		memcpy(&u32, p, sizeof(u32));
		p += sizeof(u32);
		a->prefix_vltime = ntohl(u32);
		if (a->prefix_pltime < state->lowpl)
			state->lowpl = a->prefix_pltime;
		if (a->prefix_vltime > state->expire)
			state->expire = a->prefix_vltime;
		memcpy(&u8, p, sizeof(u8));
		p += sizeof(u8);
		a->prefix_len = u8;
		memcpy(&a->prefix.s6_addr, p, sizeof(a->prefix.s6_addr));
		p += sizeof(a->prefix.s6_addr);
		ia = inet_ntop(AF_INET6, &a->prefix.s6_addr,
		    iabuf, sizeof(iabuf));
		snprintf(a->saddr, sizeof(a->saddr),
		    "%s/%d", ia, a->prefix_len);
		memset(a->addr.s6_addr, 0, sizeof(a->addr.s6_addr));
		TAILQ_INSERT_TAIL(&state->addrs, a, next);
		i++;
	}
	return i;
}

static int
dhcp6_findia(struct interface *ifp, const uint8_t *d, size_t l,
    const char *sfrom)
{
	struct dhcp6_state *state;
	const struct if_options *ifo;
	const struct dhcp6_option *o;
	const uint8_t *p;
	int i;
	uint32_t u32, renew, rebind;
	uint8_t iaid[4];
	size_t ol;
	struct ipv6_addr *ap, *nap;

	ifo = ifp->options;
	i = 0;
	state = D6_STATE(ifp);
	while ((o = dhcp6_findoption(ifo->ia_type, d, l))) {
		l -= ((const uint8_t *)o - d);
		d += ((const uint8_t *)o - d);
		ol = htons(o->len);
		l -= sizeof(*o) + ol;
		d += sizeof(*o) + ol;
		u32 = ifo->ia_type == D6_OPTION_IA_TA ? 4 : 12;
		if (ol < u32) {
			errno = EINVAL;
			syslog(LOG_ERR, "%s: IA option truncated", ifp->name);
			continue;
		}

		p = D6_COPTION_DATA(o);
		memcpy(iaid, p, sizeof(iaid));
		p += sizeof(iaid);
		ol -= sizeof(iaid);
		if (ifo->ia_type != D6_OPTION_IA_TA) {
			memcpy(&u32, p, sizeof(u32));
			renew = ntohl(u32);
			p += sizeof(u32);
			ol -= sizeof(u32);
			memcpy(&u32, p, sizeof(u32));
			rebind = ntohl(u32);
			p += sizeof(u32);
			ol -= sizeof(u32);
			if (renew > rebind && rebind > 0) {
				if (sfrom)
				    syslog(LOG_WARNING,
					"%s: T1 (%d) > T2 (%d) from %s",
					ifp->name, renew, rebind, sfrom);
				renew = 0;
				rebind = 0;
			}
			if (renew != 0 &&
			    (renew < state->renew || state->renew == 0))
				state->renew = renew;
			if (rebind != 0 &&
			    (rebind < state->rebind || state->rebind == 0))
				state->rebind = rebind;
		}
		if (dhcp6_checkstatusok(ifp, NULL, p, ol) == -1)
			return -1;
		if (ifo->ia_type == D6_OPTION_IA_PD) {
			dhcp6_freedrop_addrs(ifp, 0);
			if (dhcp6_findpd(ifp, iaid, p, ol) == 0) {
				syslog(LOG_ERR,
				    "%s: %s: DHCPv6 REPLY missing Prefix",
				    ifp->name, sfrom);
				return -1;
			}
		} else {
			TAILQ_FOREACH(ap, &state->addrs, next) {
				ap->flags |= IPV6_AF_STALE;
			}
			if (dhcp6_findna(ifp, iaid, p, ol) == 0) {
				syslog(LOG_ERR,
				    "%s: %s: DHCPv6 REPLY missing IA Address",
				    ifp->name, sfrom);
				return -1;
			}
			TAILQ_FOREACH_SAFE(ap, &state->addrs, next, nap) {
				if (ap->flags & IPV6_AF_STALE) {
					TAILQ_REMOVE(&state->addrs, ap, next);
					if (ap->dadcallback)
						eloop_q_timeout_delete(0, NULL,
						    ap->dadcallback);
					free(ap);
				}
			}
		}
		i++;
	}
	return i;
}

static int
dhcp6_validatelease(struct interface *ifp,
    const struct dhcp6_message *m, size_t len,
    const char *sfrom)
{
	struct dhcp6_state *state;
	const struct dhcp6_option *o;

	state = D6_STATE(ifp);
	o = dhcp6_getoption(ifp->options->ia_type, m, len);
	if (o == NULL) {
		if (sfrom)
			syslog(LOG_ERR, "%s: no IA in REPLY from %s",
			    ifp->name, sfrom);
		return -1;
	}

	state->renew = state->rebind = state->expire = 0;
	state->lowpl = ND6_INFINITE_LIFETIME;
	len -= (const char *)o - (const char *)m;
	return dhcp6_findia(ifp, (const uint8_t *)o, len, sfrom);
}

static ssize_t
dhcp6_writelease(const struct interface *ifp)
{
	const struct dhcp6_state *state;
	int fd;
	ssize_t bytes;

	state = D6_CSTATE(ifp);
	syslog(LOG_DEBUG, "%s: writing lease `%s'",
	    ifp->name, state->leasefile);

	fd = open(state->leasefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		syslog(LOG_ERR, "%s: dhcp6_writelease: %m", ifp->name);
		return -1;
	}
	bytes = write(fd, state->new, state->new_len);
	close(fd);
	return bytes;
}

static ssize_t
dhcp6_readlease(struct interface *ifp)
{
	struct dhcp6_state *state;
	struct stat st;
	int fd;
	ssize_t bytes;
	struct timeval now;

	state = D6_STATE(ifp);
	if (stat(state->leasefile, &st) == -1) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	syslog(LOG_DEBUG, "%s: reading lease `%s'",
	    ifp->name, state->leasefile);
	state->new = malloc(st.st_size);
	if (state->new == NULL)
		return -1;
	state->new_len = st.st_size;
	fd = open(state->leasefile, O_RDONLY);
	if (fd == -1)
		return -1;
	bytes = read(fd, state->new, state->new_len);
	close(fd);

	/* Check to see if the lease is still valid */
	if (dhcp6_validatelease(ifp, state->new, state->new_len, NULL) == -1)
		goto ex;

	if (state->expire != ND6_INFINITE_LIFETIME) {
		gettimeofday(&now, NULL);
		if ((time_t)state->expire < now.tv_sec - st.st_mtime) {
			syslog(LOG_DEBUG,"%s: discarding expired lease",
			    ifp->name);
			goto ex;
		}
	}

	return bytes;

ex:
	dhcp6_freedrop_addrs(ifp, 0);
	free(state->new);
	state->new = NULL;
	state->new_len = 0;
	unlink(state->leasefile);
	return 0;
}

static void
dhcp6_startinit(struct interface *ifp)
{
	struct dhcp6_state *state;
	int r;

	state = D6_STATE(ifp);
	state->state = DH6S_INIT;
	state->expire = ND6_INFINITE_LIFETIME;
	state->lowpl = ND6_INFINITE_LIFETIME;
	if (!(options & DHCPCD_TEST) &&
	    ifp->options->ia_type != D6_OPTION_IA_TA &&
	    ifp->options->reboot != 0)
	{
		r = dhcp6_readlease(ifp);
		if (r == -1)
			syslog(LOG_ERR, "%s: dhcp6_readlease: %s: %m",
					ifp->name, state->leasefile);
		else if (r != 0) {
			/* RFC 3633 section 12.1 */
			if (ifp->options->ia_type == D6_OPTION_IA_PD)
				dhcp6_startrebind(ifp);
			else
				dhcp6_startconfirm(ifp);
			return;
		}
	}
	dhcp6_startdiscover(ifp);
}

static struct ipv6_addr *
dhcp6_delegate_addr(struct interface *ifp, const struct ipv6_addr *prefix,
    const struct if_sla *sla, struct interface *ifs)
{
	struct dhcp6_state *state;
	struct ipv6_addr *a, *ap;
	int b, i, l, pl, hl;
	const uint8_t *p;
	char iabuf[INET6_ADDRSTRLEN];
	const char *ia;

	state = D6_STATE(ifp);

	l = prefix->prefix_len + sla->sla_len;
	hl= (ifp->hwlen * 8) + (2 * 8); /* 2 magic bytes for ethernet */
	pl = l + hl;
	if (pl < 0 || pl > 128) {
		syslog(LOG_ERR, "%s: invalid prefix length (%d + %d + %d = %d)",
		    ifs->name, prefix->prefix_len, sla->sla_len, hl, pl);
		return NULL;
	}

	if (state == NULL) {
		ifp->if_data[IF_DATA_DHCP6] = calloc(1, sizeof(*state));
		state = D6_STATE(ifp);
		if (state == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return NULL;
		}

		TAILQ_INIT(&state->addrs);
		state->state = DH6S_DELEGATED;
		state->reason = "DELEGATED6";
	}

	a = calloc(1, sizeof(*a));
	if (a == NULL) {
		syslog(LOG_ERR, "%s: %m", __func__);
		return NULL;
	}
	a->iface = ifp;
	a->flags = IPV6_AF_NEW | IPV6_AF_ONLINK;
	a->dadcallback = dhcp6_dadcallback;
	a->delegating_iface = ifs;
	memcpy(&a->iaid, &prefix->iaid, sizeof(a->iaid));
	a->prefix_pltime = prefix->prefix_pltime;
	a->prefix_vltime = prefix->prefix_vltime;
	a->prefix_len = l;

	memset(&a->prefix.s6_addr, 0, sizeof(a->prefix.s6_addr));
	for (i = 0, b = prefix->prefix_len; b > 0; b -= 8, i++)
		a->prefix.s6_addr[i] = prefix->prefix.s6_addr[i];
	p = &sla->sla[3];
	i = (128 - hl) / 8;
	for (b = sla->sla_len; b > 7; b -= 8, p--)
		a->prefix.s6_addr[--i] = *p;
	if (b)
		a->prefix.s6_addr[--i] |= *p;

	if (ipv6_makeaddr(&a->addr, ifp, &a->prefix, a->prefix_len) == -1)
	{
		ia = inet_ntop(AF_INET6, &a->addr.s6_addr,
		    iabuf, sizeof(iabuf));
		syslog(LOG_ERR, "%s: %m (%s/%d)", __func__, ia, a->prefix_len);
		free(a);
		return NULL;
	}

	/* Remove any exiting address */
	TAILQ_FOREACH(ap, &state->addrs, next) {
		if (IN6_ARE_ADDR_EQUAL(&ap->addr, &a->addr)) {
			TAILQ_REMOVE(&state->addrs, ap, next);
			free(ap);
			break;
		}
	}

	ia = inet_ntop(AF_INET6, &a->addr.s6_addr, iabuf, sizeof(iabuf));
	snprintf(a->saddr, sizeof(a->saddr), "%s/%d", ia, a->prefix_len);
	TAILQ_INSERT_TAIL(&state->addrs, a, next);
	return a;
}

static void
dhcp6_delegate_prefix(struct interface *ifp)
{
	struct if_options *ifo;
	struct dhcp6_state *state, *ifd_state;
	struct ipv6_addr *ap;
	size_t i, j, k;
	struct if_iaid *iaid;
	struct if_sla *sla;
	struct interface *ifd;

	ifo = ifp->options;
	state = D6_STATE(ifp);
	TAILQ_FOREACH(ifd, ifaces, next) {
		k = 0;
		TAILQ_FOREACH(ap, &state->addrs, next) {
			for (i = 0; i < ifo->iaid_len; i++) {
				iaid = &ifo->iaid[i];
				if (memcmp(iaid->iaid, ap->iaid,
				    sizeof(iaid->iaid)))
					continue;
				for (j = 0; j < iaid->sla_len; j++) {
					sla = &iaid->sla[j];
					if (strcmp(ifd->name, sla->ifname))
						continue;
					if (dhcp6_delegate_addr(ifd, ap,
					    sla, ifp))
						k++;
				}
			}
		}
		if (k) {
			ifd_state = D6_STATE(ifd);
			ipv6ns_probeaddrs(&ifd_state->addrs);
		}
	}
}

int
dhcp6_find_delegates(struct interface *ifp)
{
	struct if_options *ifo;
	struct dhcp6_state *state;
	struct ipv6_addr *ap;
	size_t i, j, k;
	struct if_iaid *iaid;
	struct if_sla *sla;
	struct interface *ifd;

	k = 0;
	TAILQ_FOREACH(ifd, ifaces, next) {
		ifo = ifd->options;
		if (ifo->ia_type != D6_OPTION_IA_PD)
			continue;
		state = D6_STATE(ifd);
		if (state == NULL || state->state != DH6S_BOUND)
			continue;
		TAILQ_FOREACH(ap, &state->addrs, next) {
			for (i = 0; i < ifo->iaid_len; i++) {
				iaid = &ifo->iaid[i];
				if (memcmp(iaid->iaid, ap->iaid,
				    sizeof(iaid->iaid)))
					continue;
				for (j = 0; j < iaid->sla_len; j++) {
					sla = &iaid->sla[j];
					if (strcmp(ifp->name, sla->ifname))
						continue;
					if (dhcp6_delegate_addr(ifp, ap,
					    sla, ifd))
					    k++;
				}
			}
		}
	}

	if (k) {
		syslog(LOG_INFO, "%s: adding delegated prefixes", ifp->name);
		state = D6_STATE(ifp);
		state->state = DH6S_DELEGATED;
		ipv6ns_probeaddrs(&state->addrs);
		ipv6_buildroutes();
	}
	return k;
}

/* ARGSUSED */
static void
dhcp6_handledata(__unused void *arg)
{
	ssize_t len;
	struct cmsghdr *cm;
	struct in6_pktinfo pkt;
	struct interface *ifp;
	const char *sfrom, *op;
	struct dhcp6_message *r;
	struct dhcp6_state *state;
	const struct dhcp6_option *o;
	const struct dhcp_opt *opt;
	const struct if_options *ifo;
	const struct ipv6_addr *ap;
	uint8_t stale;

	len = recvmsg(sock, &rcvhdr, 0);
	if (len == -1) {
		syslog(LOG_ERR, "recvmsg: %m");
		return;
	}
	sfrom = inet_ntop(AF_INET6, &from.sin6_addr,
	    ntopbuf, INET6_ADDRSTRLEN);
	if ((size_t)len < sizeof(struct dhcp6_message)) {
		syslog(LOG_ERR, "DHCPv6 RA packet too short from %s", sfrom);
		return;
	}

	pkt.ipi6_ifindex = 0;
	for (cm = (struct cmsghdr *)CMSG_FIRSTHDR(&rcvhdr);
	     cm;
	     cm = (struct cmsghdr *)CMSG_NXTHDR(&rcvhdr, cm))
	{
		if (cm->cmsg_level != IPPROTO_IPV6)
			continue;
		switch(cm->cmsg_type) {
		case IPV6_PKTINFO:
			if (cm->cmsg_len == CMSG_LEN(sizeof(pkt)))
				memcpy(&pkt, CMSG_DATA(cm), sizeof(pkt));
			break;
		}
	}

	if (pkt.ipi6_ifindex == 0) {
		syslog(LOG_ERR,
		    "DHCPv6 reply did not contain index from %s",
		    sfrom);
		return;
	}

	TAILQ_FOREACH(ifp, ifaces, next) {
		if (ifp->index == (unsigned int)pkt.ipi6_ifindex)
			break;
	}
	if (ifp == NULL) {
		syslog(LOG_ERR, "DHCPv6 reply for unexpected interface from %s",
		    sfrom);
		return;
	}
	state = D6_STATE(ifp);
	if (state == NULL || state->send == NULL) {
		syslog(LOG_ERR, "%s: DHCPv6 reply received but not running",
		    ifp->name);
		return;
	}
	/* We're already bound and this message is for another machine */
	if (state->state == DH6S_BOUND)
		return;

	r = (struct dhcp6_message *)rcvhdr.msg_iov[0].iov_base;
	if (r->xid[0] != state->send->xid[0] ||
	    r->xid[1] != state->send->xid[1] ||
	    r->xid[2] != state->send->xid[2])
	{
		syslog(LOG_ERR,
		    "%s: wrong xid 0x%02x%02x%02x"
		    " (expecting 0x%02x%02x%02x) from %s",
		    ifp->name,
		    r->xid[0], r->xid[1], r->xid[2],
		    state->send->xid[0], state->send->xid[1],
		    state->send->xid[2],
		    sfrom);
		return;
	}

	if (dhcp6_getoption(D6_OPTION_SERVERID, r, len) == NULL) {
		syslog(LOG_ERR, "%s: no DHCPv6 server ID from %s",
		    ifp->name, sfrom);
		return;
	}

	o = dhcp6_getoption(D6_OPTION_CLIENTID, r, len);
	if (o == NULL || ntohs(o->len) != duid_len ||
	    memcmp(D6_COPTION_DATA(o), duid, duid_len) != 0)
	{
		syslog(LOG_ERR, "%s: incorrect client ID from %s",
		    ifp->name, sfrom);
		return;
	}

	ifo = ifp->options;
	for (opt = dhcp6_opts; opt->option; opt++) {
		if (has_option_mask(ifo->requiremask6, opt->option) &&
		    dhcp6_getoption(opt->option, r, len) == NULL)
		{
			syslog(LOG_WARNING,
			    "%s: reject DHCPv6 (no option %s) from %s",
			    ifp->name, opt->var, sfrom);
			return;
		}
	}

	op = dhcp6_get_op(r->type);
	switch(r->type) {
	case DHCP6_REPLY:
		if (state->state == DH6S_INFORM)
			break;
		switch(state->state) {
		case DH6S_CONFIRM:
			if (dhcp6_checkstatusok(ifp, r, NULL, len) == -1) {
				dhcp6_startdiscover(ifp);
				return;
			}
			goto recv;
		case DH6S_REQUEST: /* FALLTHROUGH */
		case DH6S_RENEW: /* FALLTHROUGH */
		case DH6S_REBIND:
			goto replyok;
		default:
			op = NULL;
		}
		break;
	case DHCP6_ADVERTISE:
		if (state->state != DH6S_DISCOVER) {
			op = NULL;
			break;
		}
replyok:
		if (dhcp6_validatelease(ifp, r, len, sfrom) == -1)
			return;
		break;
	default:
		syslog(LOG_ERR, "%s: invalid DHCP6 type %s (%d)",
		    ifp->name, op, r->type);
		return;
	}
	if (op == NULL) {
		syslog(LOG_WARNING, "%s: invalid state for DHCP6 type %s (%d)",
		    ifp->name, op, r->type);
		return;
	}

	if (state->recv_len < (size_t)len) {
		free(state->recv);
		state->recv = malloc(len);
		if (state->recv == NULL) {
			syslog(LOG_ERR, "%s: malloc recv: %m", ifp->name);
			return;
		}
	}
	memcpy(state->recv, r, len);
	state->recv_len = len;

	switch(r->type) {
	case DHCP6_ADVERTISE:
		ap = TAILQ_FIRST(&state->addrs);
		syslog(LOG_INFO, "%s: ADV %s from %s",
		    ifp->name, ap->saddr, sfrom);
		dhcp6_startrequest(ifp);
		return;
	}

recv:
	stale = 1;
	TAILQ_FOREACH(ap, &state->addrs, next) {
		if (ap->flags & IPV6_AF_NEW) {
			stale = 0;
			break;
		}
	}
	syslog(stale ? LOG_DEBUG : LOG_INFO,
	    "%s: %s received from %s", ifp->name, op, sfrom);

	state->reason = NULL;
	eloop_timeout_delete(NULL, ifp);
	switch(state->state) {
	case DH6S_INFORM:
		state->renew = 0;
		state->rebind = 0;
		state->expire = ND6_INFINITE_LIFETIME;
		state->lowpl = ND6_INFINITE_LIFETIME;
		state->reason = "INFORM6";
		break;
	case DH6S_REQUEST:
		if (state->reason == NULL)
			state->reason = "BOUND6";
		/* FALLTHROUGH */
	case DH6S_RENEW:
		if (state->reason == NULL)
			state->reason = "RENEW6";
		/* FALLTHROUGH */
	case DH6S_REBIND:
		if (state->reason == NULL)
			state->reason = "REBIND6";
	case DH6S_CONFIRM:
		if (state->reason == NULL)
			state->reason = "REBOOT6";
		if (state->renew == 0) {
			if (state->expire == ND6_INFINITE_LIFETIME)
				state->renew = ND6_INFINITE_LIFETIME;
			else
				state->renew = state->lowpl * 0.5;
		}
		if (state->rebind == 0) {
			if (state->expire == ND6_INFINITE_LIFETIME)
				state->rebind = ND6_INFINITE_LIFETIME;
			else
				state->rebind = state->lowpl * 0.8;
		}
		break;
	default:
		state->reason = "UNKNOWN6";
		break;
	}

	if (state->state != DH6S_CONFIRM) {
		free(state->old);
		state->old = state->new;
		state->old_len = state->new_len;
		state->new = state->recv;
		state->new_len = state->recv_len;
		state->recv = NULL;
		state->recv_len = 0;
	}

	if (options & DHCPCD_TEST)
		script_runreason(ifp, "TEST");
	else {
		if (state->state == DH6S_INFORM)
			script_runreason(ifp, state->reason);
		state->state = DH6S_BOUND;
		if (state->renew && state->renew != ND6_INFINITE_LIFETIME)
			eloop_timeout_add_sec(state->renew,
			    dhcp6_startrenew, ifp);
		if (state->rebind && state->rebind != ND6_INFINITE_LIFETIME)
			eloop_timeout_add_sec(state->rebind,
			    dhcp6_startrebind, ifp);
		if (state->expire && state->expire != ND6_INFINITE_LIFETIME)
			eloop_timeout_add_sec(state->expire,
			    dhcp6_startexpire, ifp);
		if (ifp->options->ia_type == D6_OPTION_IA_PD)
			dhcp6_delegate_prefix(ifp);
		ipv6ns_probeaddrs(&state->addrs);
		if (state->renew || state->rebind)
			syslog(stale ? LOG_DEBUG : LOG_INFO,
			    "%s: renew in %"PRIu32" seconds,"
			    " rebind in %"PRIu32" seconds",
			    ifp->name, state->renew, state->rebind);
		ipv6_buildroutes();
		dhcp6_writelease(ifp);

		len = 1;
		/* If all addresses have completed DAD run the script */
		TAILQ_FOREACH(ap, &state->addrs, next) {
			if (ap->flags & IPV6_AF_ONLINK &&
			    (ap->flags & IPV6_AF_DADCOMPLETED) == 0)
			{
				len = 0;
				break;
			}
		}
		if (len) {
			script_runreason(ifp, state->reason);
			daemonise();
		} else
			syslog(LOG_DEBUG,
			    "%s: waiting for DHCPv6 DAD to complete",
			    ifp->name);
	}

	if (options & DHCPCD_TEST ||
	    (ifp->options->options & DHCPCD_INFORM &&
	    !(options & DHCPCD_MASTER)))
	{
#ifdef DEBUG_MEMORY
		dhcp6_free(ifp);
#endif
		exit(EXIT_SUCCESS);
	}
}

static int
dhcp6_open(void)
{
	struct sockaddr_in6 sa;
	int n;

	if (sndbuf == NULL && dhcp6_init() == -1)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_port = htons(DHCP6_CLIENT_PORT);
#ifdef BSD
	sa.sin6_len = sizeof(sa);
#endif

	sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == -1)
		return -1;

	n = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
	    &n, sizeof(n)) == -1)
		goto errexit;

	n = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
	    &n, sizeof(n)) == -1)
		goto errexit;

#ifdef SO_REUSEPORT
	n = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
	    &n, sizeof(n)) == -1)
		goto errexit;
#endif

	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		goto errexit;

	n = 1;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO,
	    &n, sizeof(n)) == -1)
		goto errexit;

	if (set_cloexec(sock) == -1 || set_nonblock(sock) == -1)
		goto errexit;

	eloop_event_add(sock, dhcp6_handledata, NULL);

	return 0;

errexit:
	close(sock);
	return -1;
}

static void
dhcp6_start1(void *arg)
{
	struct interface *ifp = arg;
	struct dhcp6_state *state;

	state = D6_STATE(ifp);
	syslog(LOG_INFO, "%s: %s", ifp->name,
	    state->state == DH6S_INFORM ?
	    "requesting DHCPv6 information" :
	    "soliciting a DHCPv6 address");
	if (state->state == DH6S_INFORM)
		dhcp6_startinform(ifp);
	else
		dhcp6_startinit(ifp);
}

int
dhcp6_start(struct interface *ifp, enum DH6S init_state)
{
	struct dhcp6_state *state;

	state = D6_STATE(ifp);
	if (state) {
		if (state->state == DH6S_DELEGATED)
			return dhcp6_find_delegates(ifp);
		/* We're already running DHCP6 */
		/* XXX: What if the managed flag changes? */
		return 0;
	}

	if (sock == -1 && dhcp6_open() == -1)
		return -1;

	if (duid == NULL) {
		duid = malloc(DUID_LEN);
		if (duid == NULL)
			return -1;
		duid_len = get_duid(duid, ifp);
	}

	ifp->if_data[IF_DATA_DHCP6] = calloc(1, sizeof(*state));
	state = D6_STATE(ifp);
	if (state == NULL)
		return -1;

	TAILQ_INIT(&state->addrs);
	if (dhcp6_find_delegates(ifp))
		return 0;

	state->state = init_state;
	snprintf(state->leasefile, sizeof(state->leasefile),
	    LEASEFILE6, ifp->name);

	if (ipv6_linklocal(ifp) == NULL) {
		syslog(LOG_DEBUG,
		    "%s: delaying DHCPv6 soliciation for LL address",
		    ifp->name);
		ipv6_addlinklocalcallback(ifp, dhcp6_start1, ifp);
		return 0;
	}

	dhcp6_start1(ifp);
	return 0;
}

static void
dhcp6_freedrop(struct interface *ifp, int drop, const char *reason)
{
	struct dhcp6_state *state;

	eloop_timeout_delete(NULL, ifp);
	state = D6_STATE(ifp);
	if (state) {
		if (ifp->options->options & DHCPCD_RELEASE) {
			if (ifp->carrier != LINK_DOWN)
				dhcp6_startrelease(ifp);
			unlink(state->leasefile);
		}
		dhcp6_freedrop_addrs(ifp, drop);
		if (drop && state->new) {
			if (reason == NULL)
				reason = "STOP6";
			script_runreason(ifp, reason);
		}
		free(state->send);
		free(state->recv);
		free(state->new);
		free(state->old);
		free(state);
		ifp->if_data[IF_DATA_DHCP6] = NULL;
	}

	/* If we don't have any more DHCP6 enabled interfaces,
	 * close the global socket */
	if (ifaces) {
		TAILQ_FOREACH(ifp, ifaces, next) {
			if (D6_STATE(ifp))
				break;
		}
	}
	if (ifp == NULL && sock != -1) {
		close(sock);
		eloop_event_delete(sock);
		sock = -1;
	}
}

void
dhcp6_drop(struct interface *ifp, const char *reason)
{

	dhcp6_freedrop(ifp, 1, reason);
}

void
dhcp6_free(struct interface *ifp)
{

	dhcp6_freedrop(ifp, 0, NULL);
}

void
dhcp6_handleifa(int cmd, const char *ifname,
    const struct in6_addr *addr, int flags)
{
	struct interface *ifp;
	struct dhcp6_state *state;

	TAILQ_FOREACH(ifp, ifaces, next) {
		state = D6_STATE(ifp);
		if (state == NULL || strcmp(ifp->name, ifname))
			continue;
		ipv6_handleifa_addrs(cmd, &state->addrs, addr, flags);
	}
}

ssize_t
dhcp6_env(char **env, const char *prefix, const struct interface *ifp,
    const struct dhcp6_message *m, ssize_t mlen)
{
	const struct dhcp6_state *state;
	const struct if_options *ifo;
	const struct dhcp_opt *opt;
	const struct dhcp6_option *o;
	ssize_t len, e;
	uint16_t ol;
	const uint8_t *od;
	char **ep, *v, *val;
	const struct ipv6_addr *ap;

	state = D6_CSTATE(ifp);
	e = 0;
	ep = env;
	ifo = ifp->options;
	for (opt = dhcp6_opts; opt->option; opt++) {
		if (!opt->var)
			continue;
		if (has_option_mask(ifo->nomask6, opt->option))
			continue;
		o = dhcp6_getoption(opt->option, m, mlen);
		if (o == NULL)
			continue;
		if (env == NULL) {
			e++;
			continue;
		}
		ol = ntohs(o->len);
		od = D6_COPTION_DATA(o);
		len = print_option(NULL, 0, opt->type, ol, od, ifp->name);
		if (len < 0)
			return -1;
		e = strlen(prefix) + 6 + strlen(opt->var) + len + 4;
		v = val = *ep++ = malloc(e);
		if (v == NULL) {
			syslog(LOG_ERR, "%s: %m", __func__);
			return -1;
		}
		v += snprintf(val, e, "%s_dhcp6_%s=", prefix, opt->var);
		if (len != 0)
			print_option(v, len, opt->type, ol, od, ifp->name);

	}

	if (TAILQ_FIRST(&state->addrs)) {
		if (env == NULL)
			e++;
		else {
			if (ifo->ia_type == D6_OPTION_IA_PD) {
				e = strlen(prefix) +
				    strlen("_dhcp6_prefix=");
				TAILQ_FOREACH(ap, &state->addrs, next) {
					e += strlen(ap->saddr) + 1;
				}
				v = val = *ep++ = malloc(e);
				if (v == NULL) {
					syslog(LOG_ERR, "%s: %m", __func__);
					return -1;
				}
				v += snprintf(val, e, "%s_dhcp6_prefix=",
					prefix);
				TAILQ_FOREACH(ap, &state->addrs, next) {
					strcpy(v, ap->saddr);
					v += strlen(ap->saddr);
					*v++ = ' ';
				}
				*--v = '\0';
			} else {
				e = strlen(prefix) +
				    strlen("_dhcp6_ip_address=");
				TAILQ_FOREACH(ap, &state->addrs, next) {
					e += strlen(ap->saddr) + 1;
				}
				v = val = *ep++ = malloc(e);
				if (v == NULL) {
					syslog(LOG_ERR, "%s: %m", __func__);
					return -1;
				}
				v += snprintf(val, e, "%s_dhcp6_ip_address=",
					prefix);
				TAILQ_FOREACH(ap, &state->addrs, next) {
					strcpy(v, ap->saddr);
					v += strlen(ap->saddr);
					*v++ = ' ';
				}
				*--v = '\0';
			}
		}
	}

	if (env == NULL)
		return e;
	return ep - env;
}
