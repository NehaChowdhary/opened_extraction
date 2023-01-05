/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 * This is main balancer's application code
 */

#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "balancer_consts.h"
#include "balancer_helpers.h"
#include "balancer_maps.h"
#include "balancer_structs.h"
#include "bpf.h"
#include "bpf_helpers.h"
#include "handle_icmp.h"
#include "jhash.h"
#include "pckt_encap.h"
#include "pckt_parsing.h"

__attribute__((__always_inline__)) 
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 25,
 Endline: 50,
 Funcname: is_under_flood,
 Input: (__u64 *cur_time),
 Output: bool,
 Helpers: [bpf_map_lookup_elem,bpf_ktime_get_ns,],
 Read_maps: [ stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
	static inline bool is_under_flood(
    __u64* cur_time) {
  __u32 conn_rate_key = MAX_VIPS + NEW_CONN_RATE_CNTR;
  struct lb_stats* conn_rate_stats =
      bpf_map_lookup_elem(&stats, &conn_rate_key);
  if (!conn_rate_stats) {
    return true;
  }
  *cur_time = bpf_ktime_get_ns();
  // we are going to check that new connections rate is less than predefined
  // value; conn_rate_stats.v1 contains number of new connections for the last
  // second, v2 - when last time quanta started.
  if ((*cur_time - conn_rate_stats->v2) > ONE_SEC) {
    // new time quanta; reseting counters
    conn_rate_stats->v1 = 1;
    conn_rate_stats->v2 = *cur_time;
  } else {
    conn_rate_stats->v1 += 1;
    if (conn_rate_stats->v1 > MAX_CONN_RATE) {
      // we are exceding max connections rate. bypasing lru update and
      // source routing lookup
      return true;
    }
  }
  return false;
}

__attribute__((__always_inline__)) 
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 53,
 Endline: 131,
 Funcname: get_packet_dst,
 Input: (struct real_definition **real, struct packet_description *pckt, struct vip_meta *vip_info, bool is_ipv6, void *lru_map),
 Output: bool,
 Helpers: [bpf_map_lookup_elem,bpf_map_update_elem,],
 Read_maps: [  ch_rings,  lpm_src_v6,  lpm_src_v4, reals, stats,],
 Update_maps: [ lru_map,],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
	static inline bool get_packet_dst(
    struct real_definition** real,
    struct packet_description* pckt,
    struct vip_meta* vip_info,
    bool is_ipv6,
    void* lru_map) {
  // to update lru w/ new connection
  struct real_pos_lru new_dst_lru = {};
  bool under_flood = false;
  bool src_found = false;
  __u32* real_pos;
  __u64 cur_time = 0;
  __u32 hash;
  __u32 key;

  under_flood = is_under_flood(&cur_time);

#ifdef LPM_SRC_LOOKUP
  if ((vip_info->flags & F_SRC_ROUTING) && !under_flood) {
    __u32* lpm_val;
    if (is_ipv6) {
      struct v6_lpm_key lpm_key_v6 = {};
      lpm_key_v6.prefixlen = 128;
      memcpy(lpm_key_v6.addr, pckt->flow.srcv6, 16);
      lpm_val = bpf_map_lookup_elem(&lpm_src_v6, &lpm_key_v6);
    } else {
      struct v4_lpm_key lpm_key_v4 = {};
      lpm_key_v4.addr = pckt->flow.src;
      lpm_key_v4.prefixlen = 32;
      lpm_val = bpf_map_lookup_elem(&lpm_src_v4, &lpm_key_v4);
    }
    if (lpm_val) {
      src_found = true;
      key = *lpm_val;
    }
    __u32 stats_key = MAX_VIPS + LPM_SRC_CNTRS;
    struct lb_stats* data_stats = bpf_map_lookup_elem(&stats, &stats_key);
    if (data_stats) {
      if (src_found) {
        data_stats->v2 += 1;
      } else {
        data_stats->v1 += 1;
      }
    }
  }
#endif
  if (!src_found) {
    bool hash_16bytes = is_ipv6;

    if (vip_info->flags & F_HASH_DPORT_ONLY) {
      // service which only use dst port for hash calculation
      // e.g. if packets has same dst port -> they will go to the same real.
      // usually VoIP related services.
      pckt->flow.port16[0] = pckt->flow.port16[1];
      memset(pckt->flow.srcv6, 0, 16);
    }
    hash = get_packet_hash(pckt, hash_16bytes) % RING_SIZE;
    key = RING_SIZE * (vip_info->vip_num) + hash;

    real_pos = bpf_map_lookup_elem(&ch_rings, &key);
    if (!real_pos) {
      return false;
    }
    key = *real_pos;
  }
  pckt->real_index = key;
  *real = bpf_map_lookup_elem(&reals, &key);
  if (!(*real)) {
    return false;
  }
  if (lru_map && !(vip_info->flags & F_LRU_BYPASS) && !under_flood) {
    if (pckt->flow.proto == IPPROTO_UDP) {
      new_dst_lru.atime = cur_time;
    }
    new_dst_lru.pos = key;
    bpf_map_update_elem(lru_map, &pckt->flow, &new_dst_lru, BPF_ANY);
  }
  return true;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 133,
 Endline: 156,
 Funcname: connection_table_lookup,
 Input: (struct real_definition **real, struct packet_description *pckt, void *lru_map, bool isGlobalLru),
 Output: void,
 Helpers: [bpf_map_lookup_elem,bpf_ktime_get_ns,],
 Read_maps: [ reals, lru_map,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline void connection_table_lookup(
    struct real_definition** real,
    struct packet_description* pckt,
    void* lru_map,
    bool isGlobalLru) {
  struct real_pos_lru* dst_lru;
  __u64 cur_time;
  __u32 key;
  dst_lru = bpf_map_lookup_elem(lru_map, &pckt->flow);
  if (!dst_lru) {
    return;
  }
  if (!isGlobalLru && pckt->flow.proto == IPPROTO_UDP) {
    cur_time = bpf_ktime_get_ns();
    if (cur_time - dst_lru->atime > LRU_UDP_TIMEOUT) {
      return;
    }
    dst_lru->atime = cur_time;
  }
  key = dst_lru->pos;
  pckt->real_index = key;
  *real = bpf_map_lookup_elem(&reals, &key);
  return;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 158,
 Endline: 230,
 Funcname: process_l3_headers,
 Input: (struct packet_description *pckt, __u8 *protocol, __u64 off, __u16 *pkt_bytes, void *data, void *data_end, bool is_ipv6),
 Output: int,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int process_l3_headers(
    struct packet_description* pckt,
    __u8* protocol,
    __u64 off,
    __u16* pkt_bytes,
    void* data,
    void* data_end,
    bool is_ipv6) {
  __u64 iph_len;
  int action;
  struct iphdr* iph;
  struct ipv6hdr* ip6h;
  if (is_ipv6) {
    ip6h = data + off;
    if (ip6h + 1 > data_end) {
      return XDP_DROP;
    }

    iph_len = sizeof(struct ipv6hdr);
    *protocol = ip6h->nexthdr;
    pckt->flow.proto = *protocol;

    // copy tos from the packet
    pckt->tos = (ip6h->priority << 4) & 0xF0;
    pckt->tos = pckt->tos | ((ip6h->flow_lbl[0] >> 4) & 0x0F);

    *pkt_bytes = bpf_ntohs(ip6h->payload_len);
    off += iph_len;
    if (*protocol == IPPROTO_FRAGMENT) {
      // we drop fragmented packets
      return XDP_DROP;
    } else if (*protocol == IPPROTO_ICMPV6) {
      action = parse_icmpv6(data, data_end, off, pckt);
      if (action >= 0) {
        return action;
      }
    } else {
      memcpy(pckt->flow.srcv6, ip6h->saddr.s6_addr32, 16);
      memcpy(pckt->flow.dstv6, ip6h->daddr.s6_addr32, 16);
    }
  } else {
    iph = data + off;
    if (iph + 1 > data_end) {
      return XDP_DROP;
    }
    // ihl contains len of ipv4 header in 32bit words
    if (iph->ihl != 5) {
      // if len of ipv4 hdr is not equal to 20bytes that means that header
      // contains ip options, and we dont support em
      return XDP_DROP;
    }
    pckt->tos = iph->tos;
    *protocol = iph->protocol;
    pckt->flow.proto = *protocol;
    *pkt_bytes = bpf_ntohs(iph->tot_len);
    off += IPV4_HDR_LEN_NO_OPT;

    if (iph->frag_off & PCKT_FRAGMENTED) {
      // we drop fragmented packets.
      return XDP_DROP;
    }
    if (*protocol == IPPROTO_ICMP) {
      action = parse_icmp(data, data_end, off, pckt);
      if (action >= 0) {
        return action;
      }
    } else {
      pckt->flow.src = iph->saddr;
      pckt->flow.dst = iph->daddr;
    }
  }
  return FURTHER_PROCESSING;
}

#ifdef INLINE_DECAP_GENERIC
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 233,
 Endline: 255,
 Funcname: check_decap_dst,
 Input: (struct packet_description *pckt, bool is_ipv6, bool *pass),
 Output: int,
 Helpers: [bpf_map_lookup_elem,],
 Read_maps: [ decap_dst,  stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int
check_decap_dst(struct packet_description* pckt, bool is_ipv6, bool* pass) {
  struct address dst_addr = {};
  struct lb_stats* data_stats;

  if (is_ipv6) {
    memcpy(dst_addr.addrv6, pckt->flow.dstv6, 16);
  } else {
    dst_addr.addr = pckt->flow.dst;
  }
  __u32* decap_dst_flags = bpf_map_lookup_elem(&decap_dst, &dst_addr);

  if (decap_dst_flags) {
    *pass = false;
    __u32 stats_key = MAX_VIPS + REMOTE_ENCAP_CNTRS;
    data_stats = bpf_map_lookup_elem(&stats, &stats_key);
    if (!data_stats) {
      return XDP_DROP;
    }
    data_stats->v1 += 1;
  }
  return FURTHER_PROCESSING;
}

#endif // of INLINE_DECAP_GENERIC

#ifdef GLOBAL_LRU_LOOKUP

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 261,
 Endline: 277,
 Funcname: reals_have_same_addr,
 Input: (struct real_definition *a, struct real_definition *b),
 Output: bool,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline bool reals_have_same_addr(
    struct real_definition* a,
    struct real_definition* b) {
  if (a->flags != b->flags) {
    return false;
  }
  if (a->flags & F_IPV6) {
    for (int i = 0; i < 4; i++) {
      if (a->dstv6[i] != b->dstv6[i]) {
        return false;
      }
      return true;
    }
  } else {
    return a->dst == b->dst;
  }
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 279,
 Endline: 335,
 Funcname: perform_global_lru_lookup,
 Input: (struct real_definition **dst, struct packet_description *pckt, __u32 cpu_num, struct vip_meta *vip_info, bool is_ipv6),
 Output: int,
 Helpers: [bpf_map_lookup_elem,],
 Read_maps: [ stats, global_lru_maps,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int perform_global_lru_lookup(
    struct real_definition** dst,
    struct packet_description* pckt,
    __u32 cpu_num,
    struct vip_meta* vip_info,
    bool is_ipv6) {
  // lookup in the global cache
  void* g_lru_map = bpf_map_lookup_elem(&global_lru_maps, &cpu_num);
  __u32 global_lru_stats_key = MAX_VIPS + GLOBAL_LRU_CNTR;

  struct lb_stats* global_lru_stats =
      bpf_map_lookup_elem(&stats, &global_lru_stats_key);
  if (!global_lru_stats) {
    return XDP_DROP;
  }

  if (!g_lru_map) {
    // We were not able to retrieve the global lru for this cpu.
    // This counter should never be anything except 0 in prod.
    // We are going to use it for monitoring.
    global_lru_stats->v1 += 1; // global lru map doesn't exist for this cpu
    g_lru_map = &fallback_glru;
  }

  connection_table_lookup(dst, pckt, g_lru_map, /*isGlobalLru=*/true);
  if (*dst) {
    global_lru_stats->v2 += 1; // we routed a flow using global lru

    // Find the real that we route the packet to if we use consistent hashing
    struct real_definition* dst_consistent_hash = NULL;
    if (get_packet_dst(
            &dst_consistent_hash,
            pckt,
            vip_info,
            is_ipv6,
            /*lru_map=*/NULL)) {
      __u32 global_lru_mismatch_stats_key = MAX_VIPS + GLOBAL_LRU_MISMATCH_CNTR;

      struct lb_stats* global_lru_mismatch_stats =
          bpf_map_lookup_elem(&stats, &global_lru_mismatch_stats_key);

      if (dst_consistent_hash && global_lru_mismatch_stats) {
        if (reals_have_same_addr(dst_consistent_hash, *dst)) {
          // We route to the same real as that indicated by the consistent
          // hash
          global_lru_mismatch_stats->v1++;
        } else {
          // We route to a real different from that indicated by the
          // consistent hash
          global_lru_mismatch_stats->v2++;
        }
      }
    }
  }

  return FURTHER_PROCESSING;
}

#endif // GLOBAL_LRU_LOOKUP

#ifdef INLINE_DECAP_IPIP
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 340,
 Endline: 387,
 Funcname: process_encaped_ipip_pckt,
 Input: (void **data, void **data_end, struct xdp_md *xdp, bool *is_ipv6, __u8 *protocol, bool pass),
 Output: int,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int process_encaped_ipip_pckt(
    void** data,
    void** data_end,
    struct xdp_md* xdp,
    bool* is_ipv6,
    __u8* protocol,
    bool pass) {
  int action;
  if (*protocol == IPPROTO_IPIP) {
    if (*is_ipv6) {
      int offset = sizeof(struct ipv6hdr) + sizeof(struct ethhdr);
      if ((*data + offset) > *data_end) {
        return XDP_DROP;
      }
      action = decrement_ttl(*data, *data_end, offset, false);
      if (!decap_v6(xdp, data, data_end, true)) {
        return XDP_DROP;
      }
      *is_ipv6 = false;
    } else {
      int offset = sizeof(struct iphdr) + sizeof(struct ethhdr);
      if ((*data + offset) > *data_end) {
        return XDP_DROP;
      }
      action = decrement_ttl(*data, *data_end, offset, false);
      if (!decap_v4(xdp, data, data_end)) {
        return XDP_DROP;
      }
    }
  } else if (*protocol == IPPROTO_IPV6) {
    int offset = sizeof(struct ipv6hdr) + sizeof(struct ethhdr);
    if ((*data + offset) > *data_end) {
      return XDP_DROP;
    }
    action = decrement_ttl(*data, *data_end, offset, true);
    if (!decap_v6(xdp, data, data_end, false)) {
      return XDP_DROP;
    }
  }
  if (action >= 0) {
    return action;
  }
  if (pass) {
    // pass packet to kernel after decapsulation
    return XDP_PASS;
  }
  return recirculate(xdp);
}
#endif // of INLINE_DECAP_IPIP

#ifdef INLINE_DECAP_GUE
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 391,
 Endline: 441,
 Funcname: process_encaped_gue_pckt,
 Input: (void **data, void **data_end, struct xdp_md *xdp, bool is_ipv6, bool pass),
 Output: int,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int 
process_encaped_gue_pckt(
    void** data,
    void** data_end,
    struct xdp_md* xdp,
    bool is_ipv6,
    bool pass) {
  int offset = 0;
  int action;
  if (is_ipv6) {
    __u8 v6 = 0;
    offset =
        sizeof(struct ipv6hdr) + sizeof(struct ethhdr) + sizeof(struct udphdr);
    // 1 byte for gue v1 marker to figure out what is internal protocol
    if ((*data + offset + 1) > *data_end) {
      return XDP_DROP;
    }
    v6 = ((__u8*)(*data))[offset];
    v6 &= GUEV1_IPV6MASK;
    if (v6) {
      // inner packet is ipv6 as well
      action = decrement_ttl(*data, *data_end, offset, true);
      if (!gue_decap_v6(xdp, data, data_end, false)) {
        return XDP_DROP;
      }
    } else {
      // inner packet is ipv4
      action = decrement_ttl(*data, *data_end, offset, false);
      if (!gue_decap_v6(xdp, data, data_end, true)) {
        return XDP_DROP;
      }
    }
  } else {
    offset =
        sizeof(struct iphdr) + sizeof(struct ethhdr) + sizeof(struct udphdr);
    if ((*data + offset) > *data_end) {
      return XDP_DROP;
    }
    action = decrement_ttl(*data, *data_end, offset, false);
    if (!gue_decap_v4(xdp, data, data_end)) {
      return XDP_DROP;
    }
  }
  if (action >= 0) {
    return action;
  }
  if (pass) {
    return XDP_PASS;
  }
  return recirculate(xdp);
}
#endif // of INLINE_DECAP_GUE

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 444,
 Endline: 457,
 Funcname: increment_quic_cid_version_stats,
 Input: (int host_id),
 Output: void,
 Helpers: [bpf_map_lookup_elem,],
 Read_maps: [ stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline void
increment_quic_cid_version_stats(int host_id) {
  __u32 quic_version_stats_key = MAX_VIPS + QUIC_CID_VERSION_STATS;
  struct lb_stats* quic_version =
      bpf_map_lookup_elem(&stats, &quic_version_stats_key);
  if (!quic_version) {
    return;
  }
  if (host_id > QUIC_CONNID_VERSION_V1_MAX_VAL) {
    quic_version->v2 += 1;
  } else {
    quic_version->v1 += 1;
  }
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 459,
 Endline: 468,
 Funcname: increment_quic_cid_drop_no_real,
 Input: (),
 Output: void,
 Helpers: [bpf_map_lookup_elem,],
 Read_maps: [ stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline void
increment_quic_cid_drop_no_real() {
  __u32 quic_drop_stats_key = MAX_VIPS + QUIC_CID_DROP_STATS;
  struct lb_stats* quic_drop =
      bpf_map_lookup_elem(&stats, &quic_drop_stats_key);
  if (!quic_drop) {
    return;
  }
  quic_drop->v1 += 1;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 470,
 Endline: 478,
 Funcname: increment_quic_cid_drop_real_0,
 Input: (),
 Output: void,
 Helpers: [bpf_map_lookup_elem,],
 Read_maps: [ stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline void increment_quic_cid_drop_real_0() {
  __u32 quic_drop_stats_key = MAX_VIPS + QUIC_CID_DROP_STATS;
  struct lb_stats* quic_drop =
      bpf_map_lookup_elem(&stats, &quic_drop_stats_key);
  if (!quic_drop) {
    return;
  }
  quic_drop->v2 += 1;
}

/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 480,
 Endline: 791,
 Funcname: process_packet,
 Input: (struct xdp_md *xdp, __u64 off, bool is_ipv6),
 Output: int,
 Helpers: [bpf_map_lookup_elem,bpf_get_smp_processor_id,],
 Read_maps: [  vip_map,  ctl_array,  reals_stats,  reals, server_id_map,  stats, lru_mapping, stats,],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline int process_packet(struct xdp_md* xdp, __u64 off, bool is_ipv6) {
  void* data = (void*)(long)xdp->data;
  void* data_end = (void*)(long)xdp->data_end;
  struct ctl_value* cval;
  struct real_definition* dst = NULL;
  struct packet_description pckt = {};
  struct vip_definition vip = {};
  struct vip_meta* vip_info;
  struct lb_stats* data_stats;
  __u64 iph_len;
  __u8 protocol;
  __u16 original_sport;

  int action;
  __u32 vip_num;
  __u32 mac_addr_pos = 0;
  __u16 pkt_bytes;
  action = process_l3_headers(
      &pckt, &protocol, off, &pkt_bytes, data, data_end, is_ipv6);
  if (action >= 0) {
    return action;
  }
  protocol = pckt.flow.proto;

#ifdef INLINE_DECAP_IPIP
  /* This is to workaround a verifier issue for 5.2.
   * The reason is that 5.2 verifier does not handle register
   * copy states properly while 5.6 handles properly.
   *
   * For the following source code:
   *   if (protocol == IPPROTO_IPIP || protocol == IPPROTO_IPV6) {
   *     ...
   *   }
   * llvm12 may generate the following simplified code sequence
   *   100  r5 = *(u8 *)(r9 +51)  // r5 is the protocol
   *   120  r4 = r5
   *   121  if r4 s> 0x10 goto target1
   *   122  *(u64 *)(r10 -184) = r5
   *   123  if r4 == 0x4 goto target2
   *   ...
   *   target2:
   *   150  r1 = *(u64 *)(r10 -184)
   *   151  if (r1 != 4) { __unreachable__}
   *
   * For the path 123->150->151, 5.6 correctly noticed
   * at insn 150: r4, r5, *(u64 *)(r10 -184) all have value 4.
   * while 5.2 has *(u64 *)(r10 -184) holding "r5" which could be
   * any value 0-255. In 5.2, "__unreachable" code is verified
   * and it caused verifier failure.
   */
  if (protocol == IPPROTO_IPIP) {
    bool pass = true;
    action = check_decap_dst(&pckt, is_ipv6, &pass);
    if (action >= 0) {
      return action;
    }
    return process_encaped_ipip_pckt(
        &data, &data_end, xdp, &is_ipv6, &protocol, pass);
  } else if (protocol == IPPROTO_IPV6) {
    bool pass = true;
    action = check_decap_dst(&pckt, is_ipv6, &pass);
    if (action >= 0) {
      return action;
    }
    return process_encaped_ipip_pckt(
        &data, &data_end, xdp, &is_ipv6, &protocol, pass);
  }
#endif // INLINE_DECAP_IPIP

  if (protocol == IPPROTO_TCP) {
    if (!parse_tcp(data, data_end, is_ipv6, &pckt)) {
      return XDP_DROP;
    }
  } else if (protocol == IPPROTO_UDP) {
    if (!parse_udp(data, data_end, is_ipv6, &pckt)) {
      return XDP_DROP;
    }
#ifdef INLINE_DECAP_GUE
    if (pckt.flow.port16[1] == bpf_htons(GUE_DPORT)) {
      bool pass = true;
      action = check_decap_dst(&pckt, is_ipv6, &pass);
      if (action >= 0) {
        return action;
      }
      return process_encaped_gue_pckt(&data, &data_end, xdp, is_ipv6, pass);
    }
#endif // of INLINE_DECAP_GUE
  } else {
    // send to tcp/ip stack
    return XDP_PASS;
  }

  if (is_ipv6) {
    memcpy(vip.vipv6, pckt.flow.dstv6, 16);
  } else {
    vip.vip = pckt.flow.dst;
  }

  vip.port = pckt.flow.port16[1];
  vip.proto = pckt.flow.proto;
  vip_info = bpf_map_lookup_elem(&vip_map, &vip);
  if (!vip_info) {
    vip.port = 0;
    vip_info = bpf_map_lookup_elem(&vip_map, &vip);
    if (!vip_info) {
      return XDP_PASS;
    }

    if (!(vip_info->flags & F_HASH_DPORT_ONLY)) {
      // VIP, which doesnt care about dst port (all packets to this VIP w/ diff
      // dst port but from the same src port/ip must go to the same real
      pckt.flow.port16[1] = 0;
    }
  }

  if (data_end - data > MAX_PCKT_SIZE) {
    REPORT_PACKET_TOOBIG(xdp, data, data_end - data, false);
#ifdef ICMP_TOOBIG_GENERATION
    __u32 stats_key = MAX_VIPS + ICMP_TOOBIG_CNTRS;
    data_stats = bpf_map_lookup_elem(&stats, &stats_key);
    if (!data_stats) {
      return XDP_DROP;
    }
    if (is_ipv6) {
      data_stats->v2 += 1;
    } else {
      data_stats->v1 += 1;
    }
    return send_icmp_too_big(xdp, is_ipv6, data_end - data);
#else
    return XDP_DROP;
#endif
  }

  __u32 stats_key = MAX_VIPS + LRU_CNTRS;
  data_stats = bpf_map_lookup_elem(&stats, &stats_key);
  if (!data_stats) {
    return XDP_DROP;
  }

  // total packets
  data_stats->v1 += 1;

  // Lookup dst based on id in packet
  if ((vip_info->flags & F_QUIC_VIP)) {
    __u32 quic_stats_key = MAX_VIPS + QUIC_ROUTE_STATS;
    struct lb_stats* quic_stats = bpf_map_lookup_elem(&stats, &quic_stats_key);
    if (!quic_stats) {
      return XDP_DROP;
    }
    int real_index;
    real_index = parse_quic(data, data_end, is_ipv6, &pckt);
    if (real_index > 0) {
      increment_quic_cid_version_stats(real_index);
      __u32 key = real_index;
      __u32* real_pos = bpf_map_lookup_elem(&server_id_map, &key);
      if (real_pos) {
        key = *real_pos;
        if (key == 0) {
          increment_quic_cid_drop_real_0();
          // increment counter for the CH based routing
          quic_stats->v1 += 1;
        } else {
          pckt.real_index = key;
          dst = bpf_map_lookup_elem(&reals, &key);
          if (!dst) {
            increment_quic_cid_drop_no_real();
            REPORT_QUIC_PACKET_DROP_NO_REAL(xdp, data, data_end - data, false);
            return XDP_DROP;
          }
          quic_stats->v2 += 1;
        }
      } else {
        // increment counter for the CH based routing
        quic_stats->v1 += 1;
      }
    } else {
      quic_stats->v1 += 1;
    }
  }

  // save the original sport before making real selection, possibly changing its
  // value.
  original_sport = pckt.flow.port16[0];

  if (!dst) {
    if ((vip_info->flags & F_HASH_NO_SRC_PORT)) {
      // service, where diff src port, but same ip must go to the same real,
      // e.g. gfs
      pckt.flow.port16[0] = 0;
    }
    __u32 cpu_num = bpf_get_smp_processor_id();
    void* lru_map = bpf_map_lookup_elem(&lru_mapping, &cpu_num);
    if (!lru_map) {
      lru_map = &fallback_cache;
      __u32 lru_stats_key = MAX_VIPS + FALLBACK_LRU_CNTR;
      struct lb_stats* lru_stats = bpf_map_lookup_elem(&stats, &lru_stats_key);
      if (!lru_stats) {
        return XDP_DROP;
      }
      // We were not able to retrieve per cpu/core lru and falling back to
      // default one. This counter should never be anything except 0 in prod.
      // We are going to use it for monitoring.
      lru_stats->v1 += 1;
    }
#ifdef TCP_SERVER_ID_ROUTING
    // First try to lookup dst in the tcp_hdr_opt (if enabled)
    if (pckt.flow.proto == IPPROTO_TCP && !(pckt.flags & F_SYN_SET)) {
      __u32 routing_stats_key = MAX_VIPS + TCP_SERVER_ID_ROUTE_STATS;
      struct lb_stats* routing_stats =
          bpf_map_lookup_elem(&stats, &routing_stats_key);
      if (!routing_stats) {
        return XDP_DROP;
      }
      if (tcp_hdr_opt_lookup(
              xdp,
              is_ipv6,
              &dst,
              &pckt,
              vip_info->flags & F_LRU_BYPASS,
              lru_map) == FURTHER_PROCESSING) {
        routing_stats->v1 += 1;
      } else {
        routing_stats->v2 += 1;
      }
    }
#endif // TCP_SERVER_ID_ROUTING

    // Next, try to lookup dst in the lru_cache
    if (!dst && !(pckt.flags & F_SYN_SET) &&
        !(vip_info->flags & F_LRU_BYPASS)) {
      connection_table_lookup(&dst, &pckt, lru_map, /*isGlobalLru=*/false);
    }

#ifdef GLOBAL_LRU_LOOKUP
    if (!dst && !(pckt.flags & F_SYN_SET) && vip_info->flags & F_GLOBAL_LRU) {
      int global_lru_lookup_result =
          perform_global_lru_lookup(&dst, &pckt, cpu_num, vip_info, is_ipv6);
      if (global_lru_lookup_result >= 0) {
        return global_lru_lookup_result;
      }
    }
#endif // GLOBAL_LRU_LOOKUP

    // if dst is not found, route via consistent-hashing of the flow.
    if (!dst) {
      if (pckt.flow.proto == IPPROTO_TCP) {
        __u32 lru_stats_key = MAX_VIPS + LRU_MISS_CNTR;
        struct lb_stats* lru_stats =
            bpf_map_lookup_elem(&stats, &lru_stats_key);
        if (!lru_stats) {
          return XDP_DROP;
        }
        if (pckt.flags & F_SYN_SET) {
          // miss because of new tcp session
          lru_stats->v1 += 1;
        } else {
          // miss of non-syn tcp packet. could be either because of LRU trashing
          // or because another katran is restarting and all the sessions
          // have been reshuffled
          REPORT_TCP_NONSYN_LRUMISS(xdp, data, data_end - data, false);
          lru_stats->v2 += 1;
        }
      }
      if (!get_packet_dst(&dst, &pckt, vip_info, is_ipv6, lru_map)) {
        return XDP_DROP;
      }
      // lru misses (either new connection or lru is full and starts to trash)
      data_stats->v2 += 1;
    }
  }

  cval = bpf_map_lookup_elem(&ctl_array, &mac_addr_pos);

  if (!cval) {
    return XDP_DROP;
  }

  vip_num = vip_info->vip_num;
  data_stats = bpf_map_lookup_elem(&stats, &vip_num);
  if (!data_stats) {
    return XDP_DROP;
  }
  data_stats->v1 += 1;
  data_stats->v2 += pkt_bytes;

  // per real statistics
  data_stats = bpf_map_lookup_elem(&reals_stats, &pckt.real_index);
  if (!data_stats) {
    return XDP_DROP;
  }
  data_stats->v1 += 1;
  data_stats->v2 += pkt_bytes;
#ifdef LOCAL_DELIVERY_OPTIMIZATION
  if ((vip_info->flags & F_LOCAL_VIP) && (dst->flags & F_LOCAL_REAL)) {
    return XDP_PASS;
  }
#endif
  // restore the original sport value to use it as a seed for the GUE sport
  pckt.flow.port16[0] = original_sport;
  if (dst->flags & F_IPV6) {
    if (!PCKT_ENCAP_V6(xdp, cval, is_ipv6, &pckt, dst, pkt_bytes)) {
      return XDP_DROP;
    }
  } else {
    if (!PCKT_ENCAP_V4(xdp, cval, &pckt, dst, pkt_bytes)) {
      return XDP_DROP;
    }
  }

  return XDP_TX;
}

SEC("xdp")
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 794,
 Endline: 817,
 Funcname: balancer_ingress,
 Input: (struct xdp_md *ctx),
 Output: int,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
int balancer_ingress(struct xdp_md* ctx) {
  void* data = (void*)(long)ctx->data;
  void* data_end = (void*)(long)ctx->data_end;
  struct ethhdr* eth = data;
  __u32 eth_proto;
  __u32 nh_off;
  nh_off = sizeof(struct ethhdr);

  if (data + nh_off > data_end) {
    // bogus packet, len less than minimum ethernet frame size
    return XDP_DROP;
  }

  eth_proto = eth->h_proto;

  if (eth_proto == BE_ETH_P_IP) {
    return process_packet(ctx, nh_off, false);
  } else if (eth_proto == BE_ETH_P_IPV6) {
    return process_packet(ctx, nh_off, true);
  } else {
    // pass to tcp/ip stack
    return XDP_PASS;
  }
}
/* 
 OPENED COMMENT BEGIN 
 { 
 File: /root/examples/katran/balancer_kern.c,
 Startline: 818,
 Endline: 827,
 Funcname: get_packet_hash,
 Input: (struct packet_description *pckt, bool hash_16bytes),
 Output: __u32,
 Helpers: [],
 Read_maps: [],
 Update_maps: [],
 Func Description: TO BE ADDED, 
 Commentor: TO BE ADDED (<name>,<email>) 
 } 
 OPENED COMMENT END 
 */ 
__attribute__((__always_inline__)) static inline __u32 get_packet_hash(struct packet_description* pckt,bool hash_16bytes) {
  if (hash_16bytes) {
    return jhash_2words(
        jhash(pckt->flow.srcv6, 16, INIT_JHASH_SEED_V6),
        pckt->flow.ports,
        INIT_JHASH_SEED);
  } else {
    return jhash_2words(pckt->flow.src, pckt->flow.ports, INIT_JHASH_SEED);
  }
}


char _license[] SEC("license") = "GPL";