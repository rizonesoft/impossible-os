/* ============================================================================
 * arp.c — Address Resolution Protocol
 *
 * Maintains an ARP table cache. Handles ARP requests/replies.
 * ============================================================================ */

#include "kernel/net/net.h"
#include "kernel/printk.h"

/* --- Memory helpers --- */
static void arp_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void arp_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

/* --- ARP table --- */
#define ARP_TABLE_SIZE 32

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[MAC_LEN];
    uint8_t  valid;
};

static struct arp_entry arp_table[ARP_TABLE_SIZE];
static uint32_t arp_next_slot = 0;

void arp_init(void)
{
    arp_memset(arp_table, 0, sizeof(arp_table));
}

/* Add or update an entry in the ARP table */
static void arp_table_insert(uint32_t ip, const uint8_t *mac)
{
    uint32_t i;

    /* Update existing entry */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            arp_memcpy(arp_table[i].mac, mac, MAC_LEN);
            return;
        }
    }

    /* Insert new entry (round-robin eviction) */
    arp_table[arp_next_slot].ip = ip;
    arp_memcpy(arp_table[arp_next_slot].mac, mac, MAC_LEN);
    arp_table[arp_next_slot].valid = 1;
    arp_next_slot = (arp_next_slot + 1) % ARP_TABLE_SIZE;
}

/* Resolve IP to MAC. Returns 0 on success, -1 on miss. */
int arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    uint32_t i;

    /* Broadcast address always resolves to broadcast MAC */
    if (ip == 0xFFFFFFFF) {
        uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        arp_memcpy(mac_out, bcast, MAC_LEN);
        return 0;
    }

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            arp_memcpy(mac_out, arp_table[i].mac, MAC_LEN);
            return 0;
        }
    }
    return -1;
}

/* Send an ARP request for a target IP */
void arp_request(uint32_t target_ip)
{
    struct arp_packet pkt;
    uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    pkt.hw_type = htons(ARP_HW_ETHER);
    pkt.proto_type = htons(ETHERTYPE_IPV4);
    pkt.hw_len = MAC_LEN;
    pkt.proto_len = 4;
    pkt.opcode = htons(ARP_OP_REQUEST);
    arp_memcpy(pkt.sender_mac, net_cfg.mac, MAC_LEN);
    pkt.sender_ip = net_cfg.ip;
    arp_memset(pkt.target_mac, 0, MAC_LEN);
    pkt.target_ip = target_ip;

    eth_send(bcast, ETHERTYPE_ARP, &pkt, sizeof(pkt));
}

/* Handle an incoming ARP packet */
void arp_handle(const void *data, uint32_t len)
{
    const struct arp_packet *pkt = (const struct arp_packet *)data;
    struct arp_packet reply;

    if (len < sizeof(struct arp_packet))
        return;

    /* Only handle Ethernet + IPv4 */
    if (ntohs(pkt->hw_type) != ARP_HW_ETHER)
        return;
    if (ntohs(pkt->proto_type) != ETHERTYPE_IPV4)
        return;

    /* Learn sender MAC → IP mapping */
    arp_table_insert(pkt->sender_ip, pkt->sender_mac);

    uint16_t op = ntohs(pkt->opcode);

    if (op == ARP_OP_REQUEST) {
        /* Someone is asking for our MAC — reply if it's for us */
        if (pkt->target_ip == net_cfg.ip && net_cfg.configured) {
            reply.hw_type = htons(ARP_HW_ETHER);
            reply.proto_type = htons(ETHERTYPE_IPV4);
            reply.hw_len = MAC_LEN;
            reply.proto_len = 4;
            reply.opcode = htons(ARP_OP_REPLY);
            arp_memcpy(reply.sender_mac, net_cfg.mac, MAC_LEN);
            reply.sender_ip = net_cfg.ip;
            arp_memcpy(reply.target_mac, pkt->sender_mac, MAC_LEN);
            reply.target_ip = pkt->sender_ip;

            eth_send(pkt->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
        }
    } else if (op == ARP_OP_REPLY) {
        /* Already learned above — just log */
        printk("[ARP] Reply: %u.%u.%u.%u\n",
               (uint64_t)(pkt->sender_ip & 0xFF),
               (uint64_t)((pkt->sender_ip >> 8) & 0xFF),
               (uint64_t)((pkt->sender_ip >> 16) & 0xFF),
               (uint64_t)((pkt->sender_ip >> 24) & 0xFF));
    }
}
