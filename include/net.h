/* ============================================================================
 * net.h — Common network definitions
 *
 * Byte-order helpers, MAC/IP types, Ethernet/ARP/IPv4/ICMP/UDP headers.
 * ============================================================================ */

#pragma once

#include "types.h"

/* --- Byte-order helpers (host is little-endian x86) --- */
static inline uint16_t htons(uint16_t h)
{
    return (uint16_t)((h << 8) | (h >> 8));
}

static inline uint16_t ntohs(uint16_t n)
{
    return htons(n);
}

static inline uint32_t htonl(uint32_t h)
{
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8)
         | ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

static inline uint32_t ntohl(uint32_t n)
{
    return htonl(n);
}

/* --- MAC address --- */
#define MAC_LEN 6
#define MAC_BROADCAST ((uint8_t[]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF})

/* --- EtherType values --- */
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

/* --- Ethernet frame header (14 bytes) --- */
struct eth_header {
    uint8_t  dst_mac[MAC_LEN];
    uint8_t  src_mac[MAC_LEN];
    uint16_t ethertype;          /* big-endian */
} __attribute__((packed));

#define ETH_HEADER_SIZE  14
#define ETH_MTU          1500
#define ETH_FRAME_MAX    (ETH_HEADER_SIZE + ETH_MTU)

/* --- ARP --- */
#define ARP_HW_ETHER  1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_packet {
    uint16_t hw_type;           /* Hardware type (1 = Ethernet) */
    uint16_t proto_type;        /* Protocol type (0x0800 = IPv4) */
    uint8_t  hw_len;            /* Hardware address length (6) */
    uint8_t  proto_len;         /* Protocol address length (4) */
    uint16_t opcode;            /* 1 = request, 2 = reply */
    uint8_t  sender_mac[MAC_LEN];
    uint32_t sender_ip;         /* big-endian */
    uint8_t  target_mac[MAC_LEN];
    uint32_t target_ip;         /* big-endian */
} __attribute__((packed));

/* --- IPv4 --- */
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

struct ipv4_header {
    uint8_t  ver_ihl;           /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;               /* Type of service */
    uint16_t total_len;         /* Total length (big-endian) */
    uint16_t id;                /* Identification */
    uint16_t flags_frag;        /* Flags + Fragment offset */
    uint8_t  ttl;               /* Time to live */
    uint8_t  protocol;          /* Protocol (ICMP=1, TCP=6, UDP=17) */
    uint16_t checksum;          /* Header checksum */
    uint32_t src_ip;            /* Source IP (big-endian) */
    uint32_t dst_ip;            /* Destination IP (big-endian) */
} __attribute__((packed));

#define IPV4_HEADER_SIZE  20

/* Make an IP from 4 octets (host byte order) */
#define MAKE_IP(a,b,c,d) htonl(((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

/* --- ICMP --- */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

#define ICMP_HEADER_SIZE 8

/* --- UDP --- */
struct udp_header {
    uint16_t src_port;          /* big-endian */
    uint16_t dst_port;          /* big-endian */
    uint16_t length;            /* big-endian (header + data) */
    uint16_t checksum;          /* optional for IPv4 */
} __attribute__((packed));

#define UDP_HEADER_SIZE 8

/* --- DHCP --- */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_packet {
    uint8_t  op;                /* 1=BOOTREQUEST, 2=BOOTREPLY */
    uint8_t  htype;             /* 1=Ethernet */
    uint8_t  hlen;              /* 6 */
    uint8_t  hops;
    uint32_t xid;               /* Transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;            /* Client IP */
    uint32_t yiaddr;            /* Your IP (offered) */
    uint32_t siaddr;            /* Server IP */
    uint32_t giaddr;            /* Gateway IP */
    uint8_t  chaddr[16];        /* Client hardware address */
    uint8_t  sname[64];         /* Server name */
    uint8_t  file[128];         /* Boot filename */
    uint32_t magic;             /* Magic cookie */
    uint8_t  options[312];      /* DHCP options */
} __attribute__((packed));

/* --- Network config (set by DHCP) --- */
struct net_config {
    uint32_t ip;                /* Our IP (big-endian) */
    uint32_t subnet;            /* Subnet mask */
    uint32_t gateway;           /* Default gateway */
    uint32_t dns;               /* DNS server */
    uint8_t  mac[MAC_LEN];      /* Our MAC address */
    uint8_t  configured;        /* 1 if DHCP completed */
};

/* Global network configuration */
extern struct net_config net_cfg;

/* --- API --- */

/* Initialize the network stack (call after NIC init) */
void net_init(void);

/* Process a received packet */
void net_rx(void *data, uint32_t len);

/* Ethernet */
void eth_send(const uint8_t *dst_mac, uint16_t ethertype,
              const void *payload, uint32_t payload_len);

/* ARP */
void arp_init(void);
void arp_handle(const void *data, uint32_t len);
void arp_request(uint32_t target_ip);
int  arp_resolve(uint32_t ip, uint8_t *mac_out);

/* IPv4 */
void ipv4_handle(const void *data, uint32_t len);
void ipv4_send(uint32_t dst_ip, uint8_t protocol,
               const void *payload, uint32_t payload_len);
uint16_t ipv4_checksum(const void *data, uint32_t len);

/* ICMP */
void icmp_handle(uint32_t src_ip, const void *data, uint32_t len);
void icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void *payload, uint32_t payload_len);

/* UDP */
void udp_handle(uint32_t src_ip, const void *data, uint32_t len);
void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *payload, uint32_t payload_len);

/* DHCP */
void dhcp_discover(void);
void dhcp_handle(const void *data, uint32_t len);
