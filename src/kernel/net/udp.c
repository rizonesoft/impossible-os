/* ============================================================================
 * udp.c — UDP send/receive
 *
 * Builds/parses UDP datagrams. Dispatches to DHCP on port 68.
 * ============================================================================ */

#include "net.h"
#include "printk.h"

/* --- Memory helpers --- */
static void udp_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* --- Handle incoming UDP packet --- */
void udp_handle(uint32_t src_ip, const void *data, uint32_t len)
{
    const struct udp_header *hdr = (const struct udp_header *)data;
    uint16_t dst_port;
    const void *payload;
    uint32_t payload_len;

    (void)src_ip;

    if (len < UDP_HEADER_SIZE)
        return;

    dst_port = ntohs(hdr->dst_port);
    payload = (const uint8_t *)data + UDP_HEADER_SIZE;
    payload_len = ntohs(hdr->length) - UDP_HEADER_SIZE;

    /* Dispatch based on destination port */
    switch (dst_port) {
    case DHCP_CLIENT_PORT:
        dhcp_handle(payload, payload_len);
        break;
    default:
        /* No handler — silently drop */
        break;
    }
}

/* --- Send a UDP datagram --- */
void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *payload, uint32_t payload_len)
{
    uint8_t buf[ETH_MTU];
    struct udp_header *hdr = (struct udp_header *)buf;
    uint32_t udp_len = UDP_HEADER_SIZE + payload_len;

    if (udp_len > ETH_MTU - IPV4_HEADER_SIZE)
        return;

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons((uint16_t)udp_len);
    hdr->checksum = 0;  /* Optional for IPv4 — skip for simplicity */

    udp_memcpy(buf + UDP_HEADER_SIZE, payload, payload_len);

    ipv4_send(dst_ip, IP_PROTO_UDP, buf, udp_len);
}
