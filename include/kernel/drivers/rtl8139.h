/* ============================================================================
 * rtl8139.h — RTL8139 NIC driver
 *
 * Supports basic packet send/receive for the RTL8139 network card.
 * Requires PCI bus driver for device detection and BAR retrieval.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* RTL8139 PCI vendor/device IDs */
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

/* Maximum packet size */
#define RTL8139_MTU  1500

/* Initialize the RTL8139 NIC. Returns 0 on success, -1 if not found. */
int rtl8139_init(void);

/* Send a raw Ethernet frame. Returns 0 on success, -1 on error. */
int rtl8139_send(const void *data, uint32_t len);

/* Get the MAC address (6 bytes). */
void rtl8139_get_mac(uint8_t mac[6]);

/* Check if a packet has been received. Returns packet length or 0. */
uint32_t rtl8139_receive(void *buf, uint32_t buf_size);
