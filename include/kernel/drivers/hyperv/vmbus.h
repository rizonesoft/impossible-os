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

struct vmbus_ring_buffer_header {
    uint32_t write_index;
    uint32_t read_index;
    uint32_t interrupt_mask;
    uint32_t pending_send_size;
    uint32_t reserved[12];       /* pad to 64 bytes */
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

/* Write data to the channel's send ring buffer. Returns 0 on success. */
int vmbus_ring_write(struct vmbus_channel *ch,
                     const void *data, uint32_t len);

/* Read data from the channel's receive ring buffer.
 * Returns number of bytes read, or 0 if ring is empty. */
uint32_t vmbus_ring_read(struct vmbus_channel *ch,
                         void *buf, uint32_t max_len);

/* Signal the host that data is available on the send ring.
 * Uses the hypercall page to send an event. */
void vmbus_signal_channel(struct vmbus_channel *ch);

/* Register a per-channel callback, fired by VMBus ISR when the host
 * sets the channel's event flag bit.  Pass NULL to unregister. */
void vmbus_set_channel_callback(struct vmbus_channel *ch,
                                vmbus_channel_callback_t cb, void *ctx);
