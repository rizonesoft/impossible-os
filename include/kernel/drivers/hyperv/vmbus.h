/* ============================================================================
 * vmbus.h — Hyper-V VMBus paravirtualization core
 *
 * Clean-room implementation from the public Hyper-V Top-Level Functional
 * Specification (TLFS). NO code derived from Linux hv_vmbus.c (GPL).
 *
 * Reference: https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- CPUID Leaves ---- */

#define HV_CPUID_VENDOR_AND_MAX_FUNCTIONS   0x40000000
#define HV_CPUID_INTERFACE                  0x40000001
#define HV_CPUID_FEATURES                   0x40000003

/* Expected interface signature: "Hv#1" = 0x31237648 */
#define HV_HYPERVISOR_INTERFACE_SIGNATURE   0x31237648u

/* ---- Hyper-V MSRs (x86-64) ---- */

#define HV_X64_MSR_GUEST_OS_ID      0x40000000
#define HV_X64_MSR_HYPERCALL        0x40000001
#define HV_X64_MSR_VP_INDEX         0x40000003

/* SynIC (Synthetic Interrupt Controller) MSRs */
#define HV_X64_MSR_SCONTROL         0x40000080  /* SynIC control */
#define HV_X64_MSR_SVERSION         0x40000081  /* SynIC version (read-only) */
#define HV_X64_MSR_SIEFP            0x40000082  /* Synthetic Interrupt Event Flags Page */
#define HV_X64_MSR_SIMP             0x40000083  /* Synthetic Interrupt Message Page */
#define HV_X64_MSR_EOM              0x40000084  /* End of Message */
#define HV_X64_MSR_SINT0            0x40000090  /* Synthetic Interrupt Source 0 */
#define HV_X64_MSR_SINT15           0x4000009F  /* Synthetic Interrupt Source 15 */

/* VMBus conventionally uses SINT2 */
#define VMBUS_MESSAGE_SINT          2
#define HV_X64_MSR_SINT_VMBUS       (HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT)

/* SINT register bits */
#define HV_SINT_MASKED              (1ULL << 16)
#define HV_SINT_AUTO_EOI            (1ULL << 17)
#define HV_SINT_VECTOR_MASK         0xFFULL

/* IDT vector for VMBus SINT2 interrupt */
#define VMBUS_INTERRUPT_VECTOR      0xF0

/* SynIC control bits */
#define HV_SCONTROL_ENABLE          (1ULL << 0)

/* SIMP / SIEFP register bits */
#define HV_SIMP_ENABLE              (1ULL << 0)
#define HV_SIEFP_ENABLE             (1ULL << 0)

/* ---- Guest OS ID encoding (TLFS §2.4.3, open-source format) ---- */

/* Bits 63:48 = 0 (open-source)
 * Bits 47:40 = OS ID (use 0x01 = "custom")
 * Bits 39:32 = major version
 * Bits 31:24 = minor version
 * Bits 23:16 = service version
 * Bits 15:0  = build number */
#define HV_GUEST_OS_ID_IMPOSSIBLE   \
    ((0x01ULL << 40) | (1ULL << 32) | (0ULL << 24) | (0ULL << 16) | 1ULL)

/* ---- Hypercall page ---- */

#define HV_HYPERCALL_ENABLE         (1ULL << 0)

/* ---- SynIC Message structures (TLFS §11.2) ---- */

/* HV_MESSAGE_TYPE — only the ones we care about */
#define HV_MESSAGE_TYPE_NONE            0x00000000
#define HV_MESSAGE_TYPE_CHANNEL         0x00000001

/* SynIC message header + payload = 256 bytes */
#define HV_MESSAGE_SIZE                 256
#define HV_MESSAGE_PAYLOAD_SIZE         240
#define HV_MESSAGE_MAX_PAYLOAD_QWORD    30

struct hv_message_header {
    uint32_t message_type;
    uint8_t  payload_size;     /* in bytes, max 240 */
    uint8_t  flags;            /* bit 0 = message_pending */
    uint8_t  reserved[2];
    uint64_t sender;           /* port ID or partition ID */
} __attribute__((packed));

struct hv_message {
    struct hv_message_header header;
    uint8_t                  payload[HV_MESSAGE_PAYLOAD_SIZE];
} __attribute__((packed));

/* SIM page: 16 message slots (one per SINT) */
#define HV_SYNIC_SINT_COUNT             16

struct hv_synic_message_page {
    struct hv_message messages[HV_SYNIC_SINT_COUNT];
} __attribute__((packed));

/* SynIC Event Flags page: 16 × 256 bits = 16 × 32 bytes = 512 bytes per SINT */
struct hv_synic_event_flags {
    uint32_t flags[64];  /* 2048 bits = 256 event flags, 8 groups of 32 */
} __attribute__((packed));

struct hv_synic_event_flags_page {
    struct hv_synic_event_flags sint[HV_SYNIC_SINT_COUNT];
} __attribute__((packed));

/* ---- Monitor page (TLFS §7.2) ---- */

/* Each trigger group has a 32-bit pending field.
 * Channel monitorid maps to: group = monitorid / 32, bit = monitorid % 32. */
struct hv_monitor_trigger_group {
    uint32_t pending;       /* bitmask of pending channel signals */
} __attribute__((packed));

/* hv_monitor_page — 4 KiB shared structure for passive signaling.
 * The hypervisor hardware monitors trigger_group[].pending bits and
 * fires coalesced synthetic interrupts, avoiding per-signal VM-exits. */
struct hv_monitor_page {
    uint32_t trigger_state;                      /* 0x000: aggregate trigger status  */
    uint32_t rsvdz1;                             /* 0x004                            */
    struct hv_monitor_trigger_group trigger_group[4]; /* 0x008: 4 groups × 32 bits    */
    uint8_t  rsvdz2[3];                          /* 0x018: padding                   */
    uint8_t  rsvdz3[1];                          /* 0x01B                            */
    uint32_t rsvdz4[3];                          /* 0x01C                            */
    uint8_t  rsvdz5[4];                          /* 0x028                            */
    uint8_t  rsvdz6[4];                          /* 0x02C                            */
    int32_t  next_checktime[4][32];               /* 0x030: timing (100ns units)      */
    uint16_t latency[4][32];                      /* 0x230: latency hints             */
    uint64_t parameter[4][32];                    /* 0x430: connection ID + flag      */
} __attribute__((packed));

/* ---- VMBus protocol versions ---- */

#define VMBUS_VERSION_WIN10_V5_2        0x00060000  /* Windows 10 RS5+ */
#define VMBUS_VERSION_WIN10             0x00050000  /* Windows 10 RTM */
#define VMBUS_VERSION_WIN8_1            0x00040000  /* Windows 8.1 */

/* ---- VMBus channel message types ---- */

#define CHANNELMSG_INVALID              0
#define CHANNELMSG_OFFERCHANNEL         1
#define CHANNELMSG_RESCIND_CHANNELOFFER 2
#define CHANNELMSG_REQUESTOFFERS        3
#define CHANNELMSG_ALLOFFERS_DELIVERED  4
#define CHANNELMSG_OPENCHANNEL          5
#define CHANNELMSG_OPENCHANNEL_RESULT   6
#define CHANNELMSG_CLOSECHANNEL         7
#define CHANNELMSG_GPADL_HEADER         8
#define CHANNELMSG_GPADL_BODY           9
#define CHANNELMSG_GPADL_CREATED        10
#define CHANNELMSG_GPADL_TEARDOWN       11
#define CHANNELMSG_GPADL_TORNDOWN       12
#define CHANNELMSG_INITIATE_CONTACT     14
#define CHANNELMSG_VERSION_RESPONSE     15

/* ---- VMBus channel message header ---- */

struct vmbus_channel_msg_header {
    uint32_t msg_type;
    uint32_t padding;
} __attribute__((packed));

/* ---- VMBus INITIATE_CONTACT message ---- */

struct vmbus_channel_initiate_contact {
    struct vmbus_channel_msg_header header;
    uint32_t vmbus_version_requested;
    uint32_t target_vcpu;           /* which vCPU receives responses */
    uint64_t interrupt_page_gpa;    /* deprecated in modern versions, set to 0 */
    uint64_t monitor_page1_gpa;
    uint64_t monitor_page2_gpa;
} __attribute__((packed));

/* ---- VMBus VERSION_RESPONSE message ---- */

struct vmbus_channel_version_response {
    struct vmbus_channel_msg_header header;
    uint8_t  version_supported;     /* 1 = accepted, 0 = rejected */
    uint8_t  connection_state;
    uint16_t padding;
} __attribute__((packed));

/* ---- GUID type (128-bit, little-endian Microsoft format) ---- */

struct hv_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} __attribute__((packed));

/* Well-known VMBus channel GUIDs */
#define HV_GUID_STORVSC     { 0xBA6163D9, 0x04A1, 0x4D29, \
    { 0xB6, 0x05, 0x72, 0xE2, 0xFF, 0xB1, 0xDC, 0x7F } }
#define HV_GUID_KBD         { 0xF912AD6D, 0x2B17, 0x48EA, \
    { 0xBD, 0x65, 0xF9, 0x27, 0xA6, 0x1C, 0x76, 0x84 } }
#define HV_GUID_VIDEO       { 0xDA0A7802, 0xE377, 0x4AAC, \
    { 0x8E, 0x77, 0x05, 0x58, 0xEB, 0x10, 0x73, 0xF8 } }
#define HV_GUID_NETVSC      { 0xF8615163, 0xDF3E, 0x46C5, \
    { 0x91, 0x3F, 0xF2, 0xD2, 0xF9, 0x65, 0xED, 0x0E } }

/* ---- VMBus OFFERCHANNEL message ---- */

struct vmbus_channel_offer_channel {
    struct vmbus_channel_msg_header header;
    struct hv_guid                  type_guid;       /* device type */
    struct hv_guid                  instance_guid;   /* unique instance */
    uint64_t                        interrupt_latency_in_100ns;
    uint32_t                        interface_ver;
    uint16_t                        child_relid;     /* channel ID */
    uint8_t                         is_dedicated;
    uint8_t                         monitor_id;
    uint32_t                        monitor_allocated;
    uint16_t                        server_context_area_size;
    uint16_t                        padding;
    /* Sub-channel info and connection ID follow in newer versions */
} __attribute__((packed));

/* ---- VMBus GPADL (Guest Physical Address Descriptor List) ---- */

/* GPADL_HEADER: sent by guest to create a memory mapping shared with host */
struct vmbus_channel_gpadl_header {
    struct vmbus_channel_msg_header header;
    uint32_t child_relid;       /* channel ID */
    uint32_t gpadl;             /* unique GPADL handle (guest-chosen) */
    uint16_t range_buflen;      /* total size of GPA range descriptor */
    uint16_t rangecount;        /* always 1 for contiguous allocations */
    /* Followed by: GPA range descriptor (offset, length, PFN list) */
    uint32_t range_offset;      /* byte offset within first page (0) */
    uint32_t range_len;         /* total bytes in the range */
    uint64_t pfn_array[32];     /* page frame numbers (enough for 128 KiB) */
} __attribute__((packed));

/* GPADL_CREATED: response from host confirming GPADL setup */
struct vmbus_channel_gpadl_created {
    struct vmbus_channel_msg_header header;
    uint32_t child_relid;
    uint32_t gpadl;
    uint32_t creation_status;   /* 0 = success */
} __attribute__((packed));

/* ---- VMBus OPENCHANNEL / OPENCHANNEL_RESULT ---- */

struct vmbus_channel_open_channel {
    struct vmbus_channel_msg_header header;
    uint32_t child_relid;
    uint32_t open_id;           /* unique open ID (guest-chosen, same as relid) */
    uint32_t ring_buffer_gpadl_handle;
    uint32_t target_vp;         /* target vCPU (0 = BSP) */
    uint32_t downstream_ring_buffer_offset;  /* offset of recv ring within GPADL */
    uint8_t  user_data[120];    /* channel-specific open data */
} __attribute__((packed));

struct vmbus_channel_open_result {
    struct vmbus_channel_msg_header header;
    uint32_t child_relid;
    uint32_t open_id;
    uint32_t status;            /* 0 = success */
} __attribute__((packed));

/* ---- VMBus ring buffer (shared memory with host) ---- */

#define VMBUS_RING_BUFFER_SIZE      (16 * 4096)  /* 64 KiB per ring */

/* Ring buffer feature bits (written into feature_bits of the ring header) */
#define HV_RING_BUFFER_FEAT_PENDING_SZ  (1u << 2)   /* supports pending_send_sz flow control */

struct vmbus_ring_buffer_header {
    uint32_t write_index;       /* guest writes here (send) / host writes here (recv) */
    uint32_t read_index;        /* host reads here (send)  / guest reads here (recv) */
    uint32_t interrupt_mask;    /* 1 = suppress partner signals during bulk processing */
    uint32_t pending_send_size; /* flow control: required bytes for a pending send */
    uint32_t reserved[10];      /* pad */
    uint32_t feature_bits;      /* capability flags (bit 2 = feat_pending_send_sz) */
    uint32_t reserved2;         /* pad to 64 bytes */
} __attribute__((packed));

/* ---- VMBus packet descriptor (TLFS §11.10) ---- */

/* Packet types for the ring buffer framing layer */
#define VMBUS_PACKET_TYPE_DATA_INBAND       6   /* data embedded in ring */
#define VMBUS_PACKET_TYPE_DATA_XFER_PAGES   7   /* data via transfer pages */
#define VMBUS_PACKET_TYPE_COMPLETION        11  /* completion notification */

/* Packet flags */
#define VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED  0x0001

/* Every packet in the ring buffer starts with this 16-byte descriptor.
 * Sizes are in 8-byte (qword) units. */
struct vmpacket_descriptor {
    uint16_t type;       /* VMBUS_PACKET_TYPE_* */
    uint16_t offset8;    /* offset to payload in 8-byte units */
    uint16_t len8;       /* total packet length in 8-byte units */
    uint16_t flags;      /* VMBUS_DATA_PACKET_FLAG_* */
    uint64_t trans_id;   /* unique transaction ID for request matching */
} __attribute__((packed));

/* Transfer page range for VMBUS_PACKET_TYPE_DATA_XFER_PAGES packets.
 * Used by StorVSC to reference the data buffer GPADL. */
struct vmbus_transfer_page_range {
    uint32_t byte_count;
    uint32_t byte_offset;
} __attribute__((packed));

/* Header for transfer-page packets. Follows vmpacket_descriptor in ring. */
struct vmbus_transfer_page_header {
    uint16_t transfer_pageset_id;  /* matches GPADL handle of data buffer */
    uint8_t  sender_owns_set;     /* 1 = sender owns pages (for writes) */
    uint8_t  reserved;
    uint32_t range_count;          /* number of ranges that follow */
    /* Followed by range_count × vmbus_transfer_page_range */
} __attribute__((packed));

/* ---- VMBus channel callback (set by drivers, fired by ISR) ---- */

struct vmbus_channel;  /* forward declaration */
typedef void (*vmbus_channel_callback_t)(struct vmbus_channel *ch, void *ctx);

/* ---- VMBus channel state ---- */

#define VMBUS_MAX_CHANNELS          256

struct vmbus_channel {
    struct vmbus_channel_offer_channel offer;
    uint16_t                           child_relid;
    int                                is_open;
    uint32_t                           gpadl_handle;
    uint32_t                           connection_id;

    /* Ring buffers (shared with host) */
    void                              *ring_pages;   /* raw PMM allocation */
    uint32_t                           ring_page_count;
    struct vmbus_ring_buffer_header   *send_ring;
    struct vmbus_ring_buffer_header   *recv_ring;
    uint32_t                           ring_size;    /* bytes per ring (half of total) */
    uint8_t                           *send_data;    /* points past send_ring header */
    uint8_t                           *recv_data;    /* points past recv_ring header */
    uint32_t                           data_size;    /* ring_size - sizeof(header) */

    /* Per-channel callback (fired by VMBus ISR on SIEF event) */
    vmbus_channel_callback_t           callback;
    void                              *callback_ctx;
};

/* ---- Public API ---- */

/* Returns 1 if running on Hyper-V, 0 otherwise.
 * Safe to call on any platform — uses CPUID. */
int hv_detect(void);

/* Initialize VMBus: hypercall page + SynIC + connect + enumerate.
 * Returns 0 on success, -1 if not on Hyper-V or init fails.
 * Safe to call on non-Hyper-V platforms (returns -1 immediately). */
int vmbus_init(void);

/* Return number of offered channels after vmbus_init(). */
int vmbus_get_channel_count(void);

/* Get the offer for channel at index (0-based). Returns NULL if invalid. */
const struct vmbus_channel_offer_channel *vmbus_get_offer(int idx);

/* Check if a channel's type GUID matches a known GUID. */
int hv_guid_equal(const struct hv_guid *a, const struct hv_guid *b);

/* Find a channel by its type GUID. Returns channel pointer or NULL. */
struct vmbus_channel *vmbus_find_channel_by_guid(const struct hv_guid *guid);

/* Open a VMBus channel: allocate ring buffers (PMM), create GPADL,
 * send OPENCHANNEL, wait for result.
 * ring_page_count: total pages for both rings (split in half).
 *   16 = 32 KiB per ring (send + recv).
 * Returns 0 on success, -1 on failure. */
int vmbus_open_channel(struct vmbus_channel *ch, uint32_t ring_page_count);

/* ---- Ring buffer raw I/O (internal, used by sendpacket wrappers) ---- */

/* Write raw bytes to send ring. Returns 0 on success, -1 if full. */
int vmbus_ring_write(struct vmbus_channel *ch,
                     const void *data, uint32_t len);

/* Read raw bytes from receive ring.
 * Returns bytes read (0 = empty). */
uint32_t vmbus_ring_read(struct vmbus_channel *ch,
                         void *buf, uint32_t max_len);

/* ---- High-level packet send/recv (correct vmpacket_descriptor framing) ---- */

/* Send an in-band data packet (control messages, storvsc init, etc.).
 * Wraps data with vmpacket_descriptor + trailing previous_pkt_offset.
 * Returns 0 on success. */
int vmbus_sendpacket(struct vmbus_channel *ch,
                     const void *data, uint32_t len,
                     uint64_t trans_id, uint16_t type, uint16_t flags);

/* Receive a packet from the recv ring. Strips vmpacket_descriptor header.
 * Writes payload into buf (up to max_len). Returns payload bytes, 0 = empty.
 * If out_trans_id is non-NULL, the transaction ID is written there. */
uint32_t vmbus_recvpacket(struct vmbus_channel *ch,
                          void *buf, uint32_t max_len,
                          uint64_t *out_trans_id);

/* Send a transfer-page packet (SCSI READ/WRITE data buffers).
 * References a GPADL-shared buffer via transfer_pageset_id.
 * The header_data is the VSTOR_PACKET (or similar) control message.
 * page_ranges describe which portions of the GPADL buffer are involved. */
int vmbus_sendpacket_pagebuffer(struct vmbus_channel *ch,
                                const void *header_data, uint32_t header_len,
                                uint64_t trans_id,
                                uint16_t transfer_pageset_id,
                                const struct vmbus_transfer_page_range *ranges,
                                uint32_t range_count);

/* Create a GPADL for an externally-allocated buffer (e.g. StorVSC transfer
 * buffer). The buffer must be PMM-allocated (identity-mapped, contiguous).
 * Returns 0 on success and writes GPADL handle to *out_handle. */
int vmbus_create_gpadl_external(struct vmbus_channel *ch, void *buffer,
                                uint32_t page_count, uint32_t *out_handle);

/* Signal the host that data is available on the send ring.
 * Uses monitor page (passive) for channels with monitor_allocated,
 * falls back to HvCallSignalEvent hypercall otherwise. */
void vmbus_signal_channel(struct vmbus_channel *ch);

/* Register a per-channel callback, fired by VMBus ISR when the host
 * sets the channel's event flag bit.  Pass NULL to unregister. */
void vmbus_set_channel_callback(struct vmbus_channel *ch,
                                vmbus_channel_callback_t cb, void *ctx);
