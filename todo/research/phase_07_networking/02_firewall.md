# 46 — Firewall

## Overview

Basic network packet filtering — allow/block connections by port, IP, or application.

---

## Design

```c
/* firewall.h */
typedef struct {
    uint32_t src_ip;        /* 0 = any */
    uint32_t dst_ip;        /* 0 = any */
    uint16_t src_port;      /* 0 = any */
    uint16_t dst_port;      /* 0 = any */
    uint8_t  protocol;      /* TCP=6, UDP=17, 0=any */
    uint8_t  action;        /* FW_ALLOW or FW_BLOCK */
    uint8_t  direction;     /* FW_INBOUND or FW_OUTBOUND */
} fw_rule_t;

int  fw_add_rule(const fw_rule_t *rule);
int  fw_remove_rule(int rule_id);
int  fw_check(const struct ip_header *pkt, uint8_t direction); /* returns ALLOW/BLOCK */
void fw_set_default(uint8_t action);  /* default policy: allow or block */
```

## Default rules
- Allow all outbound traffic
- Block all inbound except established connections
- Allow ICMP (ping)
- Allow DHCP (port 67/68)
- Allow DNS (port 53)

## Hook: called from `ip.c` on every incoming/outgoing packet
## Settings: `firewall.spl` in Settings Panel
## Codex: `System\Network\Firewall\Enabled`, `System\Network\Firewall\Rules`

## Files: `src/kernel/net/firewall.c` (~200 lines)
## Implementation: 3-5 days
