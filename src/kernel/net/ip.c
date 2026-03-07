/* ============================================================================
 * ip.c — IPv4 send/receive
 *
 * Builds IP datagrams, computes header checksums, dispatches by protocol.
 * ============================================================================ */

#include "kernel/net/net.h"
#include "kernel/printk.h"

/* --- Memory helpers --- */
static void ip_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void ip_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

/* --- IPv4 header checksum --- */
uint16_t ipv4_checksum(const void *data, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }

    /* Handle odd byte */
    if (len == 1)
        sum += *(const uint8_t *)p;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* Packet identification counter */
static uint16_t ip_id_counter = 1;

/* --- Send an IPv4 packet --- */
void ipv4_send(uint32_t dst_ip, uint8_t protocol,
               const void *payload, uint32_t payload_len)
{
    uint8_t packet[ETH_MTU];
    struct ipv4_header *hdr = (struct ipv4_header *)packet;
    uint32_t total_len = IPV4_HEADER_SIZE + payload_len;
    uint8_t dst_mac[MAC_LEN];
    uint32_t resolve_ip;

    if (total_len > ETH_MTU)
        return;

    /* Build IPv4 header */
    ip_memset(hdr, 0, IPV4_HEADER_SIZE);
    hdr->ver_ihl = 0x45;        /* IPv4, 5 DWORDs (no options) */
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)total_len);
    hdr->id = htons(ip_id_counter++);
    hdr->flags_frag = 0;
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->src_ip = net_cfg.ip;
    hdr->dst_ip = dst_ip;
    hdr->checksum = 0;
    hdr->checksum = ipv4_checksum(hdr, IPV4_HEADER_SIZE);

    /* Copy payload after header */
    ip_memcpy(packet + IPV4_HEADER_SIZE, payload, payload_len);

    /* Determine which IP to ARP for:
     * - If dst is on our subnet → ARP for dst directly
     * - Otherwise → ARP for the gateway */
    if (dst_ip == 0xFFFFFFFF) {
        /* Broadcast */
        uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        eth_send(bcast, ETHERTYPE_IPV4, packet, total_len);
        return;
    }

    if (net_cfg.configured && net_cfg.gateway != 0 &&
        (dst_ip & net_cfg.subnet) != (net_cfg.ip & net_cfg.subnet)) {
        resolve_ip = net_cfg.gateway;
    } else {
        resolve_ip = dst_ip;
    }

    /* Resolve MAC via ARP */
    if (arp_resolve(resolve_ip, dst_mac) < 0) {
        /* ARP miss — send ARP request and drop this packet */
        arp_request(resolve_ip);
        return;
    }

    eth_send(dst_mac, ETHERTYPE_IPV4, packet, total_len);
}

/* --- Handle an incoming IPv4 packet --- */
void ipv4_handle(const void *data, uint32_t len)
{
    const struct ipv4_header *hdr = (const struct ipv4_header *)data;
    uint16_t total_len;
    uint32_t hdr_len;
    const void *payload;
    uint32_t payload_len;

    if (len < IPV4_HEADER_SIZE)
        return;

    /* Check version */
    if ((hdr->ver_ihl >> 4) != 4)
        return;

    hdr_len = (uint32_t)(hdr->ver_ihl & 0x0F) * 4;
    total_len = ntohs(hdr->total_len);

    if (total_len > len)
        return;

    payload = (const uint8_t *)data + hdr_len;
    payload_len = total_len - hdr_len;

    /* Dispatch by protocol */
    switch (hdr->protocol) {
    case IP_PROTO_ICMP:
        icmp_handle(hdr->src_ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        udp_handle(hdr->src_ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        /* TCP not implemented — silently drop */
        break;
    default:
        break;
    }
}
