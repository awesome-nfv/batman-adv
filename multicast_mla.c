/*
 * Copyright (C) 2012 B.A.T.M.A.N. contributors:
 *
 * Linus Lüssing
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 *
 */

/* multicast_mla.c - MLA management
 *
 * These functions provide the MultiCast Announcement infrastructure:
 *
 * The MLA infrastructure takes care of announcing any potential multicast
 * listener to any mesh node.
 *
 * Multicast listeners are obtained from either the local batman soft interface
 * (i.e. bat0) or if present its master interface (e.g. a bridge interface).
 * Furthermore if the batman interface is a bridge slave, then multicast
 * listeners behind any other bridge port are obtained from the multicast
 * snooping database of the bridge, too.
 *
 * MLAs are MAC address based. Those addresses are currently distributed via
 * our periodic OGMs.
 *
 * A specific address is only announced if it has at least one matching
 * non-link-local IPv4 multicast address or transient IPv6 multicast address:
 * We on purpose exclude well-known multicast addresses as they are generally
 * of "low" throughput and therefore not feasible for our multicast
 * optimizations targeted at sparse, high throughput multicast streams.
 */

#include "main.h"
#include "hash.h"
#include "hard-interface.h"
#include "originator.h"
#include "translation-table.h"
#include "multicast_flow.h"
#include "multicast_tracker.h"

/* should match batadv_ogm_packet's mcast_num_mla */
#define BATADV_MLA_MAX UINT8_MAX

/**
 * batadv_mcast_mla_len - Size of the MLAs
 * @num_mla:	Number of multicast listener announcements
 *
 * Returns the total size of the multicast annoucement information
 * for a given amount of MLAs.
 */
int batadv_mcast_mla_len(int num_mla)
{
	return num_mla * ETH_ALEN;
}

#ifdef CONFIG_BATMAN_ADV_MCAST_BRIDGE_SNOOP
/**
 * batadv_mcast_mla_br_addr_cpy - Copy a bridge multicast address
 * @dst:	Destination to write to - a multicast MAC address
 * @src:	Source to read from - a multicast IP address
 *
 * This converts a given multicast IPv4/IPv6 address from a bridge
 * to its matching multicast MAC address and copies it into the given
 * destination buffer.
 *
 * Caller needs to make sure the destination buffer can hold
 * at least ETH_ALEN bytes.
 */
static void batadv_mcast_mla_br_addr_cpy(char *dst, const struct br_ip *src)
{
	if (src->proto == htons(ETH_P_IP)) {
		/* RFC 1112 */
		memcpy(dst, "\x01\x00\x5e", 3);
		memcpy(dst + 3, ((char *)&src->u.ip4) + 1, ETH_ALEN - 3);
		dst[3] &= 0x7F;
	}
#if IS_ENABLED(CONFIG_IPV6)
	else if (src->proto == htons(ETH_P_IPV6)) {
		/* RFC 2464 */
		memcpy(dst, "\x33\x33", 2);
		memcpy(dst + 2, &src->u.ip6.s6_addr32[3],
		       sizeof(src->u.ip6.s6_addr32[3]));
	}
#endif
	else
		memset(dst, 0, ETH_ALEN);
}
#endif

/**
 * batadv_mcast_mla_has_transient_ipv6 - Checks for transient IPv6 multicast
 * @addr:	MAC address to check
 * @dev:	Device which we search for
 *		matching, transient IPv6 multicast addresses on
 *
 * This checks whether for the given MAC address and interface
 * at least one matching, transient IPv6 multicast address exists.
 *
 * Returns 1 if this check passes, 0 otherwise.
 */
static int batadv_mcast_mla_has_transient_ipv6(uint8_t *addr,
					       struct net_device *dev)
{
	struct inet6_dev *idev;
	struct ifmcaddr6 *mc;
	uint8_t buf[ETH_ALEN];
	int ret = 0;

	rcu_read_lock();
	idev = __in6_dev_get(dev);
	if (!idev)
		goto unlock;

	read_lock_bh(&idev->lock);
	for (mc = idev->mc_list; mc; mc = mc->next) {
		ipv6_eth_mc_map(&mc->mca_addr, buf);
		if (memcmp(addr, buf, ETH_ALEN))
			continue;

		if (IPV6_ADDR_MC_FLAG_TRANSIENT(&mc->mca_addr)) {
			ret = 1;
			break;
		}
	}
	read_unlock_bh(&idev->lock);

unlock:
	rcu_read_unlock();
	return ret;
}

/**
 * batadv_mcast_mla_has_non_ll_ipv4 - Checks for non-link-local IPv4 multicast
 * @addr:	MAC address to check
 * @dev:	Device which we search for
 *		matching, non-link-local IPv4 multicast addresses on
 *
 * This checks whether for the given MAC address and interface
 * at least one matching, non-link-local IPv4 multicast address exists.
 *
 * Returns 1 if this check passes, 0 otherwise.
 */
static int batadv_mcast_mla_has_non_ll_ipv4(uint8_t *addr,
					    struct net_device *dev)
{
	struct in_device *idev;
	struct ip_mc_list *im;
	uint8_t buf[ETH_ALEN];
	int ret = 0;

	rcu_read_lock();
	idev = __in_dev_get_rcu(dev);
	if (!idev)
		goto unlock;

	for_each_pmc_rcu(idev, im) {
		ip_eth_mc_map(im->multiaddr, buf);
		if (memcmp(addr, buf, ETH_ALEN))
			continue;

		if (ipv4_is_local_multicast(im->multiaddr))
			continue;

		ret = 1;
		break;
	}

unlock:
	rcu_read_unlock();
	return ret;
}

/**
 * batadv_mcast_mla_has_unspecial_addr - Checks for "unspecial" multicast IPs
 * @addr:	MAC address to check
 * @dev:	Device which we search for
 *		matching, "unspecial" IP addresses on
 *
 * This checks whether for the given MAC address and interface
 * at least one matching, "unspecial" multicast IP address exists.
 *
 * For IPv6 (MAC: 33:33:...) "unspecial" means:
 * - a transient IPv6 address
 * For IPv4 (MAC: 01:00:5E:...) "unspecial" means:
 * - a non-link-local IPv4 address
 *
 * These two categories of addresses are (and should always be)
 * the same ones as the bridge multicast snooping is using.
 *
 * Returns 1 if this check passes, 0 otherwise.
 */
static int batadv_mcast_mla_has_unspecial_addr(uint8_t *addr,
					       struct net_device *dev)
{
	if (!memcmp(addr, "\x33\x33", 2))
		return batadv_mcast_mla_has_transient_ipv6(addr, dev);
	else if (!memcmp(addr, "\x01\x00\x5E", 3))
		return batadv_mcast_mla_has_non_ll_ipv4(addr, dev);
	else
		return 0;
}

/**
 * batadv_mcast_mla_get_master - Get a reference to a netdevice master
 * @dev:	The netdevice to get a reference for
 *
 * Returns the master interface (e.g. a bridge device) and increases its
 * reference counter. Returns NULL if the given netdevice does not have
 * any master.
 *
 * Caller needs to hold the rtnl_lock().
 */
static inline struct net_device *batadv_mcast_mla_get_master(
							struct net_device *dev)
{
	ASSERT_RTNL();
	if (dev->master) {
		dev_hold(dev->master);
		return dev->master;
	} else {
		return NULL;
	}
}

/**
 * batadv_mcast_mla_master_free_ref - Releases a reference from a netdevice
 * @dev:	The device to release
 *
 * Releases a reference from the given netdevice.
 */
static inline void batadv_mcast_mla_master_free_ref(struct net_device *dev)
{
	dev_put(dev);
}

/**
 * batadv_mcast_mla_local_collect - Collects local multicast listeners
 * @dev:		The device to collect multicast addresses from
 * @mcast_list:		A list to put found addresses into
 * @num_mla_max:	The maximum number of items to add
 *
 * Collects up to num_mla_max multicast addresses of the local multicast
 * listeners on the given interface, dev, in the given mcast_list.
 *
 * If the given interface is a slave of another one (e.g. a bridge interface)
 * then multicast listeners will be collected from that device instead.
 *
 * Returns -ENOMEM on memory allocation error or the number of
 * items added to the mcast_list otherwise.
 */
static int batadv_mcast_mla_local_collect(struct net_device *dev,
					  struct list_head *mcast_list,
					  int num_mla_max) {
	struct net_device *master;
	struct netdev_hw_addr *mc_list_entry, *new;
	int num_mla = 0, ret = 0;

	rtnl_lock();
	master = batadv_mcast_mla_get_master(dev);
	rtnl_unlock();

	/* if our soft interface is part of a master (e.g. a bridge)
	 * then let's use that one instead */
	dev = master ? master : dev;

	netif_addr_lock_bh(dev);
	netdev_for_each_mc_addr(mc_list_entry, dev) {
		if (num_mla >= num_mla_max) {
			pr_warn("Too many local multicast listener announcements here, just adding %i\n",
				num_mla_max);
			break;
		}

		if (!batadv_mcast_mla_has_unspecial_addr(mc_list_entry->addr,
							 dev))
			continue;

		new = kmalloc(sizeof(struct netdev_hw_addr), GFP_ATOMIC);
		if (!new) {
			ret = -ENOMEM;
			break;
		}

		memcpy(&new->addr, &mc_list_entry->addr, ETH_ALEN);
		list_add(&new->list, mcast_list);
		num_mla++;
	}
	netif_addr_unlock_bh(dev);

	if (master)
		batadv_mcast_mla_master_free_ref(master);

	return ret < 0 ? ret : num_mla;
}

/**
 * batadv_mcast_mla_is_duplicate - Checks whether an address is in a list
 * @mcast_addr:	The multicast address to check
 * @mcast_list:	The list with multicast addresses to search in
 *
 * Returns true if the given address is already in the given list.
 * Otherwise returns false.
 */
static inline bool batadv_mcast_mla_is_duplicate(uint8_t *mcast_addr,
						 struct list_head *mcast_list)
{
	struct netdev_hw_addr *mcast_entry;

	list_for_each_entry(mcast_entry, mcast_list, list)
		if (!memcmp(mcast_entry->addr, mcast_addr, ETH_ALEN))
			return true;

	return false;
}

#ifdef CONFIG_BATMAN_ADV_MCAST_BRIDGE_SNOOP
/**
 * batadv_mcast_mla_bridge_collect - Collects bridged-in multicast listeners
 * @soft_iface:		The slave interface of the bridge we search in
 * @mcast_list:		A list to put found addresses into
 * @num_mla_max:	The maximum number of items to add
 *
 * Collects up to num_mla_max multicast addresses of snooped multicast
 * listeners from any bridge slave of the bridge of the given soft interface,
 * except from the given soft_iface in the given mcast_list.
 *
 * Returns -ENOMEM on memory allocation error or the number of
 * items added to the mcast_list otherwise.
 */
static int batadv_mcast_mla_bridge_collect(struct net_device *soft_iface,
					   struct list_head *mcast_list,
					   int num_mla_max)
{
	struct list_head bridge_mcast_list;
	struct br_ip_list *br_ip_entry, *tmp;
	struct netdev_hw_addr *new;
	uint8_t mcast_addr[ETH_ALEN];
	int num_mla = 0, ret = 0;

	INIT_LIST_HEAD(&bridge_mcast_list);

	ret = br_multicast_list_adjacent(soft_iface, &bridge_mcast_list);
	if (ret < 0)
		goto out;

	list_for_each_entry(br_ip_entry, &bridge_mcast_list, list) {
		if (num_mla >= num_mla_max) {
			pr_warn("Too many local+bridge multicast listener announcements here, just adding %i\n",
				num_mla_max);
			break;
		}

		batadv_mcast_mla_br_addr_cpy(mcast_addr, &br_ip_entry->addr);
		if (batadv_mcast_mla_is_duplicate(mcast_addr, mcast_list))
			continue;

		new = kmalloc(sizeof(struct netdev_hw_addr), GFP_ATOMIC);
		if (!new) {
			ret = -ENOMEM;
			break;
		}

		memcpy(&new->addr, mcast_addr, ETH_ALEN);
		list_add(&new->list, mcast_list);
		num_mla++;
	}

out:
	list_for_each_entry_safe(br_ip_entry, tmp, &bridge_mcast_list, list) {
		list_del(&br_ip_entry->list);
		kfree(br_ip_entry);
	}

	return ret < 0 ? ret : num_mla;
}
#endif

/**
 * batadv_mcast_mla_collect_free - Frees a list of multicast addresses
 * @mcast_list:		The list to free
 *
 * Removes and frees all items in the given mcast_list.
 */
void batadv_mcast_mla_collect_free(struct list_head *mcast_list)
{
	struct netdev_hw_addr *mcast_entry, *tmp;

	list_for_each_entry_safe(mcast_entry, tmp, mcast_list, list) {
		list_del(&mcast_entry->list);
		kfree(mcast_entry);
	}
}

static void batadv_mcast_mla_tt_clean(struct batadv_priv *bat_priv,
				      struct list_head *mcast_list)
{
	struct netdev_hw_addr *mcast_entry, *tmp;

	list_for_each_entry_safe(mcast_entry, tmp, &bat_priv->mcast.mla_list, list) {
		if (batadv_mcast_mla_is_duplicate(mcast_entry->addr, mcast_list))
			continue;

		batadv_tt_local_remove(bat_priv, mcast_entry->addr,
				       "mcast TT outdated", 0);

		list_del(&mcast_entry->list);
		kfree(mcast_entry);
	}
}

static void batadv_mcast_mla_tt_add(struct net_device *soft_iface,
				    struct list_head *mcast_list)
{
	struct netdev_hw_addr *mcast_entry, *tmp;
	struct batadv_priv *bat_priv = netdev_priv(soft_iface);

	list_for_each_entry_safe(mcast_entry, tmp, mcast_list, list) {
		if (batadv_mcast_mla_is_duplicate(mcast_entry->addr,
						  &bat_priv->mcast.mla_list))
			continue;

		batadv_tt_local_add(soft_iface, mcast_entry->addr,
				    BATADV_NULL_IFINDEX);
		list_move_tail(&mcast_entry->list, &bat_priv->mcast.mla_list);
	}
}

void batadv_mcast_mla_tt_update(struct batadv_priv *bat_priv)
{
	struct batadv_hard_iface *primary_if;
	struct net_device *soft_iface;
	struct list_head mcast_list;
	int ret;

	INIT_LIST_HEAD(&mcast_list);

	primary_if = batadv_primary_if_get_selected(bat_priv);
	if (!primary_if)
		goto out;

	soft_iface = primary_if->soft_iface;

	/* Avoid attaching MLAs, if multicast optimization is disabled */
	if (!atomic_read(&bat_priv->mcast_group_awareness))
		goto update;

	ret = batadv_mcast_mla_local_collect(soft_iface, &mcast_list,
					     BATADV_MLA_MAX);
	if (ret < 0)
		goto out;

#ifdef CONFIG_BATMAN_ADV_MCAST_BRIDGE_SNOOP
	ret = batadv_mcast_mla_bridge_collect(soft_iface, &mcast_list,
					      BATADV_MLA_MAX - num_mla);
	if (ret < 0)
		goto out;
#endif

update:
	batadv_mcast_mla_tt_clean(bat_priv, &mcast_list);
	batadv_mcast_mla_tt_add(soft_iface, &mcast_list);

out:
	if (primary_if)
		batadv_hardif_free_ref(primary_if);

	batadv_mcast_mla_collect_free(&mcast_list);
}

int batadv_mcast_mla_local_seq_print_text(struct seq_file *seq, void *offset)
{
	struct net_device *net_dev = (struct net_device *)seq->private;
	struct net_device *master;
	struct netdev_hw_addr *mc_list_entry;

	rtnl_lock();
	master = batadv_mcast_mla_get_master(net_dev);
	rtnl_unlock();

	seq_printf(seq,
		   "Locally retrieved multicast listener announcements (from %s%s%s):\n",
		   master ? master->name : net_dev->name,
		   master ? ", master of " : "",
		   master ? net_dev->name : "");

	netif_addr_lock_bh(master ? master : net_dev);
	netdev_for_each_mc_addr(mc_list_entry, master ? master : net_dev) {
		if (!batadv_mcast_mla_has_unspecial_addr(
						mc_list_entry->addr,
						master ? master : net_dev))
			continue;

		seq_printf(seq, "%pM\n", mc_list_entry->addr);
	}
	netif_addr_unlock_bh(master ? master : net_dev);

	if (master)
		batadv_mcast_mla_master_free_ref(master);

	return 0;
}

#ifdef CONFIG_BATMAN_ADV_MCAST_BRIDGE_SNOOP
int batadv_mcast_mla_bridge_seq_print_text(struct seq_file *seq, void *offset)
{
	struct net_device *net_dev = (struct net_device *)seq->private;
	struct list_head bridge_mc_list;
	struct br_ip_list *br_ip_entry, *tmp;
	uint8_t buff[ETH_ALEN];

	INIT_LIST_HEAD(&bridge_mc_list);
	br_multicast_list_adjacent(net_dev, &bridge_mc_list);

	seq_printf(seq,
		   "Bridge snooped multicast listener announcements (from %s):\n",
		   net_dev->name);

	list_for_each_entry_safe(br_ip_entry, tmp, &bridge_mc_list, list) {
		batadv_mcast_mla_br_addr_cpy(buff, &br_ip_entry->addr);

		seq_printf(seq, "%pM\n", buff);

		list_del(&br_ip_entry->list);
		kfree(br_ip_entry);
	}

	return 0;
}
#endif