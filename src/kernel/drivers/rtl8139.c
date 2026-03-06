/* ============================================================================
 * rtl8139.c — RTL8139 NIC driver
 *
 * Simple network interface controller driver for the Realtek RTL8139.
 * Uses PCI I/O space (BAR0) for register access.
 *
 * References:
 *   - RTL8139 programming guide
 *   - OSDev wiki: RTL8139
 * ============================================================================ */

#include "rtl8139.h"
#include "pci.h"
#include "idt.h"
#include "pic.h"
#include "printk.h"
#include "heap.h"
#include "net.h"

/* --- Port I/O --- */
static inline void outb_nic(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_nic(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl_nic(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_nic(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw_nic(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl_nic(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* --- RTL8139 Register offsets (from I/O base) --- */
#define REG_MAC0       0x00  /* MAC address bytes 0-3 */
#define REG_MAC4       0x04  /* MAC address bytes 4-5 */
#define REG_TSD0       0x10  /* Transmit Status Descriptor 0 */
#define REG_TSAD0      0x20  /* Transmit Start Address Descriptor 0 */
#define REG_RBSTART    0x30  /* Receive Buffer Start Address */
#define REG_CMD        0x37  /* Command register */
#define REG_CAPR       0x38  /* Current Address of Packet Read (read ptr) */
#define REG_CBR        0x3A  /* Current Buffer Address (write ptr) */
#define REG_IMR        0x3C  /* Interrupt Mask Register */
#define REG_ISR        0x3E  /* Interrupt Status Register */
#define REG_TCR        0x40  /* Transmit Configuration Register */
#define REG_RCR        0x44  /* Receive Configuration Register */
#define REG_CONFIG1    0x52  /* Configuration Register 1 */

/* Command register bits */
#define CMD_RST        0x10  /* Reset */
#define CMD_RE         0x08  /* Receiver Enable */
#define CMD_TE         0x04  /* Transmitter Enable */

/* Interrupt status/mask bits */
#define INT_ROK        0x0001  /* Receive OK */
#define INT_RER        0x0002  /* Receive Error */
#define INT_TOK        0x0004  /* Transmit OK */
#define INT_TER        0x0008  /* Transmit Error */
#define INT_RX_OVERFLOW 0x0010 /* Rx Buffer Overflow */
#define INT_LINK_CHG   0x0020  /* Link Change */
#define INT_TIMEOUT    0x4000  /* Timeout */

/* Receive Configuration Register bits */
#define RCR_AAP        (1 << 0)   /* Accept All Packets (promiscuous) */
#define RCR_APM        (1 << 1)   /* Accept Physical Match */
#define RCR_AM         (1 << 2)   /* Accept Multicast */
#define RCR_AB         (1 << 3)   /* Accept Broadcast */
#define RCR_WRAP       (1 << 7)   /* Rx buffer wrap mode */

/* Transmit Status register bits */
#define TSD_OWN        (1 << 13)  /* DMA operation completed */
#define TSD_SIZE_MASK  0x1FFF     /* Packet size mask */

/* Rx packet header */
struct rtl8139_rx_header {
    uint16_t status;
    uint16_t length;
} __attribute__((packed));

/* Rx header status bits */
#define RX_ROK         (1 << 0)
#define RX_FAE         (1 << 1)   /* Frame Alignment Error */
#define RX_CRC         (1 << 2)   /* CRC Error */

/* --- Driver state --- */

/* Rx ring buffer: 8 KiB + 16 bytes (for wrap) + 1500 (for largest packet) */
#define RX_BUF_SIZE    (8192 + 16 + 1500)

/* Tx buffer: 4 descriptors, max 1792 bytes each */
#define TX_BUF_SIZE    1792
#define TX_DESC_COUNT  4

static uint16_t io_base;              /* I/O port base from BAR0 */
static uint8_t  mac_addr[6];          /* MAC address */
static uint8_t *rx_buffer;            /* Rx ring buffer (physical addr) */
static uint16_t rx_offset;            /* Current read offset in Rx buffer */
static uint8_t *tx_buffers[TX_DESC_COUNT]; /* Tx buffers */
static uint8_t  tx_cur;              /* Current Tx descriptor index */
static uint8_t  irq_line;            /* PCI IRQ line */
static volatile uint32_t rx_ready;   /* Packets waiting flag */

/* --- Simple memcpy (kernel-side) --- */
static void nic_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
}

/* --- IRQ handler --- */
static uint64_t rtl8139_irq_handler(struct interrupt_frame *frame)
{
    uint16_t status = inw_nic(io_base + REG_ISR);

    if (status & INT_ROK) {
        /* Drain all received packets from the ring buffer */
        uint8_t pkt_buf[ETH_FRAME_MAX];
        uint32_t pkt_len;
        while ((pkt_len = rtl8139_receive(pkt_buf, sizeof(pkt_buf))) > 0) {
            net_rx(pkt_buf, pkt_len);
        }
    }

    if (status & (INT_RER | INT_TER | INT_RX_OVERFLOW)) {
        printk("[RTL8139] Error: ISR=0x%x\n", (uint64_t)status);
    }

    /* Acknowledge all handled interrupts */
    outw_nic(io_base + REG_ISR, status);

    pic_send_eoi(irq_line);
    return (uint64_t)frame;
}

/* --- Public API --- */

int rtl8139_init(void)
{
    struct pci_device pci;
    int i;

    /* Find the RTL8139 on the PCI bus */
    pci = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!pci.found) {
        printk("[RTL8139] NIC not found on PCI bus\n");
        return -1;
    }

    printk("[RTL8139] Found at PCI %u:%u.%u, IRQ %u\n",
           (uint64_t)pci.bus, (uint64_t)pci.dev,
           (uint64_t)pci.func, (uint64_t)pci.irq_line);

    /* Get I/O base from BAR0 (bit 0 = I/O space indicator) */
    io_base = (uint16_t)(pci.bar[0] & ~0x3);
    irq_line = pci.irq_line;

    printk("[RTL8139] I/O base: 0x%x\n", (uint64_t)io_base);

    /* Enable PCI bus mastering (required for DMA) */
    pci_enable_bus_mastering(&pci);

    /* 1. Power on the NIC */
    outb_nic(io_base + REG_CONFIG1, 0x00);

    /* 2. Software reset */
    outb_nic(io_base + REG_CMD, CMD_RST);
    /* Wait for reset to complete (RST bit clears) */
    for (i = 0; i < 100000; i++) {
        if (!(inb_nic(io_base + REG_CMD) & CMD_RST))
            break;
    }
    if (i >= 100000) {
        printk("[RTL8139] Reset timeout!\n");
        return -1;
    }

    /* 3. Read MAC address */
    for (i = 0; i < 6; i++)
        mac_addr[i] = inb_nic(io_base + REG_MAC0 + i);

    printk("[RTL8139] MAC: %x:%x:%x:%x:%x:%x\n",
           (uint64_t)mac_addr[0], (uint64_t)mac_addr[1],
           (uint64_t)mac_addr[2], (uint64_t)mac_addr[3],
           (uint64_t)mac_addr[4], (uint64_t)mac_addr[5]);

    /* 4. Allocate Rx buffer (must be physically contiguous) */
    rx_buffer = (uint8_t *)kmalloc(RX_BUF_SIZE);
    if (!rx_buffer) {
        printk("[RTL8139] Cannot allocate Rx buffer\n");
        return -1;
    }
    /* Zero the buffer */
    for (i = 0; i < (int)RX_BUF_SIZE; i++)
        rx_buffer[i] = 0;
    rx_offset = 0;

    /* 5. Allocate Tx buffers */
    for (i = 0; i < TX_DESC_COUNT; i++) {
        tx_buffers[i] = (uint8_t *)kmalloc(TX_BUF_SIZE);
        if (!tx_buffers[i]) {
            printk("[RTL8139] Cannot allocate Tx buffer %d\n", (uint64_t)i);
            return -1;
        }
    }
    tx_cur = 0;

    /* 6. Write Rx buffer address to RBSTART
     * NOTE: Uses identity-mapped physical address (kernel heap is
     * identity-mapped in our simple memory setup) */
    outl_nic(io_base + REG_RBSTART, (uint32_t)(uint64_t)rx_buffer);

    /* 7. Set interrupt mask: ROK + TOK */
    outw_nic(io_base + REG_IMR, INT_ROK | INT_TOK | INT_RER |
                                 INT_TER | INT_RX_OVERFLOW);

    /* 8. Configure receive: accept broadcast, multicast,
     *    physical match, and enable wrap */
    outl_nic(io_base + REG_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_WRAP);

    /* 9. Configure transmit (use default DMA burst size) */
    outl_nic(io_base + REG_TCR, 0x03000000);  /* IFG=normal, DMA burst=2048 */

    /* 10. Enable receiver and transmitter */
    outb_nic(io_base + REG_CMD, CMD_RE | CMD_TE);

    /* 11. Register IRQ handler */
    idt_register_handler(PIC1_OFFSET + irq_line, rtl8139_irq_handler);
    pic_unmask_irq(irq_line);

    rx_ready = 0;

    printk("[OK] RTL8139 NIC initialized (IRQ %u)\n", (uint64_t)irq_line);
    return 0;
}

int rtl8139_send(const void *data, uint32_t len)
{
    uint8_t desc = tx_cur;

    if (len > TX_BUF_SIZE)
        return -1;

    /* Copy packet data to Tx buffer */
    nic_memcpy(tx_buffers[desc], data, len);

    /* Write Tx buffer address to TSAD[desc] */
    outl_nic(io_base + REG_TSAD0 + desc * 4,
             (uint32_t)(uint64_t)tx_buffers[desc]);

    /* Write packet size to TSD[desc] to start DMA.
     * Bit 13 (OWN) is clear → NIC owns the descriptor. */
    outl_nic(io_base + REG_TSD0 + desc * 4, len & TSD_SIZE_MASK);

    /* Advance to next descriptor */
    tx_cur = (tx_cur + 1) % TX_DESC_COUNT;

    return 0;
}

void rtl8139_get_mac(uint8_t mac[6])
{
    int i;
    for (i = 0; i < 6; i++)
        mac[i] = mac_addr[i];
}

uint32_t rtl8139_receive(void *buf, uint32_t buf_size)
{
    struct rtl8139_rx_header *hdr;
    uint16_t pkt_len;
    uint8_t cmd;

    /* Check if buffer is empty */
    cmd = inb_nic(io_base + REG_CMD);
    if (cmd & 0x01)  /* BUFE (Buffer Empty) bit */
        return 0;

    /* Read packet header from Rx ring buffer */
    hdr = (struct rtl8139_rx_header *)(rx_buffer + rx_offset);

    /* Validate status */
    if (!(hdr->status & RX_ROK)) {
        /* Bad packet — skip it */
        printk("[RTL8139] Bad Rx packet, status=0x%x\n",
               (uint64_t)hdr->status);
        /* Reset read pointer */
        rx_offset = inw_nic(io_base + REG_CBR) % RX_BUF_SIZE;
        return 0;
    }

    /* Packet length includes 4-byte CRC */
    pkt_len = hdr->length - 4;

    if (pkt_len > buf_size)
        pkt_len = (uint16_t)buf_size;

    /* Copy packet data (skip 4-byte header) */
    nic_memcpy(buf, rx_buffer + rx_offset + 4, pkt_len);

    /* Advance read offset: header (4) + length + align to DWORD */
    rx_offset = (uint16_t)((rx_offset + hdr->length + 4 + 3) & ~3);
    rx_offset %= RX_BUF_SIZE;

    /* Update CAPR (Current Address of Packet Read) */
    outw_nic(io_base + REG_CAPR, rx_offset - 16);

    if (rx_ready > 0)
        rx_ready--;

    return pkt_len;
}
