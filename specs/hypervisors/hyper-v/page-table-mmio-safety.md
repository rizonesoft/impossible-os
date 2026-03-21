# Hyper-V Page Table MMIO Safety

## Architecture and Implementation Specification for Confidential VM MMIO in Custom Operating Systems

| Field          | Value                                                        |
| -------------- | ------------------------------------------------------------ |
| **Status**     | Draft                                                        |
| **Version**    | 1.0                                                          |
| **Date**       | 2026-03-20                                                   |
| **Target**     | Impossible OS — x86-64 Long Mode                             |
| **References** | Hyper-V TLFS v6.0b, Intel TDX Module 1.5, AMD SEV-SNP ABI   |

---

## Table of Contents

1. [Introduction to Confidential Virtualization and MMIO Interception](#1-introduction)
2. [The Architectural Paradigm Shift: Implicit Traps vs Explicit Hypercalls](#2-paradigm-shift)
3. [Establishing the Hyper-V Hypercall Interface](#3-hypercall-interface)
4. [Memory Visibility Transitions and the Speculative Execution Threat](#4-memory-visibility)
5. [Detailed Specification: Explicit MMIO Hypercalls](#5-mmio-hypercalls)
6. [Securing the VMBus and Virtual PCI (vPCI)](#6-vmbus-vpci)
7. [The MMIO Guard Architecture: Enforcing Stage-2 Boundaries](#7-mmio-guard)
8. [Virtualization-Based Security (VBS) and Cross-VTL Communication](#8-vbs-vtl)
9. [Attack Surface Analysis and Real-World Emulation Vulnerabilities](#9-attack-surface)
10. [Conclusion](#10-conclusion)

---

## 1. Introduction to Confidential Virtualization and MMIO Interception

The advent of Confidential Computing (CoCo) and Virtualization-Based Security (VBS) has
fundamentally altered the threat model and architectural paradigm of modern hypervisors.
Historically, hypervisors such as Microsoft Hyper-V utilized a **trap-and-emulate** mechanism
to handle Memory-Mapped I/O (MMIO). In a traditional Virtual Machine (VM), when a guest OS
attempts to read from or write to a physical address designated for a hardware device, the
hardware's **Extended Page Tables (EPT)** or **Second Level Address Translation (SLAT)** tables
trigger an EPT violation. This virtualization exit transfers control to the hypervisor,
allowing it to inspect the faulting instruction, decode the memory operands, and emulate the
expected device behavior before returning execution to the guest OS.

This mechanism has long served as the bedrock of hardware virtualization, providing a seamless
abstraction layer for virtual hardware devices, including those operating over the **Virtual
PCI (vPCI)** bus and various emulated hardware components such as the two 8237 Direct Memory
Access (DMA) controllers commonly found on a PC motherboard.

### 1.1 The Confidential Computing Disruption

The introduction of isolated virtual machines, powered by advanced silicon features such as
**Intel Trust Domain Extensions (TDX)** and **AMD Secure Encrypted Virtualization-Secure
Nested Paging (SEV-SNP)**, breaks this legacy abstraction entirely.

In a Confidential Computing environment:

- The hypervisor is **deliberately and explicitly excluded** from the guest OS's trust boundary
- Guest memory is **cryptographically protected** via hardware-enforced encryption keys
- CPU registers are **automatically scrubbed** during all hypervisor transitions to prevent
  state leakage
- The hypervisor **cannot transparently read** the guest's instruction stream to determine
  which MMIO operation caused the fault

> [!IMPORTANT]
> If the hypervisor cannot decode the specific instruction that caused the memory fault, it
> cannot emulate the device access, rendering traditional trap-and-emulate virtualization
> **impossible**.

### 1.2 The MMIO Safety Solution

To bridge this architectural gap without compromising the confidentiality and integrity of
the isolated VM, custom operating systems running under Microsoft Hyper-V must implement
strict **MMIO Safety** protocols. This involves:

1. **Explicitly notifying** the hypervisor of the intended MMIO operation via standardized
   hypercalls (`HVCALL_MMIO_READ` and `HVCALL_MMIO_WRITE`)
2. **Meticulously managing** the visibility of page tables during transitions
3. **Implementing protections** akin to the Android Protected Kernel-based Virtual Machine
   (pKVM) "MMIO Guard" to ensure that peripheral devices and the untrusted hypervisor cannot
   exploit shared memory regions to compromise the guest kernel

---

## 2. The Architectural Paradigm Shift: Implicit Traps vs Explicit Hypercalls

The transition from implicit EPT violations to explicit hypercalls represents a fundamental
**inversion of trust** in operating system design.

### 2.1 Traditional MMIO Flow

In a standard Hyper-V deployment:

```
Guest MMIO Access → EPT Violation → Hypervisor Intercept
    → VMWP Device Emulation → Result Injected → Guest Resumes
```

The **Virtual Machine Worker Process (VMWP)** handles device emulation in the root partition.
It assigns PCI devices using virtual PCI (vPCI) **Virtualization Service Providers (VSPs)**
and **Virtualization Service Consumers (VSCs)** operating over the high-speed VMBus.

### 2.2 Confidential VM MMIO Flow

In a CoCo environment, the guest OS kernel must assume responsibility for explicit
communication:

```
Guest MMIO Access → #VC/#VE Exception → Guest Handler Decodes Instruction
    → Guest Issues HVCALL_MMIO_READ/WRITE → Hypervisor Emulates
    → Result Returned via Hypercall Output → Guest Resumes
```

### 2.3 Hardware-Level Interception Mechanisms

Advanced silicon vendors have taken divergent approaches to flagging invalid memory access
within an isolated environment:

#### AMD SEV-SNP: The #VC Exception

| Property           | Detail                                                       |
| ------------------ | ------------------------------------------------------------ |
| **Exception Type** | `#VC` (VMM Communication Exception, vector 29)               |
| **Trigger**        | Instructions accessing unmapped or protected MMIO            |
| **Guest Decode**   | Guest has crypto keys — can read its own instruction stream  |
| **Communication**  | Guest Hypervisor Communication Block (GHCB)                  |

The `#VC` handler must implement logic to:
1. Read from the source operand
2. Write the result to the destination operand
3. Split the operation to accurately identify the faulting access

> [!NOTE]
> When the CPU passes the exit reason to the `#VC` exception handler, it does **not**
> explicitly indicate whether the source operand, destination operand, or both caused the
> exception. The handler must implement split-operation logic.

#### Intel TDX: The #VE Exception

| Property           | Detail                                                       |
| ------------------ | ------------------------------------------------------------ |
| **Exception Type** | `#VE` (Virtualization Exception, vector 20)                  |
| **Trigger**        | Accessing unmapped MMIO region via EPT violation              |
| **MMIO Mechanism** | `TDG.VP.VMCALL` with MMIO sub-function via `TDCALL`          |
| **Shared Bit**     | Guest must set the **Shared bit** (highest GPA bit) to 1     |

Under Intel TDX, MMIO is treated strictly as shared memory and **cannot** be accessed via
direct memory read/write instructions. The guest must set the Shared bit (the highest-order
bit of the GPA, bit 47 for GPAW=48 or bit 51 for GPAW=52) to mark the address as shared,
permitting hypervisor interaction.

> [!WARNING]
> Hyper-V does **not** support guest VMs utilizing older AMD Secure Memory Encryption (SME)
> or first-generation SEV encryption without SNP. These standards are insufficient for fully
> confidential VM isolation.

#### ARM64 pKVM: Exception Level Isolation

| Property           | Detail                                                       |
| ------------------ | ------------------------------------------------------------ |
| **Isolation Level** | Exception Level 2 (EL2) isolated memory protections         |
| **Guard**          | MMIO Guard mediates access between EL1 guest and host        |
| **Specification**  | Firmware Framework for Arm (FF-A) for page sharing           |

### 2.4 The Hyper-V Abstraction Layer

To prevent maintaining separate hardware-specific state machines for AMD `#VC`, Intel `#VE`,
and ARM64 Exception Level routing, Microsoft Hyper-V abstracts these via its **unified
hypercall ABI**:

```
┌─────────────────────────────────┐
│   Custom OS MMIO Accessor       │
│   (readl, writel, inb, outb)    │
├─────────────────────────────────┤
│   Hyper-V Unified Hypercall ABI │
│   HVCALL_MMIO_READ  (0x0106)    │
│   HVCALL_MMIO_WRITE (0x0107)    │
├─────────────────────────────────┤
│   Hardware Abstraction          │
│   AMD #VC │ Intel #VE │ ARM EL2 │
└─────────────────────────────────┘
```

By leveraging Hyper-V's paravisor mode or fully-enlightened operational modes, the custom OS
can use the standardized `HVCALL_MMIO_READ` and `HVCALL_MMIO_WRITE` hypercalls, ensuring the
exact nature of the access is securely communicated without exposing the surrounding guest
instruction stream, local variables, or private CPU registers.

---

## 3. Establishing the Hyper-V Hypercall Interface

Before MMIO safety protocols can be implemented, the custom OS must initialize the
fundamental Hyper-V hypercall interface.

### 3.1 Hypervisor Detection and Capabilities

The initialization sequence requires dynamic identification of the hypervisor and systematic
capability enumeration:

#### CPUID Detection Sequence

| Step | CPUID Leaf   | Register    | Check                                    |
| ---- | ------------ | ----------- | ---------------------------------------- |
| 1    | `0x00000001` | ECX[31]     | Hypervisor present bit set               |
| 2    | `0x40000000` | EAX         | Maximum supported CPUID leaf             |
|      |              | EBX:ECX:EDX | Vendor signature (`"Microsoft Hv"`)      |
| 3    | `0x40000003` | Various     | Extended capabilities and feature flags  |

```c
/* Hypervisor detection pseudocode */
static bool detect_hyperv(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Step 1: Check hypervisor present bit */
    cpuid(0x01, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 31)))
        return false;

    /* Step 2: Verify vendor signature "Microsoft Hv" */
    cpuid(0x40000000, &eax, &ebx, &ecx, &edx);
    if (ebx != 0x7263694D ||   /* "Micr" */
        ecx != 0x666F736F ||   /* "osof" */
        edx != 0x76482074)     /* "t Hv" */
        return false;

    /* Step 3: Query extended capabilities */
    cpuid(0x40000003, &eax, &ebx, &ecx, &edx);
    /* Check for extended hypercall interface support */

    return true;
}
```

### 3.2 Hypercall Page Provisioning

Once compatibility is confirmed, the OS must provision the **hypercall page**:

> [!CAUTION]
> If the hypercall page inadvertently overlaps with an active MMIO range, a RAMdisk, or
> pre-allocated kernel memory, it invites **catastrophic memory corruption**.

#### Provisioning Steps

1. **Identify** a continuous, unoccupied page within the Guest Physical Address (GPA) space
2. **Write** the GPA to the Hypercall MSR (`HV_X64_MSR_HYPERCALL` at address `0x40000001`)
3. **Set** the "Enable Hypercall Page" bit via bitwise OR
4. **Create** a VA mapping in internal page tables pointing to the hypercall GPA
5. **Mark** the page as executable

```c
/* Hypercall page provisioning pseudocode */
#define HV_X64_MSR_HYPERCALL    0x40000001
#define HV_X64_MSR_GUEST_OS_ID  0x40000000
#define HYPERCALL_PAGE_ENABLE    (1ULL << 0)

static void provision_hypercall_page(uintptr_t gpa) {
    /* Write Guest OS ID first (required before any hypercalls) */
    wrmsr(HV_X64_MSR_GUEST_OS_ID, IMPOSSIBLE_OS_GUEST_ID);

    /* Enable the hypercall page at the specified GPA */
    uint64_t msr_val = (gpa & ~0xFFFULL) | HYPERCALL_PAGE_ENABLE;
    wrmsr(HV_X64_MSR_HYPERCALL, msr_val);

    /* Map the GPA into kernel virtual address space as executable */
    vmap_page(gpa, VMAP_EXEC | VMAP_KERNEL);
}
```

> [!NOTE]
> After MSR configuration, attempting to **write** directly to the hypercall page will
> correctly result in a **General Protection (#GP) exception**, protecting the interface
> integrity.

### 3.3 The Hypercall Calling Convention

Hyper-V enforces a strict ABI for executing hypercalls:

#### Register Conventions by Architecture

| Architecture   | Call Code | Input Param 1      | Input Param 2      | Output       | Volatile Registers      |
| -------------- | --------- | ------------------ | ------------------ | ------------ | ----------------------- |
| x64 (Standard) | RCX      | RDX (GPA of input) | R8 (GPA of output) | RCX (result) | Output struct modified  |
| x64 (Fast)     | RCX      | RDX                | R8                 | RAX          | RDI, RSI for inputs 3/4 |
| ARM64 (SMCCC)  | X0       | X1                 | X2                 | X0 (result)  | X1-X17 unmodified       |

#### Hypercall Types

| Type       | Description                                         | Optimization    |
| ---------- | --------------------------------------------------- | --------------- |
| **Simple** | Single task with fixed-size inputs                  | General purpose |
| **Rep**    | Repeated task over a list of elements               | High throughput |
| **Fast**   | Parameters passed via registers (no memory buffers) | Low latency     |

The input value loaded into RCX is the hypercall ID **encoded alongside control flags**:

```c
/* Hypercall input value encoding */
typedef union {
    uint64_t as_u64;
    struct {
        uint64_t call_code   : 16;  /* Bits 15:0  — Hypercall ID */
        uint64_t fast        : 1;   /* Bit  16    — Use fast (register) calling */
        uint64_t var_hdr_sz  : 9;   /* Bits 26:17 — Variable header size / 8 */
        uint64_t reserved    : 4;   /* Bits 30:27 — Must be zero */
        uint64_t is_nested   : 1;   /* Bit  31    — Process at L0 in nested virt */
        uint64_t rep_count   : 12;  /* Bits 43:32 — Rep call element count */
        uint64_t reserved2   : 4;   /* Bits 47:44 — Must be zero */
        uint64_t rep_start   : 12;  /* Bits 59:48 — Rep call start index */
        uint64_t reserved3   : 4;   /* Bits 63:60 — Must be zero */
    };
} hv_hypercall_input_t;
```

---

## 4. Memory Visibility Transitions and the Speculative Execution Threat

Before defining the MMIO operations, the custom OS must architect a safe mechanism for
transitioning memory visibility. In isolated VMs, certain memory pages must be mapped as
**shared** or **decrypted** so that the hypervisor can interact with them legitimately:

- VMBus ring buffers
- Synthetic Interrupt Controller (SynIC) pages
- Per-CPU hypercall input/output buffers

### 4.1 The #VC and #VE Panic Vector

When the OS transitions a memory page from encrypted (private) to decrypted (shared):

1. It issues a hypercall instructing the hypervisor to remap pages in host stage-2 tables
2. It updates its own stage-1 page tables (removing encryption bit or adjusting the S-bit)

> [!CAUTION]
> During the transition window, the physical memory page is in an **undefined, transient
> state**. Any memory access — including **speculative prefetch** — triggers a fatal
> `#VC`/`#VE` exception that the handler **cannot safely resolve**.

#### The Speculative Execution Danger

```
Timeline of Vulnerability:
─────────────────────────────────────────────────────────────────
  Page encrypted ──┐
                   │ Clear encryption bit in PTE
                   │ ← DANGER ZONE: page state is undefined
                   │ Issue hypercall to remap in host tables
                   │ ← CPU speculative prefetch can touch page here
                   │ Host acknowledges remap
                   │ Set new attributes in PTE
  Page decrypted ──┘
─────────────────────────────────────────────────────────────────
```

The primary danger arises from low-level kernel mechanisms like `load_unaligned_zeropad()`
which routinely make stray, out-of-bounds memory references as part of optimized string and
memory copy operations. If such a function speculatively touches a page mid-transition, the
resulting `#VC`/`#VE` exception is **unrecoverable**.

### 4.2 Implementing the PRESENT Bit Shield

The custom OS must adopt the **PRESENT bit clearing** methodology during all memory
visibility transitions:

```
┌───────────────────────────────────────────────────────────────┐
│  STEP 1: Clear PRESENT bit                                    │
│  ─────────────────────────────────────                        │
│  Strip PRESENT from PTE → page is Not Present                 │
│  Any speculative access now generates #PF (safe, fixable)     │
│                                                               │
│  STEP 2: Execute the transition                               │
│  ──────────────────────────────                               │
│  Issue hypercall to alter host visibility state               │
│  MMU generates #PF (not #VC/#VE) for any stray accesses       │
│                                                               │
│  STEP 3: Standard Page Fault fixup                            │
│  ────────────────────────────────                             │
│  #PF handler detects speculative access → clean fixup          │
│  No kernel crash                                              │
│                                                               │
│  STEP 4: Restore PRESENT bit                                  │
│  ───────────────────────────                                  │
│  Set PRESENT + new encrypted/decrypted attribute              │
│  Error path guarantees PRESENT is restored on failure          │
└───────────────────────────────────────────────────────────────┘
```

#### Implementation Pseudocode

```c
/* Safe memory visibility transition */
static int hv_safe_visibility_transition(uintptr_t gpa, size_t npages,
                                         bool make_shared) {
    int ret;

    /* Step 1: Clear PRESENT bit to shield against speculative access */
    set_memory_np(gpa, npages);

    /* Step 2: Issue hypercall while page is Not Present */
    ret = hv_set_host_visibility(gpa, npages, make_shared);
    if (ret) {
        /* Error path: MUST restore PRESENT to avoid broken memory */
        goto err_restore;
    }

    /* Step 4: Restore PRESENT with new visibility attribute */
    if (make_shared)
        set_memory_decrypted_present(gpa, npages);
    else
        set_memory_encrypted_present(gpa, npages);

    return 0;

err_restore:
    /* Guarantee PRESENT bits are restored even on failure */
    set_memory_p(gpa, npages);
    return ret;
}
```

> [!IMPORTANT]
> This architectural pattern is **entirely non-negotiable** for custom OS stability in CoCo
> environments. Failure to clear the PRESENT bit guarantees intermittent, highly
> non-deterministic kernel panics driven by unpredictable CPU prefetchers.

---

## 5. Detailed Specification: Explicit MMIO Hypercalls

With the hypercall interface established and page table visibility protocols defined, the
custom OS must implement explicit MMIO routing structures.

### 5.1 MMIO Read Operations (`HVCALL_MMIO_READ`)

**Hypercall ID:** `0x0106`

#### Input Structure: `hv_mmio_read_input`

| Field      | Type  | Description                                         |
| ---------- | ----- | --------------------------------------------------- |
| `gpa`      | `u64` | Guest Physical Address of the MMIO register to read |
| `size`     | `u32` | Size of the read operation (1, 2, 4, or 8 bytes)    |
| `reserved` | `u32` | Padding for 64-bit alignment                        |

#### Output Structure: `hv_mmio_read_output`

| Field  | Type     | Description                                                              |
| ------ | -------- | ------------------------------------------------------------------------ |
| `data` | `u8[32]` | Array holding the read value (up to `HV_HYPERCALL_MMIO_MAX_DATA_LENGTH`) |

#### Implementation

```c
#define HVCALL_MMIO_READ  0x0106
#define HV_HYPERCALL_MMIO_MAX_DATA_LENGTH  32

typedef struct {
    uint64_t gpa;
    uint32_t size;
    uint32_t reserved;
} hv_mmio_read_input_t;

typedef struct {
    uint8_t data[HV_HYPERCALL_MMIO_MAX_DATA_LENGTH];
} hv_mmio_read_output_t;

static uint64_t hv_mmio_read(uint64_t gpa, uint32_t size) {
    hv_mmio_read_input_t  *input;
    hv_mmio_read_output_t *output;
    uint64_t result = 0;

    /* Disable interrupts to reserve per-CPU hypercall page */
    irq_disable();

    input  = (hv_mmio_read_input_t *)get_percpu_hypercall_input();
    output = (hv_mmio_read_output_t *)get_percpu_hypercall_output();

    input->gpa      = gpa;
    input->size     = size;
    input->reserved = 0;

    uint64_t status = hv_do_hypercall(HVCALL_MMIO_READ, input, output);

    if (hv_status_success(status)) {
        /* Copy from shared page to local register IMMEDIATELY */
        switch (size) {
        case 1: result = output->data[0]; break;
        case 2: result = *(uint16_t *)output->data; break;
        case 4: result = *(uint32_t *)output->data; break;
        case 8: result = *(uint64_t *)output->data; break;
        }
    }

    irq_enable();
    return result;
}
```

### 5.2 MMIO Write Operations (`HVCALL_MMIO_WRITE`)

**Hypercall ID:** `0x0107`

#### Input Structure: `hv_mmio_write_input`

| Field      | Type     | Description                                     |
| ---------- | -------- | ----------------------------------------------- |
| `gpa`      | `u64`    | Guest Physical Address of the MMIO register     |
| `size`     | `u32`    | Size of the write payload (1, 2, 4, or 8 bytes) |
| `reserved` | `u32`    | Padding for memory alignment                    |
| `data`     | `u8[32]` | Raw data payload to be written to the device    |

#### Implementation

```c
#define HVCALL_MMIO_WRITE  0x0107

typedef struct {
    uint64_t gpa;
    uint32_t size;
    uint32_t reserved;
    uint8_t  data[HV_HYPERCALL_MMIO_MAX_DATA_LENGTH];
} hv_mmio_write_input_t;

static void hv_mmio_write(uint64_t gpa, uint32_t size, uint64_t value) {
    hv_mmio_write_input_t *input;

    irq_disable();

    input = (hv_mmio_write_input_t *)get_percpu_hypercall_input();

    input->gpa      = gpa;
    input->size     = size;
    input->reserved = 0;

    /* Correctly align value into the data array */
    switch (size) {
    case 1: input->data[0]              = (uint8_t)value;  break;
    case 2: *(uint16_t *)input->data    = (uint16_t)value; break;
    case 4: *(uint32_t *)input->data    = (uint32_t)value; break;
    case 8: *(uint64_t *)input->data    = (uint64_t)value; break;
    }

    uint64_t status = hv_do_hypercall(HVCALL_MMIO_WRITE, input, NULL);
    /* Propagate errors back to requesting kernel driver */
    if (!hv_status_success(status))
        printk("[MMIO] Write failed: gpa=0x%lx size=%u status=0x%lx\n",
               gpa, size, status);

    irq_enable();
}
```

---

## 6. Securing the VMBus and Virtual PCI (vPCI)

### 6.1 VMBus Isolation and Memory Marking

The VMBus relies on shared memory constructs — ring buffers and monitor pages — for event
signaling and bulk data transfer. In a CoCo VM, the hypervisor reading the ring buffer would
encounter **indecipherable encrypted ciphertext** unless explicitly decrypted.

#### Required Memory Transitions for VMBus

| VMBus Resource                 | Action Required                                  |
| ------------------------------ | ------------------------------------------------ |
| Monitor pages                  | Transition to host-visible via PRESENT shield    |
| Primary ring buffer mappings   | Mark as decrypted in guest page tables           |
| Secondary memory mappings      | Propagate "decrypted" attribute                  |
| SynIC message/event flag pages | Share with hypervisor (unless paravisor handles) |

> [!WARNING]
> Failure to manage encryption states breaks fundamental Hyper-V Integration Services.
> Components relying on VMBus (Heartbeat IC, Shutdown IC, VSS IC) will show **"No Contact"**
> status in Hyper-V Manager and the VM cannot be gracefully shut down.

#### Affected Data Structures

| Structure      | Description                         | VMBus Channel      |
| -------------- | ----------------------------------- | ------------------ |
| `hv_kvp_msg`   | Key-Value Pair exchange messages    | KVP Exchange IC    |
| `hv_vss_msg`   | Volume Shadow Copy Service messages | VSS IC             |
| Ring buffers   | Bidirectional data transfer         | All VMBus channels |

### 6.2 Virtual PCI Configuration Space

PCI configuration space operates differently than standard MMIO ranges. In a CoCo VM, the
hypervisor **fails to emulate** configuration traps due to memory encryption.

#### Solution: Specialized vPCI Frontend Driver

The custom OS must implement a vPCI frontend driver that:

1. **Intercepts** standard PCI configuration read/write requests
2. **Redirects** them through `HVCALL_MMIO_READ` / `HVCALL_MMIO_WRITE`
3. **Maintains** a dynamic `use_calls` flag in the device tree or ACPI parsing logic

```c
/* vPCI frontend configuration read */
static uint32_t hv_pcifront_read_config(uint64_t cfg_gpa,
                                        uint32_t offset,
                                        uint32_t size) {
    if (is_coco_environment()) {
        /* Route through explicit hypercall */
        return (uint32_t)hv_mmio_read(cfg_gpa + offset, size);
    } else {
        /* Legacy direct memory access */
        return mmio_read_direct(cfg_gpa + offset, size);
    }
}
```

When the `use_calls` flag is active (indicating a CoCo environment), **all** PCI
configuration routing is shunted to the hypercall subsystem, allowing standard device drivers
to function as if running on bare-metal hardware.

---

## 7. The MMIO Guard Architecture: Enforcing Stage-2 Boundaries

Exposing MMIO through explicit hypercalls solves the emulation issue but opens a secondary
attack vector: **malicious peripheral injection and unauthorized DMA**.

### 7.1 Principles of the MMIO Guard

| Principle                | Description                                         |
| ------------------------ | --------------------------------------------------- |
| **Strict Enrollment**    | Guest explicitly enrolls pages into MMIO guard      |
| **Default Private**      | All pages are private unless explicitly authorized  |
| **Stage-2 Enforcement**  | Hardware MMU/IOMMU blocks unauthorized transactions |
| **Per-Page Granularity** | Authorization is granted at individual page level   |

```
┌─────────────────────────────────────────────┐
│           Guest Physical Memory             │
│                                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐     │
│  │ PRIVATE │  │ PRIVATE │  │ ENROLLED│     │
│  │ (kernel)│  │ (data)  │  │ (MMIO)  │◄────── Explicitly enrolled
│  └─────────┘  └─────────┘  └─────────┘     │
│                                             │
│  Stage-2 Page Tables enforce:               │
│  • PRIVATE pages: NO host/device access     │
│  • ENROLLED pages: Mediated MMIO only       │
└─────────────────────────────────────────────┘
```

If a virtual device attempts to access a GPA that has **not** been flagged as an active MMIO
buffer within the hypervisor's stage-2 tracking structures, the hardware MMU or IOMMU
**blocks the transaction immediately**.

### 7.2 Bounce Buffering and Virtio Constraints

For paravirtualized devices using the virtio standard (virtio-net, vsock, etc.), the custom
OS must implement **bounce buffers**:

```
┌──────────────────┐     ┌─────────────────┐     ┌──────────────┐
│  Private Kernel   │────►│  Bounce Buffer  │────►│  Virtio      │
│  Data Structures  │     │  (Shared/Guard) │     │  Device      │
│  (Encrypted)      │◄────│  (Decrypted)    │◄────│  (Host VMM)  │
└──────────────────┘     └─────────────────┘     └──────────────┘
        COPY                   MMIO                    DMA
```

#### Bounce Buffer Protocol

1. **Allocate** a dedicated, isolated memory region explicitly shared with the host
2. **Copy** data from internal encrypted kernel structures into the bounce buffer
3. **Execute** the explicit MMIO hypercall to trigger the device
4. **Copy** resulting data back to private memory
5. **Zero** the bounce buffer after use

> [!IMPORTANT]
> The OS must **never** directly map internal, sensitive kernel data structures to the virtio
> device. This guarantees that an out-of-bounds read or write by a malicious VSP cannot leak
> sensitive kernel secrets or overwrite active execution stacks.

---

## 8. Virtualization-Based Security (VBS) and Cross-VTL Communication

MMIO safety and hypercall abstraction also apply to isolating components **within** the guest
itself using Virtualization-Based Security (VBS).

### 8.1 Virtual Trust Levels

| VTL  | Name          | Purpose                                 |
| ---- | ------------- | ---------------------------------------- |
| VTL0 | Normal OS     | Standard operating system execution      |
| VTL1 | Secure Kernel | Credential Guard, HVCI, secure enclave   |

### 8.2 Cross-VTL Hypercalls

| Hypercall                        | ID       | Description                            |
| -------------------------------- | -------- | -------------------------------------- |
| `HvVtlCall`                      | `0x0011` | Switch execution context VTL0 → VTL1   |
| `HvVtlReturn`                    | `0x0012` | Return execution context VTL1 → VTL0   |
| `HvCallModifyVtlProtectionMask`  | —        | Prevent lower VTLs from tampering MMIO |

> [!WARNING]
> A custom OS implementing VBS must **strictly validate all MMIO payload sizes and memory
> boundaries** crossing the VTL threshold. A compromised VTL0 application must not be able
> to exploit the secure kernel in VTL1 via malformed MMIO payloads.

---

## 9. Attack Surface Analysis and Real-World Emulation Vulnerabilities

### 9.1 Uninitialized Memory Leaks — CVE-2018-0888

| Property       | Detail                                                 |
| -------------- | ------------------------------------------------------ |
| **Component**  | `BatteryEmulator::MmioRead()` in VMWP                  |
| **Root Cause** | Output buffer allocated but **not fully initialized**  |
| **Impact**     | Guest could read residual heap data from root partition |
| **Data Leaked**| ASLR offsets, cryptographic key fragments               |

**Mitigation for custom OS:** Rigorously validate `hv_mmio_read_input.size` and
`hv_mmio_write_input.size` before dispatch. Zero all output buffers before use.

### 9.2 VpciBus Assignment Exploitation

The VMWP assigns PCI devices via `VpciBus` using ioctls like
`IOCTL_VPCI_ASSIGN_DEVICE` handled by `vpcivsp!VpciIoctlAssignDevice`. Without proper VMBus
channel security and MMIO Guards, an attacker could forge malicious ioctl messages to achieve
**complete virtual machine escape**.

### 9.3 Mobile Architecture Memory Safety — CVE-2024-32897

Demonstrates that memory safety vulnerabilities extend beyond server hypervisors into
mobile baseband firmware:

| Property       | Detail                                                         |
| -------------- | -------------------------------------------------------------- |
| **Component**  | `ProtocolCdmaCallWaitingIndAdapter::GetCwInfo()` in baseband   |
| **Root Cause** | Out-of-bounds read in modem protocol adapter                   |
| **Impact**     | Remote information disclosure (requires baseband compromise)   |
| **Relevance**  | Illustrates MMIO-adjacent attack surface in embedded firmware  |

> [!CAUTION]
> A custom OS must **hardcode** its MMIO Safety configurations. User-space configurations
> must **never** be allowed to override fundamental hypervisor memory protections.

### 9.4 Mitigating Double-Fetch Vulnerabilities

A critical vulnerability pattern in hypervisor-guest communication:

```
┌─────────┐         ┌────────────┐         ┌──────────┐
│ Guest   │ writes  │ Shared     │ reads   │Hypervisor│
│ Thread  │────────►│ Memory     │────────►│          │
│         │         │ Page       │         │ Check(v) │
│ Malicious│ modify │            │ read    │ Use(v')  │◄── v ≠ v' = EXPLOIT
│ Thread  │────────►│            │────────►│          │
└─────────┘         └────────────┘         └──────────┘
```

#### Mandatory Mitigation

The custom OS **must** dictate that `hv_mmio_read_output.data` is:

1. **Copied exactly once** from the shared page
2. Placed into **local, unshared CPU registers** or **strictly private kernel memory**
3. **Never** used directly from a shared, host-visible page as a loop counter, array index,
   or size determinant

```c
/* CORRECT: Copy-then-use pattern */
uint32_t safe_value;
memcpy(&safe_value, output->data, sizeof(safe_value));
/* Now use safe_value — it's in private stack memory */

/* WRONG: Direct use from shared page */
uint32_t *unsafe = (uint32_t *)output->data;
for (int i = 0; i < *unsafe; i++) { /* TOCTOU vulnerability! */ }
```

---

## 10. Conclusion

The integration of Hyper-V Page Table MMIO Safety into a custom operating system requires a
profound paradigm shift from traditional OS architecture. The hypervisor can no longer be
trusted as an omnipotent, transparent emulator.

### Summary of Requirements

| Requirement             | Implementation                                              |
| ----------------------- | ----------------------------------------------------------- |
| Explicit MMIO routing   | `HVCALL_MMIO_READ` (0x0106) / `HVCALL_MMIO_WRITE` (0x0107) |
| Page table transitions  | PRESENT bit clearing during visibility changes              |
| VMBus shared memory     | Explicit decryption marking for ring buffers                |
| vPCI configuration      | Frontend driver routing through hypercalls                  |
| MMIO Guard              | Per-page enrollment, stage-2 enforcement                    |
| Bounce buffers          | Isolated shared regions for virtio communication            |
| Double-fetch prevention | Single-copy to private memory before use                    |
| VBS/VTL isolation       | Strict payload validation across VTL boundaries             |

### Design Principles

1. **Never trust the hypervisor** — treat it as an untrusted peripheral controller
2. **Never access mid-transition pages** — always clear PRESENT first
3. **Never use shared-page values directly** — copy to private memory first
4. **Never expose kernel structures to virtio** — always use bounce buffers
5. **Never allow user-space MMIO config overrides** — hardcode safety settings

By rigorously implementing these protocols, the custom OS achieves cryptographic security,
operational stability, and compliance with the Confidential Computing paradigm under
Microsoft Hyper-V's isolated execution environments.

---

> [!NOTE]
> This specification is aligned with the Hyper-V Top-Level Functional Specification v6.0b
> and incorporates lessons from CVE-2018-0888, CVE-2024-32897, and the Linux kernel's
> CoCo/Hyper-V patches (hv_vtom_clear_present, hv_vtom_set_host_visibility).
