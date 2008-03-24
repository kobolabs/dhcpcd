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
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include <arpa/inet.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "interface.h"
#include "logger.h"

/* Darwin doesn't define this for some very odd reason */
#ifndef SA_SIZE
# define SA_SIZE(sa)						\
	(  (!(sa) || ((struct sockaddr *)(sa))->sa_len == 0) ?	\
	   sizeof(long)		:				\
	   1 + ( (((struct sockaddr *)(sa))->sa_len - 1) | (sizeof(long) - 1) ) )
#endif

int
if_address(const char *ifname, struct in_addr address,
	   struct in_addr netmask, struct in_addr broadcast, int del)
{
	int s;
	struct ifaliasreq ifa;
	union {
		struct sockaddr *sa;
		struct sockaddr_in *sin;
	} _s;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		logger(LOG_ERR, "socket: %s", strerror(errno));
		return -1;
	}

	memset(&ifa, 0, sizeof(ifa));
	strlcpy(ifa.ifra_name, ifname, sizeof(ifa.ifra_name));

#define ADDADDR(_var, _addr) \
	_s.sa = &_var; \
	_s.sin->sin_family = AF_INET; \
	_s.sin->sin_len = sizeof(*_s.sin); \
	memcpy(&_s.sin->sin_addr, &_addr, sizeof(_s.sin->sin_addr));

	ADDADDR(ifa.ifra_addr, address);
	ADDADDR(ifa.ifra_mask, netmask);
	if (!del)
		ADDADDR(ifa.ifra_broadaddr, broadcast);
#undef ADDADDR

	if (ioctl(s, del ? SIOCDIFADDR : SIOCAIFADDR, &ifa) == -1) {
		logger(LOG_ERR, "ioctl %s: %s",
		       del ? "SIOCDIFADDR" : "SIOCAIFADDR",
		       strerror(errno));
		close(s);
		return -1;
	}

	close(s);
	return 0;
}

int
if_route(const char *ifname, struct in_addr destination,
	 struct in_addr netmask, struct in_addr gateway,
	 int metric, int change, int del)
{
	int s;
	static int seq;
	union sockunion {
		struct sockaddr sa;
		struct sockaddr_in sin;
#ifdef INET6
		struct sockaddr_in6 sin6;
#endif
		struct sockaddr_dl sdl;
		struct sockaddr_storage ss;
	} su;
	struct rtm 
	{
		struct rt_msghdr hdr;
		char buffer[sizeof(su) * 3];
	} rtm;
	char *bp = rtm.buffer;
	size_t l;
	unsigned char *hwaddr;
	size_t hwlen = 0;

	log_route(destination, netmask, gateway, metric, change, del);

	if ((s = socket(PF_ROUTE, SOCK_RAW, 0)) == -1) {
		logger(LOG_ERR, "socket: %s", strerror(errno));
		return -1;
	}

	memset(&rtm, 0, sizeof(rtm));

	rtm.hdr.rtm_version = RTM_VERSION;
	rtm.hdr.rtm_seq = ++seq;
	rtm.hdr.rtm_type = change ? RTM_CHANGE : del ? RTM_DELETE : RTM_ADD;
	rtm.hdr.rtm_flags = RTF_UP | RTF_STATIC;

	/* This order is important */
	rtm.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;

#define ADDADDR(_addr) \
	memset (&su, 0, sizeof(su)); \
	su.sin.sin_family = AF_INET; \
	su.sin.sin_len = sizeof(su.sin); \
	memcpy (&su.sin.sin_addr, &_addr, sizeof(su.sin.sin_addr)); \
	l = SA_SIZE (&(su.sa)); \
	memcpy (bp, &(su), l); \
	bp += l;

	ADDADDR (destination);

	if (netmask.s_addr == INADDR_BROADCAST ||
	    gateway.s_addr == INADDR_ANY)
	{
		/* Make us a link layer socket */
		if (netmask.s_addr == INADDR_BROADCAST) 
			rtm.hdr.rtm_flags |= RTF_HOST;

		hwaddr = xmalloc(sizeof(unsigned char) * HWADDR_LEN);
		_do_interface(ifname, hwaddr, &hwlen, NULL, false, false);
		memset(&su, 0, sizeof(su));
		su.sdl.sdl_len = sizeof(su.sdl);
		su.sdl.sdl_family = AF_LINK;
		su.sdl.sdl_nlen = strlen(ifname);
		memcpy(&su.sdl.sdl_data, ifname, (size_t)su.sdl.sdl_nlen);
		su.sdl.sdl_alen = hwlen;
		memcpy(((unsigned char *)&su.sdl.sdl_data) + su.sdl.sdl_nlen,
		       hwaddr, (size_t)su.sdl.sdl_alen);

		l = SA_SIZE(&(su.sa));
		memcpy(bp, &su, l);
		bp += l;
		free(hwaddr);
	} else {
		rtm.hdr.rtm_flags |= RTF_GATEWAY;
		ADDADDR(gateway);
	}

	ADDADDR(netmask);
#undef ADDADDR

	rtm.hdr.rtm_msglen = l = bp - (char *)&rtm;
	if (write(s, &rtm, l) == -1) {
		/* Don't report error about routes already existing */
		if (errno != EEXIST)
			logger(LOG_ERR, "write: %s", strerror(errno));
		close(s);
		return -1;
	}

	close(s);
	return 0;
}
