/*
 * dhcpcd - DHCP client daemon -
 * Copyright 2006-2007 Roy Marples <uberlord@gentoo.org>
 * 
 * dhcpcd is an RFC2131 compliant DHCP client daemon.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <net/if_arp.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "dhcpcd.h"
#include "dhcp.h"
#include "interface.h"
#include "logger.h"
#include "socket.h"

#define BROADCAST_FLAG 0x8000

static const char *dhcp_message[] = {
  [DHCP_DISCOVER] 	= "DHCP_DISCOVER",
  [DHCP_OFFER]		= "DHCP_OFFER",
  [DHCP_REQUEST]	= "DHCP_REQUEST",
  [DHCP_DECLINE]	= "DHCP_DECLINE",
  [DHCP_ACK]		= "DHCP_ACK",
  [DHCP_NAK]		= "DHCP_NAK",
  [DHCP_RELEASE]	= "DHCP_RELEASE",
  [DHCP_INFORM]		= "DHCP_INFORM",
  [DHCP_INFORM + 1]	= NULL
};

size_t send_message (const interface_t *iface, const dhcp_t *dhcp,
		     unsigned long xid, char type,
		     const options_t *options)
{
  dhcpmessage_t message;
  struct udp_dhcp_packet packet;
  unsigned char *m = (unsigned char *) &message;
  unsigned char *p = (unsigned char *) &message.options;
  unsigned char *n_params = NULL;
  unsigned long l;
  struct in_addr from;
  struct in_addr to;
  long up = uptime() - iface->start_uptime;
  uint32_t ul;
  uint16_t sz;
  unsigned int message_length;

  if (!iface || !options || !dhcp)
    return -1;

  memset (&from, 0, sizeof (from));
  memset (&to, 0, sizeof (to));

  if (type == DHCP_RELEASE)
    to.s_addr = dhcp->serveraddress.s_addr;

  memset (&message, 0, sizeof (dhcpmessage_t));

  if (type == DHCP_INFORM ||
      type == DHCP_RELEASE ||
      type == DHCP_REQUEST)
    {
      message.ciaddr = iface->previous_address.s_addr;
      from.s_addr = iface->previous_address.s_addr;
    }

  message.op = DHCP_BOOTREQUEST;
  message.hwtype = iface->family;
  switch (iface->family)
    {
    case ARPHRD_ETHER:
    case ARPHRD_IEEE802:
      message.hwlen = ETHER_ADDR_LEN;
      memcpy (&message.chaddr, &iface->hwaddr, ETHER_ADDR_LEN);
      break;
    case ARPHRD_IEEE1394:
    case ARPHRD_INFINIBAND:
      if (message.ciaddr == 0)
	message.flags = htons (BROADCAST_FLAG);
      message.hwlen = 0;
      break;
    default:
      logger (LOG_ERR, "dhcp: unknown hardware type %d", iface->family);
    }

  if (up < 0 || up > UINT16_MAX)
    message.secs = htons ((short) UINT16_MAX);
  else
    message.secs = htons (up);
  message.xid = xid;
  message.cookie = htonl (MAGIC_COOKIE);

  *p++ = DHCP_MESSAGETYPE; 
  *p++ = 1;
  *p++ = type;

  if (type == DHCP_REQUEST)
    {
      *p++ = DHCP_MAXMESSAGESIZE;
      *p++ = 2;
      sz = get_mtu (iface->name);
      if (sz < MTU_MIN)
	{
	  if (set_mtu (iface->name, MTU_MIN) == 0)
	    sz = MTU_MIN;
	}
      sz = htons (sz);
      memcpy (p, &sz, 2);
      p += 2;
    }

#define PUTADDR(_type, _val) \
    { \
      *p++ = _type; \
      *p++ = 4; \
      memcpy (p, &_val.s_addr, 4); \
      p += 4; \
    }
  if (dhcp->address.s_addr != 0 && iface->previous_address.s_addr == 0
      && type != DHCP_RELEASE)
    PUTADDR (DHCP_ADDRESS, dhcp->address);

  if (dhcp->serveraddress.s_addr != 0 && dhcp->address.s_addr !=0 &&
      (iface->previous_address.s_addr == 0 || type == DHCP_RELEASE))
    PUTADDR (DHCP_SERVERIDENTIFIER, dhcp->serveraddress);
#undef PUTADDR

  if (type == DHCP_REQUEST || type == DHCP_DISCOVER)
    {
      if (options->leasetime != 0)
	{
	  *p++ = DHCP_LEASETIME;
	  *p++ = 4;
	  ul = htonl (options->leasetime);
	  memcpy (p, &ul, 4);
	  p += 4;
	}
    }

  if (type == DHCP_DISCOVER || type == DHCP_INFORM || type == DHCP_REQUEST)
    {
      *p++ = DHCP_PARAMETERREQUESTLIST;
      n_params = p;
      *p++ = 0;

      /* Only request DNSSERVER in discover to keep the packets small.
	 RFC2131 Section 3.5 states that the REQUEST must include the list
	 from the DISCOVER message, so I think we can safely do this. */

      if (type == DHCP_DISCOVER)
	*p++ = DHCP_DNSSERVER;
      else
	{
	  *p++ = DHCP_RENEWALTIME;
	  *p++ = DHCP_REBINDTIME;
	  *p++ = DHCP_NETMASK;
	  *p++ = DHCP_BROADCAST;
	  *p++ = DHCP_CSR;
	  /* RFC 3442 states classless static routes should be before routers
	   * and static routes as classless static routes override them both */
	  *p++ = DHCP_STATICROUTE;
	  *p++ = DHCP_ROUTERS;
	  *p++ = DHCP_HOSTNAME;
	  *p++ = DHCP_DNSSEARCH;
	  *p++ = DHCP_DNSDOMAIN;
	  *p++ = DHCP_DNSSERVER;
	  *p++ = DHCP_NISDOMAIN;
	  *p++ = DHCP_NISSERVER;
	  *p++ = DHCP_NTPSERVER;
	  *p++ = DHCP_MTU;
	  /* These parameters were requested by dhcpcd-2.0 and earlier
	     but we never did anything with them */
	  /*    *p++ = DHCP_DEFAULTIPTTL;
	   *p++ = DHCP_MASKDISCOVERY;
	   *p++ = DHCP_ROUTERDISCOVERY; */
	}

      *n_params = p - n_params - 1;

      if (*options->hostname) 
	{
	  if (options->fqdn == FQDN_DISABLE)
	    {
	      *p++ = DHCP_HOSTNAME;
	      *p++ = l = strlen (options->hostname);
	      memcpy (p, options->hostname, l);
	      p += l;
	    }
	  else
	    {
	      /* Draft IETF DHC-FQDN option (81) */
	      *p++ = DHCP_FQDN;
	      *p++ = (l = strlen (options->hostname)) + 3;
	      /* Flags: 0000NEOS
	       * S: 1 => Client requests Server to update A RR in DNS as well as PTR
	       * O: 1 => Server indicates to client that DNS has been updated
	       * E: 1 => Name data is DNS format
	       * N: 1 => Client requests Server to not update DNS
	       */
	      *p++ = options->fqdn & 0x9;
	      *p++ = 0; /* rcode1, response from DNS server for PTR RR */
	      *p++ = 0; /* rcode2, response from DNS server for A RR if S=1 */
	      memcpy (p, options->hostname, l);
	      p += l;
	    }
	}
    }

  if (type != DHCP_DECLINE && type != DHCP_RELEASE)
    {
      if (options->userclass_len > 0)
	{
	  *p++ = DHCP_USERCLASS;
	  *p++ = options->userclass_len;
	  memcpy (p, &options->userclass, options->userclass_len);
	  p += options->userclass_len;
	}
      
      *p++ = DHCP_CLASSID;
      *p++ = l = strlen (options->classid);
      memcpy (p, options->classid, l);
      p += l;
    }

  *p++ = DHCP_CLIENTID;
  if (options->clientid[0])
    {
      l = strlen (options->clientid);
      *p++ = l + 1;
      *p++ = 0; /* string */
      memcpy (p, options, l);
      p += l;
    }
  else
    {
      *p++ = iface->hwlen + 1;
      *p++ = iface->family;
      memcpy (p, &iface->hwaddr, iface->hwlen);
      p += iface->hwlen;
    }

  *p++ = DHCP_END;

  message_length = p - m;

  memset (&packet, 0, sizeof (struct udp_dhcp_packet));
  make_dhcp_packet (&packet, (unsigned char *) &message, message_length,
		    from, to);

  logger (LOG_DEBUG, "sending %s with xid 0x%x", dhcp_message[(int) type], xid);
  return send_packet (iface, ETHERTYPE_IP, (unsigned char *) &packet,
		      message_length + sizeof (struct ip) +
		      sizeof (struct udphdr));
}

static unsigned long getnetmask (unsigned long ip_in)
{
  unsigned long t, p = ntohl (ip_in);

  if (IN_CLASSA (p))
    t = ~IN_CLASSA_NET;
  else
    {
      if (IN_CLASSB (p))
	t = ~IN_CLASSB_NET;
      else
	{
	  if (IN_CLASSC (p))
	    t = ~IN_CLASSC_NET;
	  else
	    t = 0;
	}
    }
  while (t & p) t >>= 1;
  return htonl (~t);
}

/* Decode an RFC3397 DNS search order option into a space
   seperated string. Returns length of string (including 
   terminating zero) or zero on error. out may be NULL
   to just determine output length. */
static unsigned int decode_search (const unsigned char *p, int len, char *out)
{
  const unsigned char *r, *q = p;
  unsigned int count = 0, l, hops;

  while (q - p < len)
    {
      r = NULL;
      hops = 0;
      while ((l = *q++))
	{
	  unsigned int label_type = l & 0xc0;
	  if (label_type == 0x80 || label_type == 0x40)
	    return 0;
	  else if (label_type == 0xc0) /* pointer */
	    { 
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
	    }
	  else 
	    {
	      /* straightforward name segment, add with '.' */
	      count += l + 1;
	      if (out)
		{
		  memcpy (out, q, l);
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
static route_t *decodeCSR(const unsigned char *p, int len)
{
  const unsigned char *q = p;
  int cidr;
  int ocets;
  route_t *first;
  route_t *route;

  /* Minimum is 5 -first is CIDR and a router length of 4 */
  if (len < 5)
    return NULL;

  first = xmalloc (sizeof (route_t));
  route = first;

  while (q - p < len)
    {
      memset (route, 0, sizeof (route_t));

      cidr = *q++;
      if (cidr == 0)
	ocets = 0;
      else if (cidr < 9)
	ocets = 1;
      else if (cidr < 17)
	ocets = 2;
      else if (cidr < 25)
	ocets = 3;
      else
	ocets = 4;

      if (ocets > 0)
	{
	  memcpy (&route->destination.s_addr, q, ocets);
	  q += ocets;
	}

      /* Now enter the netmask */
      if (ocets > 0)
	{
	  memset (&route->netmask.s_addr, 255, ocets - 1);
	  memset ((unsigned char *) &route->netmask.s_addr + (ocets - 1),
		  (256 - (1 << (32 - cidr) % 8)), 1);
	}

      /* Finally, snag the router */
      memcpy (&route->gateway.s_addr, q, 4);
      q += 4;

      /* We have another route */
      if (q - p < len)
	{
	  route->next = xmalloc (sizeof (route_t));
	  route = route->next;
	}
    }

  return first;
}

void free_dhcp (dhcp_t *dhcp)
{
  if (! dhcp)
    return;

  if (dhcp->routes)
    free_route (dhcp->routes);

  if (dhcp->hostname)
    free (dhcp->hostname);

  if (dhcp->dnsservers)
    free_address (dhcp->dnsservers);
  if (dhcp->dnsdomain)
    free (dhcp->dnsdomain);
  if (dhcp->dnssearch)
    free (dhcp->dnssearch);

  if (dhcp->ntpservers)
    free_address (dhcp->ntpservers);

  if (dhcp->nisdomain)
    free (dhcp->nisdomain);
  if (dhcp->nisservers)
    free_address (dhcp->nisservers);

  if (dhcp->rootpath)
    free (dhcp->rootpath);

  if (dhcp->fqdn)
    {
      if (dhcp->fqdn->name)
	free (dhcp->fqdn->name);
      free (dhcp->fqdn);
    }
}

static bool dhcp_add_address(address_t **address, const unsigned char *data, int length)
{
  int i;
  address_t *p = *address;

  for (i = 0; i < length; i += 4)
    {
      if (*address == NULL)
	{
	  *address = xmalloc (sizeof (address_t));
	  p = *address;
	}
      else
	{
	  p->next = xmalloc (sizeof (address_t));
	  p = p->next;
	}
      memset (p, 0, sizeof (address_t));
 
      /* Sanity check */
      if (i + 4 > length)
	{
	  logger (LOG_ERR, "invalid address length");
	  return (false);
	}

      memcpy (&p->address.s_addr, data + i, 4);
    }

  return (true);
}

int parse_dhcpmessage (dhcp_t *dhcp, const dhcpmessage_t *message)
{
  const unsigned char *p = message->options;
  const unsigned char *end = p; /* Add size later for gcc-3 issue */
  unsigned char option;
  unsigned char length;
  unsigned int len = 0;
  int i;
  int retval = -1;
  route_t *first_route = xmalloc (sizeof (route_t));
  route_t *route = first_route;
  route_t *last_route = NULL;
  route_t *csr = NULL;

  end += sizeof (message->options);

  memset (first_route, 0, sizeof (route_t));

  dhcp->address.s_addr = message->yiaddr;
  strcpy (dhcp->servername, message->servername);

#define LEN_ERR \
    { \
      logger (LOG_ERR, "invalid length %d for option %d", length, option); \
      p += length; \
      continue; \
    }

  while (p < end)
    {
      option = *p++;
      if (!option)
	continue;

      length = *p++;

      if (p + length >= end)
	{
	  logger (LOG_ERR, "dhcp option exceeds message length");
	  retval = -1;
	  goto eexit;
	}

      switch (option)
	{
	case DHCP_END:
	  goto eexit;

	case DHCP_MESSAGETYPE:
	  retval = (int) *p;
	  p += length;
	  continue;

	default:
	  if (length == 0)
	    {
	      logger (LOG_DEBUG, "option %d has zero length, skipping",
		      option);
	      continue;
	    }
	}

#define LENGTH(_length) \
      if (length != _length) \
      LEN_ERR;
#define MIN_LENGTH(_length) \
      if (length < _length) \
      LEN_ERR;
#define MULT_LENGTH(_mult) \
      if (length % _mult != 0) \
      LEN_ERR;
#define GET_UINT8(_val) \
      LENGTH (sizeof (uint8_t)); \
      memcpy (&_val, p, sizeof (uint8_t));
#define GET_UINT16(_val) \
      LENGTH (sizeof (uint16_t)); \
      memcpy (&_val, p, sizeof (uint16_t));
#define GET_UINT32(_val) \
      LENGTH (sizeof (uint32_t)); \
      memcpy (&_val, p, sizeof (uint32_t));
#define GET_UINT16_H(_val) \
      GET_UINT16 (_val); \
      _val = ntohs (_val);
#define GET_UINT32_H(_val) \
      GET_UINT32 (_val); \
      _val = ntohl (_val);

      switch (option)
	{
	case DHCP_ADDRESS:
	  GET_UINT32 (dhcp->address.s_addr);
	  break;
	case DHCP_NETMASK:
	  GET_UINT32 (dhcp->netmask.s_addr);
	  break;
	case DHCP_BROADCAST:
	  GET_UINT32 (dhcp->broadcast.s_addr);
	  break;
	case DHCP_SERVERIDENTIFIER:
	  GET_UINT32 (dhcp->serveraddress.s_addr);
	  break;
	case DHCP_LEASETIME:
	  GET_UINT32_H (dhcp->leasetime);
	  break;
	case DHCP_RENEWALTIME:
	  GET_UINT32_H (dhcp->renewaltime);
	  break;
	case DHCP_REBINDTIME:
	  GET_UINT32_H (dhcp->rebindtime);
	  break;
	case DHCP_MTU:
	  GET_UINT16_H (dhcp->mtu);
	  /* Minimum legal mtu is 68 accoridng to RFC 2132.
	     In practise it's 576 (minimum maximum message size) */
	  if (dhcp->mtu < MTU_MIN)
	    {
	      logger (LOG_DEBUG, "MTU %d is too low, minium is %d; ignoring", dhcp->mtu, MTU_MIN);
	      dhcp->mtu = 0;
	    }
	  break;

#undef GET_UINT32_H
#undef GET_UINT32
#undef GET_UINT16_H
#undef GET_UINT16
#undef GET_UINT8

#define GETSTR(_var) \
	  MIN_LENGTH (sizeof (char)); \
	  if (_var) free (_var); \
	  _var = xmalloc (length + 1); \
	  memcpy (_var, p, length); \
	  memset (_var + length, 0, 1);
	case DHCP_HOSTNAME:
	  GETSTR (dhcp->hostname);
	  break;
	case DHCP_DNSDOMAIN:
	  GETSTR (dhcp->dnsdomain);
	  break;
	case DHCP_MESSAGE:
	  GETSTR (dhcp->message);
	  break;
	case DHCP_ROOTPATH:
	  GETSTR (dhcp->rootpath);
	  break;
	case DHCP_NISDOMAIN:
	  GETSTR (dhcp->nisdomain);
	  break;
#undef GETSTR

#define GETADDR(_var) \
	  MULT_LENGTH (4); \
	  if (! dhcp_add_address (&_var, p, length)) \
	    { \
	      retval = -1; \
	      goto eexit; \
	    }
	case DHCP_DNSSERVER:
	  GETADDR (dhcp->dnsservers);
	  break;
	case DHCP_NTPSERVER:
	  GETADDR (dhcp->ntpservers);
	  break;
	case DHCP_NISSERVER:
	  GETADDR (dhcp->nisservers);
	  break;
#undef GETADDR

	case DHCP_DNSSEARCH:
	  MIN_LENGTH (1);
	  if (dhcp->dnssearch)
	    free (dhcp->dnssearch);
	  if ((len = decode_search (p, length, NULL)) > 0)
	    {
	      dhcp->dnssearch = xmalloc (len);
	      decode_search (p, length, dhcp->dnssearch);
	    }
	  break;

	case DHCP_CSR:
	  MIN_LENGTH (5);
	  if (csr)
	    free_route (csr);
	  csr = decodeCSR (p, length);
	  break;

	case DHCP_STATICROUTE:
	  MULT_LENGTH (8);
	  for (i = 0; i < length; i += 8)
	    {
	      memcpy (&route->destination.s_addr, p + i, 4);
	      memcpy (&route->gateway.s_addr, p + i + 4, 4);
	      route->netmask.s_addr = getnetmask (route->destination.s_addr); 
	      last_route = route;
	      route->next = xmalloc (sizeof (route_t));
	      route = route->next;
	      memset (route, 0, sizeof (route_t));
	    }
	  break;

	case DHCP_ROUTERS:
	  MULT_LENGTH (4); 
	  for (i = 0; i < length; i += 4)
	    {
	      memcpy (&route->gateway.s_addr, p + i, 4);
	      last_route = route;
	      route->next = xmalloc (sizeof (route_t));
	      route = route->next;
	      memset (route, 0, sizeof (route_t));
	    }
	  break;

#undef LENGTH
#undef MIN_LENGTH
#undef MULT_LENGTH

	default:
	  logger (LOG_DEBUG, "no facility to parse DHCP code %u", option);
	  break;
	}

      p += length;
    }

eexit:
  /* Fill in any missing fields */
  if (! dhcp->netmask.s_addr)
    dhcp->netmask.s_addr = getnetmask (dhcp->address.s_addr);
  if (! dhcp->broadcast.s_addr)
    dhcp->broadcast.s_addr = dhcp->address.s_addr | ~dhcp->netmask.s_addr;

  /* If we have classess static routes then we discard
     static routes and routers according to RFC 3442 */
  if (csr)
    {
      dhcp->routes = csr;
      free_route (first_route); 
    }
  else
    {
      dhcp->routes = first_route;
      if (last_route)
	{
	  free (last_route->next);
	  last_route->next = NULL;
	}
      else
	{
	  free_route (dhcp->routes);
	  dhcp->routes = NULL;
	}
    }

  return retval;
}

