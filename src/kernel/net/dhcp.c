/* ============================================================================
 * dhcp.c — DHCP client
 *
 * Performs DHCP discover/offer/request/ack to obtain IP configuration.
 * ============================================================================ */

#include "net.h"
#include "printk.h"

/* --- Memory helpers --- */
static void dhcp_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void dhcp_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

/* DHCP state */
static uint32_t dhcp_xid = 0x12345678;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_offered_ip = 0;
static uint8_t  dhcp_state = 0;  /* 0=idle, 1=discover sent, 2=request sent */

/* --- Add a DHCP option --- */
static uint8_t *dhcp_add_option(uint8_t *ptr, uint8_t code,
                                 uint8_t len, const void *data)
{
    *ptr++ = code;
    *ptr++ = len;
    dhcp_memcpy(ptr, data, len);
    return ptr + len;
}

/* --- Send DHCP Discover --- */
void dhcp_discover(void)
{
    struct dhcp_packet pkt;
    uint8_t *opt;
    uint8_t msg_type;
    uint32_t total_len;

    dhcp_memset(&pkt, 0, sizeof(pkt));

    pkt.op = 1;          /* BOOTREQUEST */
    pkt.htype = 1;       /* Ethernet */
    pkt.hlen = 6;
    pkt.xid = htonl(dhcp_xid);
    pkt.flags = htons(0x8000);  /* Broadcast flag */
    dhcp_memcpy(pkt.chaddr, net_cfg.mac, MAC_LEN);
    pkt.magic = htonl(DHCP_MAGIC_COOKIE);

    /* Options */
    opt = pkt.options;

    /* Option 53: DHCP Message Type = DISCOVER */
    msg_type = DHCP_DISCOVER;
    opt = dhcp_add_option(opt, 53, 1, &msg_type);

    /* Option 55: Parameter Request List */
    {
        uint8_t params[] = {1, 3, 6, 15, 26, 28};  /* Subnet, Router, DNS, Domain, MTU, Broadcast */
        opt = dhcp_add_option(opt, 55, sizeof(params), params);
    }

    /* End option */
    *opt++ = 0xFF;

    total_len = (uint32_t)(opt - (uint8_t *)&pkt);

    /* Send via UDP broadcast */
    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             &pkt, total_len);

    dhcp_state = 1;
    printk("[DHCP] Discover sent\n");
}

/* --- Send DHCP Request --- */
static void dhcp_request(void)
{
    struct dhcp_packet pkt;
    uint8_t *opt;
    uint8_t msg_type;
    uint32_t total_len;

    dhcp_memset(&pkt, 0, sizeof(pkt));

    pkt.op = 1;
    pkt.htype = 1;
    pkt.hlen = 6;
    pkt.xid = htonl(dhcp_xid);
    pkt.flags = htons(0x8000);
    dhcp_memcpy(pkt.chaddr, net_cfg.mac, MAC_LEN);
    pkt.magic = htonl(DHCP_MAGIC_COOKIE);

    opt = pkt.options;

    /* Option 53: DHCP Message Type = REQUEST */
    msg_type = DHCP_REQUEST;
    opt = dhcp_add_option(opt, 53, 1, &msg_type);

    /* Option 50: Requested IP Address */
    opt = dhcp_add_option(opt, 50, 4, &dhcp_offered_ip);

    /* Option 54: Server Identifier */
    opt = dhcp_add_option(opt, 54, 4, &dhcp_server_ip);

    *opt++ = 0xFF;

    total_len = (uint32_t)(opt - (uint8_t *)&pkt);

    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             &pkt, total_len);

    dhcp_state = 2;
    printk("[DHCP] Request sent for %u.%u.%u.%u\n",
           (uint64_t)(dhcp_offered_ip & 0xFF),
           (uint64_t)((dhcp_offered_ip >> 8) & 0xFF),
           (uint64_t)((dhcp_offered_ip >> 16) & 0xFF),
           (uint64_t)((dhcp_offered_ip >> 24) & 0xFF));
}

/* --- Parse DHCP options --- */
static void dhcp_parse_options(const uint8_t *opts, uint32_t len,
                                uint8_t *msg_type_out)
{
    uint32_t i = 0;

    while (i < len) {
        uint8_t code = opts[i++];

        if (code == 0xFF)   /* End */
            break;
        if (code == 0x00) { /* Pad */
            continue;
        }

        if (i >= len) break;
        uint8_t opt_len = opts[i++];
        if (i + opt_len > len) break;

        switch (code) {
        case 1:  /* Subnet Mask */
            if (opt_len == 4)
                dhcp_memcpy(&net_cfg.subnet, &opts[i], 4);
            break;
        case 3:  /* Router (Gateway) */
            if (opt_len >= 4)
                dhcp_memcpy(&net_cfg.gateway, &opts[i], 4);
            break;
        case 6:  /* DNS Server */
            if (opt_len >= 4)
                dhcp_memcpy(&net_cfg.dns, &opts[i], 4);
            break;
        case 53: /* DHCP Message Type */
            if (opt_len == 1 && msg_type_out)
                *msg_type_out = opts[i];
            break;
        case 54: /* Server Identifier */
            if (opt_len == 4)
                dhcp_memcpy(&dhcp_server_ip, &opts[i], 4);
            break;
        default:
            break;
        }

        i += opt_len;
    }
}

/* --- Handle incoming DHCP packet --- */
void dhcp_handle(const void *data, uint32_t len)
{
    const struct dhcp_packet *pkt = (const struct dhcp_packet *)data;
    uint8_t msg_type = 0;
    uint32_t opts_offset;

    if (len < 240)  /* Minimum DHCP packet */
        return;

    /* Check op=BOOTREPLY, matching xid */
    if (pkt->op != 2)
        return;
    if (ntohl(pkt->xid) != dhcp_xid)
        return;

    /* Parse options (after magic cookie) */
    opts_offset = (uint32_t)((uint64_t)&pkt->options - (uint64_t)pkt);
    dhcp_parse_options(pkt->options,
                       len > opts_offset ? len - opts_offset : 0,
                       &msg_type);

    switch (msg_type) {
    case DHCP_OFFER:
        if (dhcp_state != 1)
            break;

        dhcp_offered_ip = pkt->yiaddr;

        printk("[DHCP] Offer: %u.%u.%u.%u\n",
               (uint64_t)(dhcp_offered_ip & 0xFF),
               (uint64_t)((dhcp_offered_ip >> 8) & 0xFF),
               (uint64_t)((dhcp_offered_ip >> 16) & 0xFF),
               (uint64_t)((dhcp_offered_ip >> 24) & 0xFF));

        dhcp_request();
        break;

    case DHCP_ACK:
        if (dhcp_state != 2)
            break;

        net_cfg.ip = pkt->yiaddr;
        net_cfg.configured = 1;
        dhcp_state = 0;

        printk("[OK] DHCP: IP %u.%u.%u.%u",
               (uint64_t)(net_cfg.ip & 0xFF),
               (uint64_t)((net_cfg.ip >> 8) & 0xFF),
               (uint64_t)((net_cfg.ip >> 16) & 0xFF),
               (uint64_t)((net_cfg.ip >> 24) & 0xFF));
        printk(", GW %u.%u.%u.%u",
               (uint64_t)(net_cfg.gateway & 0xFF),
               (uint64_t)((net_cfg.gateway >> 8) & 0xFF),
               (uint64_t)((net_cfg.gateway >> 16) & 0xFF),
               (uint64_t)((net_cfg.gateway >> 24) & 0xFF));
        printk(", DNS %u.%u.%u.%u\n",
               (uint64_t)(net_cfg.dns & 0xFF),
               (uint64_t)((net_cfg.dns >> 8) & 0xFF),
               (uint64_t)((net_cfg.dns >> 16) & 0xFF),
               (uint64_t)((net_cfg.dns >> 24) & 0xFF));
        break;

    default:
        break;
    }
}
