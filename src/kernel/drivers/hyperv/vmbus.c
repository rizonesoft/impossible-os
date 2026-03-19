/* ============================================================================
 * vmbus.c — Hyper-V VMBus paravirtualization core
 *
 * Clean-room implementation from the public Hyper-V Top-Level Functional
 * Specification (TLFS). NO code derived from Linux hv_vmbus.c (GPL).
 *
 * Boot sequence:
 *   1. hv_detect()         — CPUID check for Hyper-V signature
 *   2. hv_setup_hypercall() — guest OS ID + hypercall page via MSRs
 *   3. hv_setup_synic()     — SIM/SIEF pages + SINT2 for VMBus
 *   4. vmbus_connect()      — negotiate protocol version with host
 *   5. vmbus_enumerate()    — request + receive channel offers
 *
 * Reference: https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs
 * ============================================================================ */

#include "kernel/drivers/hyperv/vmbus.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- MSR helpers ---- */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* ---- CPUID helper ---- */

static inline void cpuid(uint32_t leaf,
                          uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

/* ---- Memory helpers ---- */

static void vmbus_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

static void vmbus_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
}

/* ---- Internal state ---- */

static int hv_detected = -1;  /* -1 = not yet checked, 0 = no, 1 = yes */

/* Hypercall page (4 KiB, executable) */
static void *hypercall_page;

/* SynIC pages (per-BSP only for now) */
static struct hv_synic_message_page     *sim_page;
static struct hv_synic_event_flags_page *sief_page;

/* Monitor pages (required by VMBus INITIATE_CONTACT) */
static void *monitor_page1;
static void *monitor_page2;

/* VMBus connection state */
static int vmbus_connected;
static uint32_t vmbus_version;

/* Channel offers */
static struct vmbus_channel_offer_channel offers[VMBUS_MAX_CHANNELS];
static struct vmbus_channel channels[VMBUS_MAX_CHANNELS];
static int offer_count;
static volatile int offers_complete;

/* GPADL handle counter */
static uint32_t next_gpadl_handle = 0x100;

/* ---- §1. Hyper-V Detection ---- */

int hv_detect(void)
{
    uint32_t eax, ebx, ecx, edx;

    if (hv_detected >= 0)
        return hv_detected;

    /* Check CPUID.1 ECX bit 31: hypervisor present */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 31))) {
        hv_detected = 0;
        return 0;
    }

    /* Check CPUID leaf 0x40000000: vendor signature "Microsoft Hv" */
    cpuid(HV_CPUID_VENDOR_AND_MAX_FUNCTIONS, &eax, &ebx, &ecx, &edx);
    /* ebx = "Micr", ecx = "osof", edx = "t Hv" */
    if (ebx != 0x7263694D || ecx != 0x666F736F || edx != 0x76482074) {
        hv_detected = 0;
        return 0;
    }

    /* Check CPUID leaf 0x40000001: interface signature "Hv#1" */
    cpuid(HV_CPUID_INTERFACE, &eax, &ebx, &ecx, &edx);
    if (eax != HV_HYPERVISOR_INTERFACE_SIGNATURE) {
        hv_detected = 0;
        return 0;
    }

    hv_detected = 1;
    klog(LOG_INFO, "hyperv", "Hyper-V detected (interface Hv#1)");
    return 1;
}

/* ---- §2. Hypercall Page Setup ---- */

static int hv_setup_hypercall(void)
{
    uint64_t msr_val;
    uint64_t page_gpa;

    /* Step 1: Write guest OS identity (required before enabling hypercall page) */
    wrmsr(HV_X64_MSR_GUEST_OS_ID, HV_GUEST_OS_ID_IMPOSSIBLE);

    /* Step 2: Allocate a 4 KiB page for the hypercall trampoline */
    hypercall_page = (void *)(uintptr_t)pmm_alloc_contiguous(1);
    if (!hypercall_page) {
        klog(LOG_ERROR, "hyperv", "Failed to allocate hypercall page");
        return -1;
    }
    vmbus_memset(hypercall_page, 0, 4096);
    page_gpa = (uint64_t)(uintptr_t)hypercall_page;

    /* Step 3: Enable hypercall page — write GPA with enable bit */
    msr_val = (page_gpa & ~0xFFFULL) | HV_HYPERCALL_ENABLE;
    wrmsr(HV_X64_MSR_HYPERCALL, msr_val);

    /* Step 4: Verify enable bit reads back */
    msr_val = rdmsr(HV_X64_MSR_HYPERCALL);
    if (!(msr_val & HV_HYPERCALL_ENABLE)) {
        klog(LOG_ERROR, "hyperv", "Hypercall page enable failed (MSR=0x%x)",
             msr_val);
        return -1;
    }

    klog(LOG_INFO, "hyperv", "Hypercall page at 0x%x (enabled)",
         (uint64_t)page_gpa);
    return 0;
}

/* ---- §3. SynIC Setup ---- */

static int hv_setup_synic(void)
{
    uint64_t simp_val, siefp_val, sint_val;

    /* Allocate SIM page (Synthetic Interrupt Message Page) */
    sim_page = (struct hv_synic_message_page *)(uintptr_t)pmm_alloc_contiguous(1);
    if (!sim_page) {
        klog(LOG_ERROR, "hyperv", "Failed to allocate SIM page");
        return -1;
    }
    vmbus_memset(sim_page, 0, 4096);

    /* Allocate SIEF page (Synthetic Interrupt Event Flags Page) */
    sief_page = (struct hv_synic_event_flags_page *)(uintptr_t)pmm_alloc_contiguous(1);
    if (!sief_page) {
        klog(LOG_ERROR, "hyperv", "Failed to allocate SIEF page");
        return -1;
    }
    vmbus_memset(sief_page, 0, 4096);

    /* Enable SIM page: write GPA | enable bit to SIMP MSR */
    simp_val = ((uint64_t)(uintptr_t)sim_page & ~0xFFFULL) | HV_SIMP_ENABLE;
    wrmsr(HV_X64_MSR_SIMP, simp_val);

    /* Enable SIEF page: write GPA | enable bit to SIEFP MSR */
    siefp_val = ((uint64_t)(uintptr_t)sief_page & ~0xFFFULL) | HV_SIEFP_ENABLE;
    wrmsr(HV_X64_MSR_SIEFP, siefp_val);

    /* Configure SINT2 for VMBus:
     * - Vector = VMBUS_INTERRUPT_VECTOR (0xF0)
     * - Unmask (clear bit 16)
     * - AutoEOI (set bit 17) */
    sint_val = (uint64_t)VMBUS_INTERRUPT_VECTOR | HV_SINT_AUTO_EOI;
    /* Note: NOT setting HV_SINT_MASKED — we want it unmasked */
    wrmsr(HV_X64_MSR_SINT_VMBUS, sint_val);

    /* Enable SynIC globally */
    wrmsr(HV_X64_MSR_SCONTROL, HV_SCONTROL_ENABLE);

    klog(LOG_INFO, "hyperv",
         "SynIC enabled: SIM=0x%x SIEF=0x%x SINT2=vec 0x%x",
         (uint64_t)(uintptr_t)sim_page,
         (uint64_t)(uintptr_t)sief_page,
         (uint64_t)VMBUS_INTERRUPT_VECTOR);

    return 0;
}

/* ---- §4. VMBus Connection ---- */

/* Post a VMBus channel message to the hypervisor via the SIM page.
 * In practice, the host reads from a designated monitor area or the
 * guest writes to the hypervisor's message port. For the initial
 * INITIATE_CONTACT, we use a hypercall (HvCallPostMessage). */
static int vmbus_post_message(const void *msg, uint32_t msg_size)
{
    /* HvCallPostMessage hypercall:
     * Input: connection_id (1), message_type (1), payload
     * The hypercall page contains the vmcall/vmmcall instruction.
     * We use the simple (slow) hypercall path. */

    /* Aligned input buffer for HvCallPostMessage (must be 8-byte aligned) */
    struct {
        uint32_t connection_id;
        uint32_t reserved;
        uint32_t message_type;
        uint32_t payload_size;
        uint8_t  payload[240];
    } __attribute__((packed, aligned(8))) input;

    uint64_t status;
    uint64_t hypercall_input;
    uint64_t input_gpa;

    if (!hypercall_page)
        return -1;

    vmbus_memset(&input, 0, sizeof(input));
    input.connection_id = 1;   /* VMBus connection ID */
    input.message_type  = HV_MESSAGE_TYPE_CHANNEL;
    input.payload_size  = msg_size;

    if (msg_size > 240)
        return -1;
    vmbus_memcpy(input.payload, msg, msg_size);

    input_gpa = (uint64_t)(uintptr_t)&input;

    /* Hypercall number for HvCallPostMessage = 0x005C
     * Input: slow path (bit 16 = 0), input GPA in RDX
     * Output: status in RAX */
    hypercall_input = 0x005C;  /* HvCallPostMessage */

    __asm__ volatile(
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"
        "call *%3\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(status)
        : "r"(hypercall_input), "r"(input_gpa), "r"(hypercall_page)
        : "rax", "rcx", "rdx", "r8", "memory"
    );

    if (status != 0) {
        klog(LOG_ERROR, "hyperv", "HvCallPostMessage failed: status=0x%x",
             status);
        return -1;
    }

    return 0;
}

/* Wait for a message on SINT2 with timeout (simple polling) */
static struct hv_message *vmbus_wait_message(uint32_t timeout_ms)
{
    struct hv_message *msg = &sim_page->messages[VMBUS_MESSAGE_SINT];
    volatile uint32_t *msg_type = (volatile uint32_t *)((uintptr_t)&msg->header);
    uint32_t i;

    /* Simple spin-wait — each iteration is ~1 µs on modern CPUs */
    for (i = 0; i < timeout_ms * 1000; i++) {
        __asm__ volatile("pause" ::: "memory");
        if (*msg_type != HV_MESSAGE_TYPE_NONE)
            return msg;
    }

    return NULL;
}

/* Acknowledge a SynIC message: clear slot + write EOM */
static void vmbus_ack_message(struct hv_message *msg)
{
    uint8_t pending = msg->header.flags & 0x01;

    /* Clear the message slot */
    msg->header.message_type = HV_MESSAGE_TYPE_NONE;

    /* Memory barrier — ensure message is cleared before EOM */
    __asm__ volatile("mfence" ::: "memory");

    /* If more messages are pending, write EOM to drain the queue */
    if (pending)
        wrmsr(HV_X64_MSR_EOM, 0);
}

static int vmbus_negotiate_version(uint32_t version)
{
    struct vmbus_channel_initiate_contact contact;
    struct hv_message *response;
    struct vmbus_channel_version_response *ver_resp;

    vmbus_memset(&contact, 0, sizeof(contact));
    contact.header.msg_type          = CHANNELMSG_INITIATE_CONTACT;
    contact.vmbus_version_requested  = version;
    contact.target_vcpu              = 0;  /* BSP */
    contact.interrupt_page_gpa       = 0;  /* deprecated in modern versions */
    contact.monitor_page1_gpa        = (uint64_t)(uintptr_t)monitor_page1;
    contact.monitor_page2_gpa        = (uint64_t)(uintptr_t)monitor_page2;

    if (vmbus_post_message(&contact, sizeof(contact)) < 0) {
        klog(LOG_ERROR, "hyperv", "Failed to post INITIATE_CONTACT");
        return -1;
    }

    /* Wait for VERSION_RESPONSE (up to 5 seconds) */
    response = vmbus_wait_message(5000);
    if (!response) {
        klog(LOG_ERROR, "hyperv", "Timeout waiting for VERSION_RESPONSE");
        return -1;
    }

    /* The payload starts after the HV message header, which contains
     * the VMBus channel message */
    ver_resp = (struct vmbus_channel_version_response *)response->payload;

    if (ver_resp->header.msg_type != CHANNELMSG_VERSION_RESPONSE) {
        klog(LOG_WARN, "hyperv", "Unexpected message type %u (expected %u)",
             (uint64_t)ver_resp->header.msg_type,
             (uint64_t)CHANNELMSG_VERSION_RESPONSE);
        vmbus_ack_message(response);
        return -1;
    }

    int supported = ver_resp->version_supported;
    vmbus_ack_message(response);

    return supported ? 0 : -1;
}

static int vmbus_connect(void)
{
    /* Allocate monitor pages (required by INITIATE_CONTACT) */
    monitor_page1 = (void *)(uintptr_t)pmm_alloc_contiguous(1);
    monitor_page2 = (void *)(uintptr_t)pmm_alloc_contiguous(1);
    if (!monitor_page1 || !monitor_page2) {
        klog(LOG_ERROR, "hyperv", "Failed to allocate monitor pages");
        return -1;
    }
    vmbus_memset(monitor_page1, 0, 4096);
    vmbus_memset(monitor_page2, 0, 4096);

    /* Try highest version first, then fall back */
    klog(LOG_INFO, "hyperv", "Negotiating VMBus version 0x%x...",
         (uint64_t)VMBUS_VERSION_WIN10_V5_2);

    if (vmbus_negotiate_version(VMBUS_VERSION_WIN10_V5_2) == 0) {
        vmbus_version = VMBUS_VERSION_WIN10_V5_2;
        goto connected;
    }

    klog(LOG_INFO, "hyperv", "Falling back to VMBus version 0x%x...",
         (uint64_t)VMBUS_VERSION_WIN10);

    if (vmbus_negotiate_version(VMBUS_VERSION_WIN10) == 0) {
        vmbus_version = VMBUS_VERSION_WIN10;
        goto connected;
    }

    klog(LOG_INFO, "hyperv", "Falling back to VMBus version 0x%x...",
         (uint64_t)VMBUS_VERSION_WIN8_1);

    if (vmbus_negotiate_version(VMBUS_VERSION_WIN8_1) == 0) {
        vmbus_version = VMBUS_VERSION_WIN8_1;
        goto connected;
    }

    klog(LOG_ERROR, "hyperv", "VMBus version negotiation failed");
    return -1;

connected:
    vmbus_connected = 1;
    klog(LOG_INFO, "hyperv", "VMBus connected (version 0x%x)",
         (uint64_t)vmbus_version);
    return 0;
}

/* ---- §5. Channel Enumeration ---- */

static int vmbus_enumerate(void)
{
    struct vmbus_channel_msg_header request;
    struct hv_message *msg;
    int timeout_count;

    offer_count = 0;
    offers_complete = 0;

    /* Send REQUESTOFFERS to the hypervisor */
    vmbus_memset(&request, 0, sizeof(request));
    request.msg_type = CHANNELMSG_REQUESTOFFERS;

    if (vmbus_post_message(&request, sizeof(request)) < 0) {
        klog(LOG_ERROR, "hyperv", "Failed to post REQUESTOFFERS");
        return -1;
    }

    /* Collect channel offers until ALLOFFERS_DELIVERED */
    timeout_count = 0;
    while (!offers_complete && timeout_count < 100) {
        msg = vmbus_wait_message(100);  /* 100ms per attempt */
        if (!msg) {
            timeout_count++;
            continue;
        }

        struct vmbus_channel_msg_header *hdr =
            (struct vmbus_channel_msg_header *)msg->payload;

        switch (hdr->msg_type) {
        case CHANNELMSG_OFFERCHANNEL: {
            if (offer_count < VMBUS_MAX_CHANNELS) {
                vmbus_memcpy(&offers[offer_count], msg->payload,
                             sizeof(struct vmbus_channel_offer_channel));

                struct vmbus_channel_offer_channel *offer = &offers[offer_count];

                /* Populate channel state */
                vmbus_memcpy(&channels[offer_count].offer, offer,
                             sizeof(*offer));
                channels[offer_count].child_relid = offer->child_relid;
                channels[offer_count].is_open = 0;
                channels[offer_count].gpadl_handle = 0;
                channels[offer_count].connection_id = 0;
                channels[offer_count].ring_pages = NULL;

                klog(LOG_INFO, "hyperv",
                     "  Channel %u: type=%08x-%04x-%04x relid=%u",
                     (uint64_t)offer_count,
                     (uint64_t)offer->type_guid.data1,
                     (uint64_t)offer->type_guid.data2,
                     (uint64_t)offer->type_guid.data3,
                     (uint64_t)offer->child_relid);

                offer_count++;
            }
            break;
        }

        case CHANNELMSG_ALLOFFERS_DELIVERED:
            offers_complete = 1;
            break;

        default:
            klog(LOG_DEBUG, "hyperv", "  Unexpected msg type %u during enum",
                 (uint64_t)hdr->msg_type);
            break;
        }

        vmbus_ack_message(msg);
        timeout_count = 0;  /* Reset timeout on any message */
    }

    klog(LOG_INFO, "hyperv", "VMBus enumeration complete: %u channels offered",
         (uint64_t)offer_count);
    return 0;
}

/* ---- §6. GUID comparison ---- */

int hv_guid_equal(const struct hv_guid *a, const struct hv_guid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    int i;

    for (i = 0; i < 16; i++) {
        if (pa[i] != pb[i])
            return 0;
    }
    return 1;
}

/* ---- §7. Channel find by GUID ---- */

struct vmbus_channel *vmbus_find_channel_by_guid(const struct hv_guid *guid)
{
    int i;
    for (i = 0; i < offer_count; i++) {
        if (hv_guid_equal(&channels[i].offer.type_guid, guid))
            return &channels[i];
    }
    return NULL;
}

/* ---- §8. Channel open (GPADL + OPENCHANNEL) ---- */

static int vmbus_create_gpadl(struct vmbus_channel *ch, void *buffer,
                               uint32_t page_count, uint32_t *out_handle)
{
    struct vmbus_channel_gpadl_header gpadl_hdr;
    struct hv_message *response;
    struct vmbus_channel_gpadl_created *created;
    uint32_t i;
    uint32_t handle = next_gpadl_handle++;

    vmbus_memset(&gpadl_hdr, 0, sizeof(gpadl_hdr));
    gpadl_hdr.header.msg_type = CHANNELMSG_GPADL_HEADER;
    gpadl_hdr.child_relid     = ch->child_relid;
    gpadl_hdr.gpadl           = handle;
    gpadl_hdr.rangecount      = 1;

    /* GPA range descriptor */
    gpadl_hdr.range_offset = 0;
    gpadl_hdr.range_len   = page_count * 4096;

    /* Size of the range descriptor: offset(4) + len(4) + pfn_array(8*n) */
    gpadl_hdr.range_buflen = 8 + (uint16_t)(page_count * 8);

    /* Fill PFN array with page frame numbers */
    uint64_t base_pfn = (uint64_t)(uintptr_t)buffer >> 12;
    for (i = 0; i < page_count && i < 32; i++)
        gpadl_hdr.pfn_array[i] = base_pfn + i;

    /* Calculate actual message size (header + range descriptor).
     * We need: struct header (48 bytes) + pfn_array entries actually used */
    uint32_t msg_size = (uint32_t)((uintptr_t)&gpadl_hdr.pfn_array[page_count] -
                                    (uintptr_t)&gpadl_hdr);

    if (vmbus_post_message(&gpadl_hdr, msg_size) < 0) {
        klog(LOG_ERROR, "hyperv", "Failed to post GPADL_HEADER");
        return -1;
    }

    /* Wait for GPADL_CREATED response */
    response = vmbus_wait_message(5000);
    if (!response) {
        klog(LOG_ERROR, "hyperv", "Timeout waiting for GPADL_CREATED");
        return -1;
    }

    created = (struct vmbus_channel_gpadl_created *)response->payload;

    if (created->header.msg_type != CHANNELMSG_GPADL_CREATED ||
        created->creation_status != 0) {
        klog(LOG_ERROR, "hyperv",
             "GPADL creation failed (msg_type=%u, status=%u)",
             (uint64_t)created->header.msg_type,
             (uint64_t)created->creation_status);
        vmbus_ack_message(response);
        return -1;
    }

    vmbus_ack_message(response);
    *out_handle = handle;

    klog(LOG_DEBUG, "hyperv", "GPADL created: handle=0x%x pages=%u",
         (uint64_t)handle, (uint64_t)page_count);
    return 0;
}

int vmbus_open_channel(struct vmbus_channel *ch, uint32_t ring_page_count)
{
    struct vmbus_channel_open_channel open_msg;
    struct hv_message *response;
    struct vmbus_channel_open_result *result;
    uint32_t half_pages;
    uint32_t half_bytes;
    void *ring_buf;
    uint32_t gpadl_handle;

    if (!ch || ch->is_open)
        return -1;

    /* ring_page_count must be even (split between send and recv) */
    if (ring_page_count < 4 || (ring_page_count & 1))
        return -1;

    half_pages = ring_page_count / 2;
    half_bytes = half_pages * 4096;

    /* Allocate contiguous pages for both ring buffers */
    ring_buf = (void *)(uintptr_t)pmm_alloc_contiguous(ring_page_count);
    if (!ring_buf) {
        klog(LOG_ERROR, "hyperv",
             "Failed to allocate ring buffer (%u pages)", (uint64_t)ring_page_count);
        return -1;
    }
    vmbus_memset(ring_buf, 0, ring_page_count * 4096);

    /* Create GPADL for the ring buffer memory */
    if (vmbus_create_gpadl(ch, ring_buf, ring_page_count, &gpadl_handle) < 0) {
        return -1;
    }

    /* Set up channel ring buffer pointers */
    ch->ring_pages      = ring_buf;
    ch->ring_page_count = ring_page_count;
    ch->gpadl_handle    = gpadl_handle;
    ch->send_ring       = (struct vmbus_ring_buffer_header *)ring_buf;
    ch->recv_ring       = (struct vmbus_ring_buffer_header *)
                          ((uint8_t *)ring_buf + half_bytes);
    ch->ring_size       = half_bytes;
    ch->send_data       = (uint8_t *)ch->send_ring +
                          sizeof(struct vmbus_ring_buffer_header);
    ch->recv_data       = (uint8_t *)ch->recv_ring +
                          sizeof(struct vmbus_ring_buffer_header);
    ch->data_size       = half_bytes - sizeof(struct vmbus_ring_buffer_header);

    /* Send OPENCHANNEL message */
    vmbus_memset(&open_msg, 0, sizeof(open_msg));
    open_msg.header.msg_type              = CHANNELMSG_OPENCHANNEL;
    open_msg.child_relid                  = ch->child_relid;
    open_msg.open_id                      = ch->child_relid;  /* use relid as open_id */
    open_msg.ring_buffer_gpadl_handle     = gpadl_handle;
    open_msg.target_vp                    = 0;  /* BSP */
    open_msg.downstream_ring_buffer_offset = half_bytes;

    if (vmbus_post_message(&open_msg, sizeof(open_msg)) < 0) {
        klog(LOG_ERROR, "hyperv", "Failed to post OPENCHANNEL (relid=%u)",
             (uint64_t)ch->child_relid);
        return -1;
    }

    /* Wait for OPENCHANNEL_RESULT */
    response = vmbus_wait_message(5000);
    if (!response) {
        klog(LOG_ERROR, "hyperv", "Timeout waiting for OPENCHANNEL_RESULT");
        return -1;
    }

    result = (struct vmbus_channel_open_result *)response->payload;

    if (result->header.msg_type != CHANNELMSG_OPENCHANNEL_RESULT ||
        result->status != 0) {
        klog(LOG_ERROR, "hyperv",
             "OPENCHANNEL failed (msg_type=%u, status=%u)",
             (uint64_t)result->header.msg_type,
             (uint64_t)result->status);
        vmbus_ack_message(response);
        return -1;
    }

    vmbus_ack_message(response);
    ch->is_open = 1;

    klog(LOG_INFO, "hyperv",
         "Channel %u opened (GPADL=0x%x, ring=%u pages)",
         (uint64_t)ch->child_relid,
         (uint64_t)gpadl_handle,
         (uint64_t)ring_page_count);
    return 0;
}

/* ---- §9. Ring buffer I/O ---- */

int vmbus_ring_write(struct vmbus_channel *ch,
                     const void *data, uint32_t len)
{
    uint32_t write_idx;
    uint32_t avail;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t first_chunk, second_chunk;

    if (!ch || !ch->is_open || !ch->send_ring)
        return -1;

    write_idx = ch->send_ring->write_index;

    /* Calculate available space (simple single-producer model) */
    uint32_t read_idx = ch->send_ring->read_index;
    if (write_idx >= read_idx)
        avail = ch->data_size - (write_idx - read_idx) - 1;
    else
        avail = (read_idx - write_idx) - 1;

    /* Need space for data + 8-byte packet header (64-bit length prefix) */
    if (avail < len + 8)
        return -1;

    /* Write 64-bit length prefix */
    uint64_t pkt_len = (uint64_t)len;
    uint8_t *len_bytes = (uint8_t *)&pkt_len;
    uint32_t i;
    for (i = 0; i < 8; i++) {
        ch->send_data[write_idx % ch->data_size] = len_bytes[i];
        write_idx++;
    }

    /* Write data with wrap-around */
    first_chunk = ch->data_size - (write_idx % ch->data_size);
    if (first_chunk > len)
        first_chunk = len;

    vmbus_memcpy(&ch->send_data[write_idx % ch->data_size], src, first_chunk);
    second_chunk = len - first_chunk;
    if (second_chunk > 0)
        vmbus_memcpy(&ch->send_data[0], src + first_chunk, second_chunk);

    write_idx += len;

    /* Memory barrier before updating write index */
    __asm__ volatile("sfence" ::: "memory");
    ch->send_ring->write_index = write_idx % ch->data_size;

    return 0;
}

uint32_t vmbus_ring_read(struct vmbus_channel *ch,
                         void *buf, uint32_t max_len)
{
    uint32_t read_idx, write_idx;
    uint32_t avail;
    uint8_t *dst = (uint8_t *)buf;
    uint64_t pkt_len;
    uint32_t first_chunk, second_chunk;
    uint32_t i;

    if (!ch || !ch->is_open || !ch->recv_ring)
        return 0;

    /* Memory barrier: ensure we read fresh indices */
    __asm__ volatile("lfence" ::: "memory");

    read_idx  = ch->recv_ring->read_index;
    write_idx = ch->recv_ring->write_index;

    if (read_idx == write_idx)
        return 0;  /* Ring is empty */

    /* Calculate available bytes */
    if (write_idx >= read_idx)
        avail = write_idx - read_idx;
    else
        avail = ch->data_size - (read_idx - write_idx);

    if (avail < 8)
        return 0;  /* Not enough for length prefix */

    /* Read 64-bit length prefix */
    uint8_t *len_bytes = (uint8_t *)&pkt_len;
    for (i = 0; i < 8; i++) {
        len_bytes[i] = ch->recv_data[read_idx % ch->data_size];
        read_idx++;
    }

    if (pkt_len == 0 || pkt_len > max_len) {
        /* Packet too large for buffer — skip it */
        read_idx += (uint32_t)pkt_len;
        __asm__ volatile("sfence" ::: "memory");
        ch->recv_ring->read_index = read_idx % ch->data_size;
        return 0;
    }

    /* Read data with wrap-around */
    first_chunk = ch->data_size - (read_idx % ch->data_size);
    if (first_chunk > (uint32_t)pkt_len)
        first_chunk = (uint32_t)pkt_len;

    vmbus_memcpy(dst, &ch->recv_data[read_idx % ch->data_size], first_chunk);
    second_chunk = (uint32_t)pkt_len - first_chunk;
    if (second_chunk > 0)
        vmbus_memcpy(dst + first_chunk, &ch->recv_data[0], second_chunk);

    read_idx += (uint32_t)pkt_len;

    /* Update read index */
    __asm__ volatile("sfence" ::: "memory");
    ch->recv_ring->read_index = read_idx % ch->data_size;

    return (uint32_t)pkt_len;
}

/* ---- §10. Signal host ---- */

void vmbus_signal_channel(struct vmbus_channel *ch)
{
    /* HvCallSignalEvent hypercall (0x005D):
     * Sets an event flag bit for the target SINT.
     * The host polls these flags and processes the ring buffer. */
    uint64_t hypercall_input;
    uint64_t input_gpa;
    uint64_t status;

    /* The event flag connection ID for the channel is the child_relid.
     * Input param is the connection ID (child_relid) as a 64-bit value. */
    uint64_t connection_id_val = (uint64_t)ch->child_relid;
    input_gpa = (uint64_t)(uintptr_t)&connection_id_val;

    hypercall_input = 0x005D;  /* HvCallSignalEvent */

    if (!hypercall_page)
        return;

    __asm__ volatile(
        "mov %1, %%rcx\n\t"
        "mov %2, %%rdx\n\t"
        "call *%3\n\t"
        "mov %%rax, %0\n\t"
        : "=r"(status)
        : "r"(hypercall_input), "r"(input_gpa), "r"(hypercall_page)
        : "rax", "rcx", "rdx", "r8", "memory"
    );

    /* Ignore status — signaling is best-effort */
    (void)status;
}

/* ---- §11. VMBus ISR (called from IDT vector 0xF0) ---- */

/* This ISR will be registered in the IDT by the interrupt subsystem.
 * For now, we use polling in vmbus_wait_message(). The ISR is provided
 * for future interrupt-driven operation. */
void vmbus_isr(void)
{
    struct hv_message *msg = &sim_page->messages[VMBUS_MESSAGE_SINT];

    if (msg->header.message_type == HV_MESSAGE_TYPE_NONE)
        return;

    /* For now, just mark that a message is available.
     * Synthetic drivers will process it via vmbus_wait_message().
     * Future: wake blocked channel waiters here. */

    /* Note: AutoEOI is set on SINT2, so we don't need to send LAPIC EOI */
}

/* ---- Public API ---- */

int vmbus_init(void)
{
    if (!hv_detect()) {
        klog(LOG_INFO, "hyperv",
             "Not running on Hyper-V — skipping VMBus");
        return -1;
    }

    klog(LOG_INFO, "hyperv", "Initializing VMBus core protocol...");

    /* Step 1: Set up hypercall page */
    if (hv_setup_hypercall() < 0)
        return -1;

    /* Step 2: Set up SynIC (messages + interrupts) */
    if (hv_setup_synic() < 0)
        return -1;

    /* Step 3: Connect to VMBus (version negotiation) */
    if (vmbus_connect() < 0)
        return -1;

    /* Step 4: Enumerate offered channels */
    if (vmbus_enumerate() < 0)
        return -1;

    printk("[OK] VMBus: %u channels (version 0x%x)\n",
           offer_count, vmbus_version);

    return 0;
}

int vmbus_get_channel_count(void)
{
    return offer_count;
}

const struct vmbus_channel_offer_channel *vmbus_get_offer(int idx)
{
    if (idx < 0 || idx >= offer_count)
        return NULL;
    return &offers[idx];
}
