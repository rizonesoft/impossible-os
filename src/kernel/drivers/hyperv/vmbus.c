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
#include "kernel/irq.h"
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

/* CPUID leaf 0x40000003 EAX: partition privileges (TLFS §2.4.4) */
#define HV_FEATURE_PRIV_ACCESS_VP_RUNTIME   (1u << 0)
#define HV_FEATURE_PRIV_ACCESS_HYPERCALL    (1u << 1)  /* Can use hypercall page */
#define HV_FEATURE_PRIV_ACCESS_SYNIC        (1u << 2)  /* Can access SynIC MSRs */
#define HV_FEATURE_PRIV_ACCESS_SYNTH_TIMER  (1u << 3)
#define HV_FEATURE_PRIV_POST_MESSAGES       (1u << 4)  /* Can post SynIC messages */
#define HV_FEATURE_PRIV_SIGNAL_EVENTS       (1u << 5)  /* Can signal SynIC events */

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
    uint32_t privs;
    uint32_t required_privs;

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

    /* Check CPUID leaf 0x40000003: partition privileges (TLFS §2.4.4).
     * VMBus needs: AccessHypercallMsrs, AccessSynicRegs, PostMessages.
     * Running in QEMU on WSL2, the outer Hyper-V CPUID signature leaks
     * through but VMBus functionality is unavailable. This check filters
     * out environments that advertise Hv#1 but lack the required privileges
     * for VMBus operation (hypercall page, SynIC MSRs, message posting). */
    cpuid(HV_CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    privs = eax;

    required_privs = HV_FEATURE_PRIV_ACCESS_HYPERCALL |
                     HV_FEATURE_PRIV_ACCESS_SYNIC |
                     HV_FEATURE_PRIV_POST_MESSAGES;

    if ((privs & required_privs) != required_privs) {
        klog(LOG_INFO, "hyperv",
             "Hyper-V signature found but missing privileges "
             "(have=0x%x, need=0x%x) — not a full Hyper-V host",
             (uint64_t)privs, (uint64_t)required_privs);
        hv_detected = 0;
        return 0;
    }

    hv_detected = 1;
    klog(LOG_INFO, "hyperv",
         "Hyper-V detected (interface Hv#1, privs=0x%x)",
         (uint64_t)privs);
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
                channels[offer_count].callback = (vmbus_channel_callback_t)0;
                channels[offer_count].callback_ctx = (void *)0;

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

/* Public wrapper — allows drivers (e.g. storvsc) to create GPADLs for
 * their own data buffers that need to be visible to the host. */
int vmbus_create_gpadl_external(struct vmbus_channel *ch, void *buffer,
                                uint32_t page_count, uint32_t *out_handle)
{
    return vmbus_create_gpadl(ch, buffer, page_count, out_handle);
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

/* Transaction ID counter (monotonic, never 0) */
static uint64_t next_trans_id = 1;

/* ---- Raw ring buffer write (no framing — caller provides full bytes) ---- */

int vmbus_ring_write(struct vmbus_channel *ch,
                     const void *data, uint32_t len)
{
    uint32_t write_idx;
    uint32_t read_idx;
    uint32_t avail;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t first_chunk, second_chunk;

    if (!ch || !ch->is_open || !ch->send_ring)
        return -1;

    write_idx = ch->send_ring->write_index;
    read_idx  = ch->send_ring->read_index;

    /* Available space (leave 1 byte to distinguish full from empty) */
    if (write_idx >= read_idx)
        avail = ch->data_size - (write_idx - read_idx) - 1;
    else
        avail = (read_idx - write_idx) - 1;

    if (avail < len)
        return -1;

    /* Write with wrap-around */
    uint32_t wr_off = write_idx % ch->data_size;
    first_chunk = ch->data_size - wr_off;
    if (first_chunk > len)
        first_chunk = len;

    vmbus_memcpy(&ch->send_data[wr_off], src, first_chunk);
    second_chunk = len - first_chunk;
    if (second_chunk > 0)
        vmbus_memcpy(&ch->send_data[0], src + first_chunk, second_chunk);

    write_idx += len;

    /* Memory barrier before updating write index */
    __asm__ volatile("sfence" ::: "memory");
    ch->send_ring->write_index = write_idx % ch->data_size;

    return 0;
}

/* ---- Raw ring buffer read ---- */

uint32_t vmbus_ring_read(struct vmbus_channel *ch,
                         void *buf, uint32_t max_len)
{
    uint32_t read_idx, write_idx;
    uint32_t avail;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t first_chunk, second_chunk;
    uint32_t to_read;

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

    to_read = avail;
    if (to_read > max_len)
        to_read = max_len;

    /* Read with wrap-around */
    uint32_t rd_off = read_idx % ch->data_size;
    first_chunk = ch->data_size - rd_off;
    if (first_chunk > to_read)
        first_chunk = to_read;

    vmbus_memcpy(dst, &ch->recv_data[rd_off], first_chunk);
    second_chunk = to_read - first_chunk;
    if (second_chunk > 0)
        vmbus_memcpy(dst + first_chunk, &ch->recv_data[0], second_chunk);

    read_idx += to_read;

    /* Update read index */
    __asm__ volatile("sfence" ::: "memory");
    ch->recv_ring->read_index = read_idx % ch->data_size;

    return to_read;
}

/* ---- High-level packet send with vmpacket_descriptor framing ---- */

/* Align value up to 8-byte boundary */
static inline uint32_t align8(uint32_t v)
{
    return (v + 7) & ~7u;
}

int vmbus_sendpacket(struct vmbus_channel *ch,
                     const void *data, uint32_t len,
                     uint64_t trans_id, uint16_t type, uint16_t flags)
{
    struct vmpacket_descriptor desc;
    uint32_t desc_size = (uint32_t)sizeof(desc);
    uint32_t total_len = desc_size + len;
    uint32_t aligned_len = align8(total_len);
    /* The ring buffer requires a 64-bit "previous packet start offset"
     * written AFTER the packet data (used by host to walk packets). */
    uint32_t wire_len = aligned_len + 8;
    uint8_t pad[8];

    vmbus_memset(&desc, 0, sizeof(desc));
    desc.type     = type;
    desc.offset8  = (uint16_t)(desc_size >> 3);  /* descriptor size in qwords */
    desc.len8     = (uint16_t)(aligned_len >> 3); /* total aligned packet in qwords */
    desc.flags    = flags;
    desc.trans_id = trans_id ? trans_id : next_trans_id++;

    /* Build a contiguous wire buffer:
     * [vmpacket_descriptor][payload][padding][prev_pkt_offset_64] */
    uint8_t wire_buf[512];  /* large enough for any control message */
    if (wire_len > sizeof(wire_buf))
        return -1;

    vmbus_memset(wire_buf, 0, wire_len);
    vmbus_memcpy(wire_buf, &desc, desc_size);
    vmbus_memcpy(wire_buf + desc_size, data, len);

    /* Trailing 64-bit previous packet start offset (always 0 for simplicity;
     * the host uses it for ring walk but 0 is acceptable) */
    vmbus_memset(pad, 0, 8);
    vmbus_memcpy(wire_buf + aligned_len, pad, 8);

    if (vmbus_ring_write(ch, wire_buf, wire_len) < 0)
        return -1;

    return 0;
}

/* ---- High-level packet receive (strips vmpacket_descriptor) ---- */

uint32_t vmbus_recvpacket(struct vmbus_channel *ch,
                          void *buf, uint32_t max_len,
                          uint64_t *out_trans_id)
{
    uint8_t pkt_buf[512];
    uint32_t bytes_read;
    struct vmpacket_descriptor *desc;
    uint32_t payload_offset;
    uint32_t payload_len;
    uint32_t total_pkt_len;

    bytes_read = vmbus_ring_read(ch, pkt_buf, sizeof(pkt_buf));
    if (bytes_read < sizeof(struct vmpacket_descriptor))
        return 0;

    desc = (struct vmpacket_descriptor *)pkt_buf;
    payload_offset = (uint32_t)desc->offset8 << 3;
    total_pkt_len  = (uint32_t)desc->len8 << 3;

    if (payload_offset > bytes_read || total_pkt_len > bytes_read)
        return 0;

    payload_len = total_pkt_len - payload_offset;
    if (payload_len > max_len)
        payload_len = max_len;

    vmbus_memcpy(buf, pkt_buf + payload_offset, payload_len);

    if (out_trans_id)
        *out_trans_id = desc->trans_id;

    return payload_len;
}

/* ---- Transfer page packet send (for StorVSC SCSI data I/O) ---- */

int vmbus_sendpacket_pagebuffer(struct vmbus_channel *ch,
                                const void *header_data, uint32_t header_len,
                                uint64_t trans_id,
                                uint16_t transfer_pageset_id,
                                const struct vmbus_transfer_page_range *ranges,
                                uint32_t range_count)
{
    /* Wire format:
     * [vmpacket_descriptor]
     * [vmbus_transfer_page_header]
     * [range_count × vmbus_transfer_page_range]
     * [header_data (e.g. VSTOR_PACKET)]
     * [padding to 8-byte alignment]
     * [prev_pkt_offset_64] */

    struct vmpacket_descriptor desc;
    struct vmbus_transfer_page_header xfer_hdr;
    uint32_t desc_size    = (uint32_t)sizeof(desc);
    uint32_t xfer_size    = (uint32_t)sizeof(xfer_hdr);
    uint32_t ranges_size  = range_count * (uint32_t)sizeof(struct vmbus_transfer_page_range);
    uint32_t data_offset  = desc_size + xfer_size + ranges_size;
    uint32_t total_len    = data_offset + header_len;
    uint32_t aligned_len  = align8(total_len);
    uint32_t wire_len     = aligned_len + 8;  /* +8 for trailing offset */

    uint8_t wire_buf[512];
    if (wire_len > sizeof(wire_buf))
        return -1;

    vmbus_memset(wire_buf, 0, wire_len);

    /* Packet descriptor */
    vmbus_memset(&desc, 0, sizeof(desc));
    desc.type     = VMBUS_PACKET_TYPE_DATA_XFER_PAGES;
    desc.offset8  = (uint16_t)(data_offset >> 3);
    desc.len8     = (uint16_t)(aligned_len >> 3);
    desc.flags    = VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED;
    desc.trans_id = trans_id ? trans_id : next_trans_id++;
    vmbus_memcpy(wire_buf, &desc, desc_size);

    /* Transfer page header */
    vmbus_memset(&xfer_hdr, 0, sizeof(xfer_hdr));
    xfer_hdr.transfer_pageset_id = transfer_pageset_id;
    xfer_hdr.sender_owns_set     = 1;
    xfer_hdr.range_count         = range_count;
    vmbus_memcpy(wire_buf + desc_size, &xfer_hdr, xfer_size);

    /* Transfer page ranges */
    if (range_count > 0 && ranges)
        vmbus_memcpy(wire_buf + desc_size + xfer_size, ranges, ranges_size);

    /* Control message (e.g. VSTOR_PACKET) */
    vmbus_memcpy(wire_buf + data_offset, header_data, header_len);

    /* Trailing 64-bit previous packet offset */
    /* (already zeroed by memset) */

    if (vmbus_ring_write(ch, wire_buf, wire_len) < 0)
        return -1;

    return 0;
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

/* ---- §11. VMBus ISR (called from IDT vector 0xF0 via irq_register) ---- */

/* Bridge irq_handler_t signature to vmbus_isr() */
static void vmbus_irq_handler(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;

    /* ---- Handle SIM messages (SINT2 message slot) ---- */
    if (sim_page) {
        struct hv_message *msg = &sim_page->messages[VMBUS_MESSAGE_SINT];
        if (msg->header.message_type != HV_MESSAGE_TYPE_NONE) {
            /* Message pending — pollers (vmbus_wait_message) will pick it up.
             * Future: wake blocked tasks here. */
        }
    }

    /* ---- Handle SIEF event flags (per-channel callbacks) ---- */
    if (sief_page) {
        struct hv_synic_event_flags *flags = &sief_page->sint[VMBUS_MESSAGE_SINT];
        int word;

        for (word = 0; word < 64; word++) {
            uint32_t bits = flags->flags[word];
            if (bits == 0)
                continue;

            /* Atomically clear all set bits we're about to process */
            __asm__ volatile("lock xchgl %0, %1"
                : "=r"(bits), "+m"(flags->flags[word])
                : "0"(0)
                : "memory");

            while (bits) {
                /* Find lowest set bit */
                int bit;
                __asm__ volatile("bsfl %1, %0" : "=r"(bit) : "r"(bits));

                uint32_t relid = (uint32_t)(word * 32 + bit);

                /* Dispatch to channel callback if registered */
                if (relid < VMBUS_MAX_CHANNELS &&
                    channels[relid].callback) {
                    channels[relid].callback(&channels[relid],
                                             channels[relid].callback_ctx);
                }

                /* Clear the bit */
                bits &= ~(1u << bit);
            }
        }
    }

    /* Note: AutoEOI is set on SINT2, so LAPIC EOI is handled by irq_dispatch_wrapper */
}

void vmbus_set_channel_callback(struct vmbus_channel *ch,
                                vmbus_channel_callback_t cb, void *ctx)
{
    if (!ch)
        return;
    ch->callback = cb;
    ch->callback_ctx = ctx;
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

    /* Step 2.5: Register VMBus ISR at SINT2 vector (0xF0) via dynamic IRQ API.
     * This replaces the catch-all IDT stub with a proper handler that
     * scans SIEF event flags and dispatches to per-channel callbacks. */
    if (irq_register(VMBUS_INTERRUPT_VECTOR, vmbus_irq_handler,
                     NULL, "vmbus") != IRQ_OK) {
        klog(LOG_WARN, "hyperv",
             "Failed to register ISR at vec 0x%x — using catch-all stub",
             (uint64_t)VMBUS_INTERRUPT_VECTOR);
        /* Non-fatal: polling still works, ISR is an optimization */
    }

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
