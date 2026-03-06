/* ============================================================================
 * icmp.c — ICMP echo (ping) handler
 *
 * Responds to echo requests and can send echo requests for ping.
 * ============================================================================ */

#include "net.h"
#include "printk.h"

/* --- Memory helpers --- */
static void icmp_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* Ping reply callback (set by ping command in shell) */
static volatile uint8_t  ping_got_reply = 0;
static volatile uint16_t ping_reply_seq = 0;
static volatile uint32_t ping_reply_src = 0;

uint8_t  icmp_ping_got_reply(void)  { return ping_got_reply; }
uint16_t icmp_ping_reply_seq(void)  { return ping_reply_seq; }
void     icmp_ping_clear_reply(void) { ping_got_reply = 0; }

/* --- Handle incoming ICMP packet --- */
void icmp_handle(uint32_t src_ip, const void *data, uint32_t len)
{
    const struct icmp_header *hdr = (const struct icmp_header *)data;

    if (len < ICMP_HEADER_SIZE)
        return;

    switch (hdr->type) {
    case ICMP_ECHO_REQUEST:
        /* Reply to ping: swap src/dst and change type to reply */
        {
            uint8_t reply_buf[ETH_MTU];
            struct icmp_header *reply = (struct icmp_header *)reply_buf;

            if (len > ETH_MTU - IPV4_HEADER_SIZE)
                return;

            /* Copy the echo request payload */
            icmp_memcpy(reply_buf, data, len);

            /* Change to echo reply */
            reply->type = ICMP_ECHO_REPLY;
            reply->code = 0;
            reply->checksum = 0;
            reply->checksum = ipv4_checksum(reply_buf, len);

            ipv4_send(src_ip, IP_PROTO_ICMP, reply_buf, len);
        }
        break;

    case ICMP_ECHO_REPLY:
        /* We got a ping reply */
        ping_got_reply = 1;
        ping_reply_seq = ntohs(hdr->seq);
        ping_reply_src = src_ip;

        printk("[ICMP] Echo reply from %u.%u.%u.%u seq=%u\n",
               (uint64_t)(src_ip & 0xFF),
               (uint64_t)((src_ip >> 8) & 0xFF),
               (uint64_t)((src_ip >> 16) & 0xFF),
               (uint64_t)((src_ip >> 24) & 0xFF),
               (uint64_t)ntohs(hdr->seq));
        break;

    default:
        break;
    }
}

/* --- Send an ICMP echo request (ping) --- */
void icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void *payload, uint32_t payload_len)
{
    uint8_t buf[ETH_MTU];
    struct icmp_header *hdr = (struct icmp_header *)buf;
    uint32_t total = ICMP_HEADER_SIZE + payload_len;

    if (total > ETH_MTU - IPV4_HEADER_SIZE)
        return;

    hdr->type = ICMP_ECHO_REQUEST;
    hdr->code = 0;
    hdr->id = htons(id);
    hdr->seq = htons(seq);
    hdr->checksum = 0;

    if (payload && payload_len > 0)
        icmp_memcpy(buf + ICMP_HEADER_SIZE, payload, payload_len);

    hdr->checksum = ipv4_checksum(buf, total);

    ipv4_send(dst_ip, IP_PROTO_ICMP, buf, total);
}
