/* ============================================================================
 * pci.h — PCI bus driver
 *
 * Configuration space access via I/O ports 0xCF8 (address) / 0xCFC (data).
 * Supports bus enumeration and device lookup by vendor/device ID.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* PCI configuration address port */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI configuration space offsets */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_CLASS        0x0B
#define PCI_SUBCLASS     0x0A
#define PCI_PROG_IF      0x09
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_IRQ_LINE     0x3C
#define PCI_IRQ_PIN      0x3D

/* PCI command register bits */
#define PCI_CMD_IO_SPACE      (1 << 0)
#define PCI_CMD_MEM_SPACE     (1 << 1)
#define PCI_CMD_BUS_MASTER    (1 << 2)
#define PCI_CMD_INT_DISABLE   (1 << 10)

/* Maximum PCI limits */
#define PCI_MAX_BUS   256
#define PCI_MAX_DEV   32
#define PCI_MAX_FUNC  8

/* PCI device descriptor */
struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];         /* Base Address Registers */
    uint8_t  found;          /* 1 if valid */
};

/* Read/write PCI configuration space */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                     uint32_t value);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                     uint16_t value);

/* Scan all PCI buses and log found devices */
void pci_scan(void);

/* Find a device by vendor/device ID. Returns a pci_device struct.
 * Check .found == 1 to see if it was found. */
struct pci_device pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Enable bus mastering for a PCI device (required for DMA) */
void pci_enable_bus_mastering(struct pci_device *dev);
