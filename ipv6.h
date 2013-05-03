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

#ifndef IPV6_H
#define IPV6_H

#include <sys/queue.h>

#include <netinet/in.h>

#define ALLROUTERS "ff02::2"
#define HOPLIMIT 255

#define ROUNDUP8(a) (1 + (((a) - 1) | 7))

struct ipv6_addr {
	TAILQ_ENTRY(ipv6_addr) next;
	struct interface *iface;
	struct in6_addr prefix;
	int prefix_len;
	uint32_t prefix_vltime;
	uint32_t prefix_pltime;
	struct in6_addr addr;
	uint8_t onlink;
	uint8_t new;
	char saddr[INET6_ADDRSTRLEN];
	uint8_t added;
	uint8_t autoconf;
	uint8_t iaid[4];
	struct interface *delegating_iface;

	void (*dadcallback)(void *);
	uint8_t dad;
	uint8_t dadcompleted;
	uint8_t *ns;
	size_t nslen;
	int nsprobes;
};
TAILQ_HEAD(ipv6_addrhead, ipv6_addr);

struct rt6 {
	TAILQ_ENTRY(rt6) next;
	struct in6_addr dest;
	struct in6_addr net;
	struct in6_addr gate;
	const struct interface *iface;
	struct ra *ra;
	int metric;
	unsigned int mtu;
};
TAILQ_HEAD(rt6head, rt6);

#ifdef INET6
int ipv6_init(void);
ssize_t ipv6_printaddr(char *, ssize_t, const uint8_t *, const char *);
struct in6_addr *ipv6_linklocal(const char *);
int ipv6_makeaddr(struct in6_addr *, const char *, const struct in6_addr *, int);
int ipv6_makeprefix(struct in6_addr *, const struct in6_addr *, int);
int ipv6_mask(struct in6_addr *, int);
int ipv6_prefixlen(const struct in6_addr *);
int ipv6_addaddr(struct ipv6_addr *);
ssize_t ipv6_addaddrs(struct ipv6_addrhead *);
int ipv6_removesubnet(const struct interface *, struct ipv6_addr *);
void ipv6_buildroutes(void);
void ipv6_drop(struct interface *);
#else
#define ipv6_init() -1
#endif

#endif
