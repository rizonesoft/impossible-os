/* ============================================================================
 * ethernet.c — Ethernet frame handling
 *
 * Builds outgoing frames and dispatches incoming frames by EtherType.
 * ============================================================================ */

#include "net.h"
#include "rtl8139.h"
#include "printk.h"

/* Global network configuration */
struct net_config net_cfg;

/* Memory helpers */
static void net_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void net_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

/* --- Initialize the network stack --- */
void net_init(void)
{
    net_memset(&net_cfg, 0, sizeof(net_cfg));
    rtl8139_get_mac(net_cfg.mac);
    net_cfg.configured = 0;

    arp_init();

    printk("[NET] Stack initialized, MAC: %x:%x:%x:%x:%x:%x\n",
           (uint64_t)net_cfg.mac[0], (uint64_t)net_cfg.mac[1],
           (uint64_t)net_cfg.mac[2], (uint64_t)net_cfg.mac[3],
           (uint64_t)net_cfg.mac[4], (uint64_t)net_cfg.mac[5]);
}

/* --- Send an Ethernet frame --- */
void eth_send(const uint8_t *dst_mac, uint16_t ethertype,
              const void *payload, uint32_t payload_len)
{
    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header *hdr = (struct eth_header *)frame;
    uint32_t frame_len;

    if (payload_len > ETH_MTU)
        payload_len = ETH_MTU;

    frame_len = ETH_HEADER_SIZE + payload_len;

    /* Build Ethernet header */
    net_memcpy(hdr->dst_mac, dst_mac, MAC_LEN);
    net_memcpy(hdr->src_mac, net_cfg.mac, MAC_LEN);
    hdr->ethertype = htons(ethertype);

    /* Copy payload */
    net_memcpy(frame + ETH_HEADER_SIZE, payload, payload_len);

    /* Pad to minimum frame size (64 bytes including CRC, 60 without) */
    if (frame_len < 60) {
        net_memset(frame + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    rtl8139_send(frame, frame_len);
}

/* --- Process a received Ethernet frame --- */
void net_rx(void *data, uint32_t len)
{
    struct eth_header *hdr = (struct eth_header *)data;
    uint16_t ethertype;
    void *payload;
    uint32_t payload_len;

    if (len < ETH_HEADER_SIZE)
        return;

    ethertype = ntohs(hdr->ethertype);
    payload = (uint8_t *)data + ETH_HEADER_SIZE;
    payload_len = len - ETH_HEADER_SIZE;

    switch (ethertype) {
    case ETHERTYPE_ARP:
        arp_handle(payload, payload_len);
        break;
    case ETHERTYPE_IPV4:
        ipv4_handle(payload, payload_len);
        break;
    default:
        break;
    }
}
