#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#include <net/ip.h>
#include <net/tcp.h>

#define FRAG_SIZE 150
#define TLS_DEFAULT_PORTNUM 443

struct tlshdr {
	u8 protocol;
#define TLSPROTO_HANDSHAKE 22
	u16 version;
	u16 len;
	/* The protocol messages start here. */
} __attribute__((packed));

struct tls_hs_msghdr {
	u32 type : 8,
#define TLS_HS_MSGTYPE_CLIENTHELLO 1
		len : 24;
} __attribute__((packed));

static void ip_copy_metadata(struct sk_buff *to, struct sk_buff *from)
{
	to->pkt_type = from->pkt_type;
	to->priority = from->priority;
	to->protocol = from->protocol;
	skb_dst_drop(to);
	skb_dst_copy(to, from);
	to->dev = from->dev;
	to->mark = from->mark;

	skb_copy_hash(to, from);

	/* Copy the flags to each fragment. */
	IPCB(to)->flags = IPCB(from)->flags;

#ifdef CONFIG_NET_SCHED
	to->tc_index = from->tc_index;
#endif
	nf_copy(to, from);
#if IS_ENABLED(CONFIG_IP_VS)
	to->ipvs_property = from->ipvs_property;
#endif
	skb_copy_secmark(to, from);
}

static struct sk_buff *ip_tcp_frag(struct net *net, struct sock *sk, struct sk_buff *from,
		unsigned int begin, unsigned int end)
{
	struct sk_buff *skb;
	struct iphdr *iph = ip_hdr(from);
	struct tcphdr *tcph = (struct tcphdr *)skb_transport_header(from);
	unsigned int iph_l, tcph_l, ll_rs, payload_l, len;
	int ptr;
	struct rtable *rt = skb_rtable(from);
	__u32 seq, ack_seq;
	
	iph_l = iph->ihl << 2;
	tcph_l = tcph->doff << 2;
	
	payload_l = ntohs(iph->tot_len) - iph_l - tcph_l;

	seq = ntohl(tcph->seq);
	ack_seq = ntohl(tcph->ack_seq);

	ptr = iph_l + tcph_l;
	len = end - begin;
	ll_rs = LL_RESERVED_SPACE(rt->dst.dev);

	skb = alloc_skb(iph_l + tcph_l + len + ll_rs, GFP_ATOMIC);
	if (!skb)
		return 0;

	ip_copy_metadata(skb, from);
	skb_reserve(skb, ll_rs);
	skb_put(skb, iph_l + tcph_l + len);
	skb_reset_network_header(skb);
	skb->transport_header = skb->network_header + iph_l;

	if (from->sk)
		skb_set_owner_w(skb, from->sk);

	skb_copy_from_linear_data(from, skb_network_header(skb), iph_l + tcph_l);
	if (skb_copy_bits(from, ptr + begin, skb_transport_header(skb) + tcph_l, len))
		BUG();

	tcph = (struct tcphdr *)skb_transport_header(skb);
	tcph->seq = htonl(seq + begin);
	tcph->ack_seq = htonl(ack_seq);

	iph = ip_hdr(skb);
	iph->tot_len = htons(iph_l + tcph_l + len);
	
	tcph->check = 0;
	skb->csum = csum_partial((unsigned char *)tcph, tcph_l + len, 0);
	tcph->check = tcp_v4_check(
			tcph_l + len,
			iph->saddr,
			iph->daddr,
			skb->csum);
	ip_send_check(iph);
/*
	printk("stealthhello: ori.seq: %u, ori.ack_seq: %u, seq: %u, ack_seq: %u\n",
			ntohl(seq), ntohl(ack_seq), ntohl(tcph->seq), ntohl(tcph->ack_seq));
*/
	return skb;
}

static unsigned int stealth_hello(struct sk_buff *skb, const struct nf_hook_state *state)
{
	struct iphdr *iph;
	unsigned int iph_l, tot_l, tcph_l, mtu, ptr;
	struct sk_buff *frag_skb;
	int err;

	if (skb->ip_summed == CHECKSUM_PARTIAL &&
			(err = skb_checksum_help(skb)))
		goto fail;

	iph = ip_hdr(skb);
	iph_l = iph->ihl << 2;
	tot_l = ntohs(iph->tot_len);
	tcph_l = tcp_hdrlen(skb);


	mtu = ip_skb_dst_mtu(state->sk, skb);
	if (IPCB(skb)->frag_max_size && IPCB(skb)->frag_max_size < mtu)
		mtu = IPCB(skb)->frag_max_size;

	if (mtu <= FRAG_SIZE || tot_l <= FRAG_SIZE)
		goto fail;

	ptr = FRAG_SIZE - iph_l - tcph_l;
	frag_skb = ip_tcp_frag(state->net, state->sk, skb, 0, ptr);
	if (!frag_skb)
		goto fail;


	err = state->okfn(state->net, state->sk, frag_skb);
	if (err)
		goto fail;

	IP_INC_STATS(state->net, IPSTATS_MIB_FRAGCREATES);

	frag_skb = ip_tcp_frag(state->net, state->sk, skb, ptr, tot_l - iph_l - tcph_l);
	if (!frag_skb)
		goto fail;

	err = state->okfn(state->net, state->sk, frag_skb);
	if (err)
		goto fail;

	IP_INC_STATS(state->net, IPSTATS_MIB_FRAGOKS);
	consume_skb(skb);
	return NF_STOLEN;

fail:
	printk("stealthhello: failed\n");
	IP_INC_STATS(state->net, IPSTATS_MIB_FRAGFAILS);
	return NF_ACCEPT;
}

static unsigned int sh_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
	struct iphdr *iph;
	int iph_l;
	int tot_l;
	struct tcphdr *tcph;
	int tcph_l;
	struct tlshdr *tlsh;
	int hdr_l;
	int tls_msgs_l;
	struct tls_hs_msghdr *msgh;

	iph = (struct iphdr *)skb_network_header(skb);
	iph_l = iph->ihl << 2;
	tot_l = ntohs(iph->tot_len);

	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	tcph = (struct tcphdr *)skb_transport_header(skb);
	tcph_l = tcph->doff << 2;

	hdr_l = iph_l + tcph_l + sizeof(struct tlshdr);

	if (ntohs(tcph->dest) != TLS_DEFAULT_PORTNUM || tot_l < hdr_l)
		return NF_ACCEPT;

	tlsh = (struct tlshdr *)((u8 *)tcph + tcph_l);
	tls_msgs_l = ntohs(tlsh->len);
	if (tot_l < hdr_l + tls_msgs_l)
		return NF_ACCEPT;

	if (tlsh->protocol != TLSPROTO_HANDSHAKE)
		return NF_ACCEPT;

	msgh = (struct tls_hs_msghdr *)((u8 *)tlsh + sizeof(struct tlshdr));
	while (tls_msgs_l > 0) {
		int msg_l;

		if (msgh->type == TLS_HS_MSGTYPE_CLIENTHELLO) {
			/*
			printk("stealthhello: %pI4 --> %pI4\n", &iph->saddr, &iph->daddr);
			*/
			return stealth_hello(skb, state);
		}

		msg_l = ntohl(msgh->len << 8);
		tls_msgs_l -= msg_l;
		msgh = (struct tls_hs_msghdr *)((u8 *)msgh + msg_l);
	}

	return NF_ACCEPT;
}

static struct nf_hook_ops sh = {
	.hook = sh_hook,
	.pf = NFPROTO_IPV4,
	.hooknum = NF_INET_POST_ROUTING,
	.priority = NF_IP_PRI_MANGLE
};

int init_module(void)
{
	return nf_register_net_hook(&init_net, &sh);
}

void cleanup_module(void)
{
	nf_unregister_net_hook(&init_net, &sh);
}

MODULE_LICENSE("GPL");