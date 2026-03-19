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
static int offer_count;
static volatile int offers_complete;

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

/* ---- §7. VMBus ISR (called from IDT vector 0xF0) ---- */

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
