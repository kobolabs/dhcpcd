/*
 * dhcpcd - DHCP client daemon -
 * Copyright 2005 - 2007 Roy Marples <roy@marples.name>
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

#ifndef IPV4LL_H
#define IPV4LL_H

#ifdef ENABLE_IPV4LL

#include "dhcp.h"
#include "interface.h"

int ipv4ll_get_address (interface_t *iface, dhcp_t *dhcp);

#endif
#endif
