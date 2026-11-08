// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2022 Oplus. All rights reserved. */
#include "osi_netlink.h"
#include "osi_base.h"

#define MAX_DATA_LEN 200
static u32 recv_pid;
static bool cpu_netlink_registered;

static bool cpu_monitor_sender_allowed(struct sk_buff *skb)
{
	kuid_t uid = NETLINK_CB(skb).creds.uid;

	return uid_eq(uid, GLOBAL_ROOT_UID) || uid_eq(uid, KUIDT_INIT(1000));
}

#ifndef USE_GEN_NETLINK
static struct sock *netlink_fd;

int send_to_user(int sock_no, size_t len, const int *data)
{
	struct sk_buff *skb;
	struct nlmsghdr *header;
	u32 port = READ_ONCE(recv_pid);
	int payload[MAX_DATA_LEN];
	size_t bytes;

	if (!data || len > MAX_DATA_LEN - SEND_DATA_LEN)
		return -EINVAL;
	if (!netlink_fd || !port)
		return -ENOTCONN;
	payload[0] = sock_no;
	payload[1] = len;
	memcpy(payload + SEND_DATA_LEN, data, len * sizeof(int));
	bytes = (len + SEND_DATA_LEN) * sizeof(int);
	skb = nlmsg_new(bytes, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	header = nlmsg_put(skb, 0, 0, 0, bytes, 0);
	if (!header) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	memcpy(nlmsg_data(header), payload, bytes);
	return netlink_unicast(netlink_fd, skb, port, MSG_DONTWAIT);
}

static void recv_from_user(struct sk_buff *skb)
{
	if (skb && cpu_monitor_sender_allowed(skb))
		WRITE_ONCE(recv_pid, NETLINK_CB(skb).portid);
}

int create_cpu_netlink(int socket_no)
{
	struct netlink_kernel_cfg config = { .input = recv_from_user };

	netlink_fd = netlink_kernel_create(&init_net, socket_no, &config);
	if (!netlink_fd)
		return -ENOMEM;
	cpu_netlink_registered = true;
	return 0;
}

void destroy_cpu_netlink(void)
{
	if (cpu_netlink_registered)
		netlink_kernel_release(netlink_fd);
	cpu_netlink_registered = false;
	netlink_fd = NULL;
	WRITE_ONCE(recv_pid, 0);
}
#else
static struct genl_family oplus_cpuload_genl_family;

int send_to_user(int sock_no, size_t len, const int *data)
{
	struct sk_buff *skb;
	void *header;
	int payload[MAX_DATA_LEN];
	u32 port = READ_ONCE(recv_pid);
	size_t bytes;

	if (!data || len > MAX_DATA_LEN - SEND_DATA_LEN)
		return -EINVAL;
	if (!port)
		return -ENOTCONN;
	payload[0] = sock_no;
	payload[1] = len;
	memcpy(payload + SEND_DATA_LEN, data, len * sizeof(int));
	bytes = (len + SEND_DATA_LEN) * sizeof(int);
	skb = genlmsg_new(nla_total_size(bytes), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &oplus_cpuload_genl_family, 0,
			     OPLUS_CPULOAD_CMD_SEND);
	if (!header || nla_put(skb, OPLUS_CPULOAD_ATTR_MSG_GENL, bytes, payload)) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}
	genlmsg_end(skb, header);
	return genlmsg_unicast(&init_net, skb, port);
}

static int recv_from_user(struct sk_buff *skb, struct genl_info *info)
{
	if (!skb || !info)
		return -EINVAL;
	if (!cpu_monitor_sender_allowed(skb))
		return -EPERM;
	WRITE_ONCE(recv_pid, info->snd_portid);
	return 0;
}

static const struct genl_ops oplus_cpuload_genl_ops[] = {
	{ .cmd = OPLUS_CPULOAD_CMD_RECV, .doit = recv_from_user },
};

static struct genl_family oplus_cpuload_genl_family = {
	.name = OPLUS_CPULOAD_FAMILY_NAME,
	.version = OPLUS_CPULOAD_FAMILY_VER,
	.maxattr = OPLUS_CPULOAD_ATTR_MSG_MAX,
	.ops = oplus_cpuload_genl_ops,
	.n_ops = ARRAY_SIZE(oplus_cpuload_genl_ops),
	.module = THIS_MODULE,
	.resv_start_op = __OPLUS_CPULOAD_CMD_MAX,
};

int create_cpu_netlink(int unused)
{
	int ret = genl_register_family(&oplus_cpuload_genl_family);

	if (!ret)
		cpu_netlink_registered = true;
	return ret;
}

void destroy_cpu_netlink(void)
{
	if (cpu_netlink_registered)
		genl_unregister_family(&oplus_cpuload_genl_family);
	cpu_netlink_registered = false;
	WRITE_ONCE(recv_pid, 0);
}
#endif
