/*
 * Network Service Header
 *
 * Copyright (c) 2017 Red Hat, Inc. -- Jiri Benc <jbenc@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/nsh.h>
#include <net/tun_proto.h>

int ovs_nsh_push(struct sk_buff *skb, const struct nshhdr *pushed_nh)
{
	struct nshhdr *nh;
	size_t length = nsh_hdr_len(pushed_nh);
	u8 next_proto;

	if (skb->mac_len) {
		next_proto = TUN_P_ETHERNET;
	} else {
		next_proto = tun_p_from_eth_p(skb->protocol);
		if (!next_proto)
			return -EAFNOSUPPORT;
	}

	/* Add the NSH header */
	if (skb_cow_head(skb, length) < 0)
		return -ENOMEM;

	skb_push(skb, length);
	nh = (struct nshhdr *)(skb->data);
	memcpy(nh, pushed_nh, length);
	nh->np = next_proto;
	skb_postpush_rcsum(skb, nh, length);

	skb->protocol = htons(ETH_P_NSH);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_mac_len(skb);

	return 0;
}
EXPORT_SYMBOL_GPL(ovs_nsh_push);

int ovs_nsh_pop(struct sk_buff *skb)
{
	struct nshhdr *nh;
	size_t length;
	__be16 inner_proto;

	if (!pskb_may_pull(skb, NSH_BASE_HDR_LEN))
		return -ENOMEM;
	nh = (struct nshhdr *)(skb->data);
	length = nsh_hdr_len(nh);
	inner_proto = tun_p_to_eth_p(nh->np);
	if (!pskb_may_pull(skb, length))
		return -ENOMEM;

	if (!inner_proto)
		return -EAFNOSUPPORT;

	skb_pull_rcsum(skb, length);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_mac_len(skb);
	skb->protocol = inner_proto;

	return 0;
}
EXPORT_SYMBOL_GPL(ovs_nsh_pop);

static struct sk_buff *nsh_gso_segment(struct sk_buff *skb,
				       netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	unsigned int nsh_len, mac_len;
	__be16 proto;
	int nhoff;

	skb_reset_network_header(skb); // 复位skb里面的network_header指针指向网络层ip头的位置

	nhoff = skb->network_header - skb->mac_header; //获取数据链路层帧头的长度
	mac_len = skb->mac_len; // 数据链路层帧的总长度

	if (unlikely(!pskb_may_pull(skb, NSH_BASE_HDR_LEN))) // 判断skb数据缓存里面是否有NSH_BASE_HDR_LEN长度的数据
		goto out;
	nsh_len = nsh_hdr_len(nsh_hdr(skb)); // 获取nsh的整个长度
	if (unlikely(!pskb_may_pull(skb, nsh_len))) // 判断skb缓存是否有足够的数据
		goto out;

	proto = tun_p_to_eth_p(nsh_hdr(skb)->np); // 从nsh头部获取嵌套的协议类型
	if (!proto)
		goto out;

	__skb_pull(skb, nsh_len); // 将skb的data指针向后偏移nsh_len的长度

	skb_reset_mac_header(skb); // 将skb的mac_header指针复位
	skb_reset_mac_len(skb); // 复位链路层长度
	skb->protocol = proto; // 用nsh获取的proto去设置skb的protocol

	features &= NETIF_F_SG;
	segs = skb_mac_gso_segment(skb, features); // 函数主要调用ip层的gso函数，将skb会划分为多段
	if (IS_ERR_OR_NULL(segs)) {
		skb_gso_error_unwind(skb, htons(ETH_P_NSH), nsh_len,
				     skb->network_header - nhoff,
				     mac_len);
		goto out;
	}

	for (skb = segs; skb; skb = skb->next) { // 遍历gso分的多段skb，并根据上面的nsh协议头去填充各段的mac帧头
		skb->protocol = htons(ETH_P_NSH);
		__skb_push(skb, nsh_len);
		skb_set_mac_header(skb, -nhoff);
		skb->network_header = skb->mac_header + mac_len;
		skb->mac_len = mac_len;
	}

out:
	return segs;
}

// GSO(Generic Segmentation Offload)，它比TSO更通用，基本思想就是尽可能的推迟数据分片直至发送到网卡驱动之前，此时会检查网卡是否支持分片功能（如TSO、UFO）,如果支持直接发送到网卡，如果不支持就进行分片后再发往网卡。这样大数据包只需走一次协议栈，而不是被分割成几个数据包分别走，这就提高了效率。
static struct packet_offload nsh_packet_offload __read_mostly = {
	.type = htons(ETH_P_NSH),
	.callbacks = {
		.gso_segment = nsh_gso_segment,  // 发送网络数据包尽可能延迟分段直至网卡驱动，减少多次的协议封装,注册gso的分段回调
	},
};

int ovs_nsh_init(void)
{
    // 此处注册的为网络层协议回调
    // 将nsh_packet_offload 注册到内核的全局数组offload_base里面
	dev_add_offload(&nsh_packet_offload);
	return 0;
}

void ovs_nsh_cleanup(void)
{
	dev_remove_offload(&nsh_packet_offload);
}
