/*
 * (C) 2009 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#ifndef _LIBNETFILTER_CONNTRACK_DCCP_H_
#define _LIBNETFILTER_CONNTRACK_DCCP_H_

#ifdef __cplusplus
extern "C" {
#endif

enum dccp_state {
	DCCP_CONNTRACK_NONE,
	DCCP_CONNTRACK_REQUEST,
	DCCP_CONNTRACK_RESPOND,
	DCCP_CONNTRACK_PARTOPEN,
	DCCP_CONNTRACK_OPEN,
	DCCP_CONNTRACK_CLOSEREQ,
	DCCP_CONNTRACK_CLOSING,
	DCCP_CONNTRACK_TIMEWAIT,
	DCCP_CONNTRACK_IGNORE,
	DCCP_CONNTRACK_INVALID,
	DCCP_CONNTRACK_MAX
};

enum dccp_roles {
	DCCP_CONNTRACK_ROLE_CLIENT,
	DCCP_CONNTRACK_ROLE_SERVER,
	__DCCP_CONNTRACK_ROLE_MAX
};
#define DCCP_ROLE_MAX		(__DCCP_CONNTRACK_ROLE_MAX - 1)

#ifdef __cplusplus
}
#endif

#endif
