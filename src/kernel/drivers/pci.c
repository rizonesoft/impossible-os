/* ============================================================================
 * pci.c — PCI bus driver
 *
 * Configuration space access via I/O ports 0xCF8/0xCFC.
 * Scans all bus/device/function combinations and logs found devices.
 * ============================================================================ */

#include "kernel/drivers/pci.h"
#include "kernel/printk.h"

/* --- Port I/O --- */
static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw_pci(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint8_t inb_pci(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* --- PCI configuration address ---
 * Bit 31:    enable
 * Bits 23-16: bus
 * Bits 15-11: device
 * Bits 10-8:  function
 * Bits 7-0:   offset (aligned to 4 bytes) */
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint32_t)(0x80000000
        | ((uint32_t)bus  << 16)
        | ((uint32_t)dev  << 11)
        | ((uint32_t)func << 8)
        | ((uint32_t)off  & 0xFC));
}

/* --- Read/Write PCI configuration space --- */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint16_t value)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    uint32_t old = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    old &= ~(0xFFFF << shift);
    old |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, old);
}

/* --- PCI bus scan --- */

static const char *pci_class_name(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x01:
        switch (sub) {
        case 0x01: return "IDE Controller";
        case 0x06: return "SATA Controller";
        default:   return "Storage";
        }
    case 0x02:
        switch (sub) {
        case 0x00: return "Ethernet";
        default:   return "Network";
        }
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x80: return "Other Bridge";
        default:   return "Bridge";
        }
    case 0x0C:
        switch (sub) {
        case 0x03: return "USB Controller";
        default:   return "Serial Bus";
        }
    default: return "Unknown";
    }
}

void pci_scan(void)
{
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t cls, sub, hdr;
    int count = 0;

    printk("[PCI] Scanning buses...\n");

    for (bus = 0; bus < 255; bus++) {
        for (dev = 0; dev < PCI_MAX_DEV; dev++) {
            for (func = 0; func < PCI_MAX_FUNC; func++) {
                vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor == 0xFFFF)
                    continue;

                device = pci_read16(bus, dev, func, PCI_DEVICE_ID);
                cls = pci_read8(bus, dev, func, PCI_CLASS);
                sub = pci_read8(bus, dev, func, PCI_SUBCLASS);

                printk("[PCI] %u:%u.%u  %x:%x  %s\n",
                       (uint64_t)bus, (uint64_t)dev, (uint64_t)func,
                       (uint64_t)vendor, (uint64_t)device,
                       pci_class_name(cls, sub));
                count++;

                /* If not multi-function, skip remaining functions */
                if (func == 0) {
                    hdr = pci_read8(bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & 0x80))
                        break;
                }
            }
        }
    }

    printk("[OK] PCI: %u devices found\n", (uint64_t)count);
}

struct pci_device pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    struct pci_device result;
    uint8_t bus, dev, func;
    uint16_t v, d;
    int i;

    result.found = 0;

    for (bus = 0; bus < 255; bus++) {
        for (dev = 0; dev < PCI_MAX_DEV; dev++) {
            for (func = 0; func < PCI_MAX_FUNC; func++) {
                v = pci_read16(bus, dev, func, PCI_VENDOR_ID);
                if (v == 0xFFFF)
                    continue;

                d = pci_read16(bus, dev, func, PCI_DEVICE_ID);
                if (v == vendor_id && d == device_id) {
                    result.bus = bus;
                    result.dev = dev;
                    result.func = func;
                    result.vendor_id = v;
                    result.device_id = d;
                    result.class_code = pci_read8(bus, dev, func, PCI_CLASS);
                    result.subclass = pci_read8(bus, dev, func, PCI_SUBCLASS);
                    result.prog_if = pci_read8(bus, dev, func, PCI_PROG_IF);
                    result.irq_line = pci_read8(bus, dev, func, PCI_IRQ_LINE);
                    for (i = 0; i < 6; i++)
                        result.bar[i] = pci_read32(bus, dev, func,
                                                    PCI_BAR0 + i * 4);
                    result.found = 1;
                    return result;
                }

                /* Multi-function check */
                if (func == 0) {
                    uint8_t hdr = pci_read8(bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & 0x80))
                        break;
                }
            }
        }
    }

    return result;
}

void pci_enable_bus_mastering(struct pci_device *dev)
{
    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
    pci_write16(dev->bus, dev->dev, dev->func, PCI_COMMAND, cmd);
}
