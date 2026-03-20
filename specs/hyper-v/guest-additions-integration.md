# Microsoft Hyper-V Guest Additions Integration

A Comprehensive Specification for Custom Operating System Development

---

## Table of Contents

1. [Executive Overview and Architectural Foundation](#1-executive-overview-and-architectural-foundation)
2. [Hypervisor Discovery and Interface Initialization](#2-hypervisor-discovery-and-interface-initialization)
3. [The Hypercall Interface Configuration](#3-the-hypercall-interface-configuration)
4. [Synthetic Interrupt Controller (SynIC)](#4-synthetic-interrupt-controller-synic)
5. [Virtual Machine Bus (VMBus) Architecture](#5-virtual-machine-bus-vmbus-architecture)
6. [Hyper-V Integration Services (Guest Additions)](#6-hyper-v-integration-services-guest-additions)
7. [Advanced VMBus Channel Operations and Memory Optimizations](#7-advanced-vmbus-channel-operations-and-memory-optimizations)
8. [Conclusion](#8-conclusion)

---

## 1. Executive Overview and Architectural Foundation

The integration of a custom operating system into the Microsoft Hyper-V virtualization
environment requires a meticulous implementation of the **Hypervisor Top-Level Functional
Specification (TLFS)**. Hyper-V operates on a partition-based architecture, functioning as
a native, **Type-1 hypervisor** that serves as the foundational layer managing hardware
access and strictly isolating execution environments.

The primary management entity is the **Root Partition**, frequently referred to as the
parent partition or host, which hosts the virtualization management stack. This stack
includes:

- **Virtual Machine Management Service (VMMS)**
- **Virtual Machine Worker Process (VMWP)**
- **Virtualization Service Providers (VSPs)** — directly control physical hardware

Custom operating systems deployed as guest virtual machines execute within **Child
Partitions**. These child partitions do not possess direct access to physical hardware
resources, nor do they natively handle physical processor interrupts. Instead, they are
presented with a virtualized view of resources, encapsulated as **virtual devices (VDevs)**.

While legacy hardware emulation (such as emulating an Intel e1000 network adapter or an
IDE controller) provides basic compatibility, it introduces severe performance penalties
due to the overhead of trapping and emulating hardware register accesses. To achieve
optimal performance, network, storage, and host-guest metadata exchange must bypass
traditional hardware emulation entirely.

This high-performance paradigm is achieved through **"Enlightened I/O"**, a specialized
virtualization-aware implementation of communication protocols. Enlightened operating
systems host **Virtualization Service Consumers (VSCs)** within their kernels, which
communicate directly with the root partition's VSPs. The conduit for this high-speed,
logical inter-partition communication is known as the **Virtual Machine Bus (VMBus)**.

The software suite that implements these enlightened VSCs, the VMBus protocol, and the
associated utility daemons is collectively known as **Integration Services**, or
**Guest Additions**.

### Initialization Sequence Overview

Implementing these integration services within a custom operating system demands a
rigorously phased initialization and configuration sequence:

1. **Hypervisor Discovery** — Discover the hypervisor via intercepted CPUID instructions
2. **MSR Configuration** — Configure Model-Specific Registers to establish a shared
   execution page for hypercalls
3. **SynIC Initialization** — Initialize the Synthetic Interrupt Controller for
   asynchronous event delivery
4. **VMBus Establishment** — Establish VMBus communication channels via protocol
   negotiation with the root partition
5. **IC Service Deployment** — Implement utility payloads for Heartbeat, Time Sync,
   Graceful Shutdown, KVP Data Exchange, and VSS

---

## 2. Hypervisor Discovery and Interface Initialization

Before a custom operating system can utilize any Hyper-V enlightenments, it must
explicitly query the underlying processor to confirm the presence, vendor identity,
and functional capabilities of the hypervisor. This discovery mechanism relies entirely
on the execution of the `CPUID` instruction, which the hypervisor intercepts to inject
its own configuration data.

### 2.1 CPUID Leaves and Feature Identification

The Microsoft hypervisor interface specifies that compliant hypervisors must intercept
CPUID execution and return deterministic values in the synthetic leaves `0x40000000`
through `0x40000005`. The presence of a virtualization environment is initially indicated
to the guest if the highest bit of the standard CPUID feature flags is set; specifically,
**`CPUID(1).ECX` bit 31 must equal 1**.

Once basic hypervisor presence is confirmed, the guest operating system must
systematically query the Hyper-V specific leaves:

| CPUID Leaf     | Macro Definition                          | Description |
|----------------|-------------------------------------------|-------------|
| `0x40000000`   | `HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS`   | Returns the maximum supported hypervisor CPUID leaf in EAX. Vendor signature across EBX, ECX, EDX must concatenate to ASCII `"Microsoft Hv"`. |
| `0x40000001`   | `HYPERV_CPUID_INTERFACE`                  | Returns the hypervisor interface signature. EAX must return `"Hv#1"` (`0x31237648`). |
| `0x40000002`   | `HYPERV_CPUID_VERSION`                    | Returns hypervisor build number, major version, minor version, and service pack across EAX–EDX. |
| `0x40000003`   | `HYPERV_CPUID_FEATURES`                   | Returns critical bitmask of architectural features available to the partition. |
| `0x40000004`   | `HYPERV_CPUID_ENLIGHTMENT_INFO`           | Returns recommendations for which enlightenments the guest should implement. |
| `0x40000005`   | `HYPERV_CPUID_IMPLEMENT_LIMITS`           | Returns maximum hardware limits (e.g., max virtual processors). |

#### Feature Flags (CPUID 0x40000003, EAX)

| Bit | Macro Definition                        | Description |
|-----|-----------------------------------------|-------------|
| 0   | `HV_X64_MSR_VP_RUNTIME_AVAILABLE`      | VP Runtime MSR available — query precise execution time. |
| 1   | `HV_X64_MSR_TIME_REF_COUNT_AVAILABLE`  | Partition Reference Counter available — monotonic time source. |
| 2   | `HV_X64_MSR_SYNIC_AVAILABLE`           | **SynIC MSRs available** (SCONTROL, SIMP, SIEFP, EOM, SINT0–SINT15). **Mandatory for VMBus.** |
| 3   | `HV_X64_MSR_SYNTIMER_AVAILABLE`        | Synthetic Timer MSRs available. |
| 4   | `HV_X64_MSR_APIC_ACCESS_AVAILABLE`     | APIC access MSRs (EOI, ICR, TPR) for optimized interrupt routing. |
| 9   | `HV_X64_MSR_REFERENCE_TSC_AVAILABLE`   | Reference TSC page present. |
| 11  | `HV_X64_MSR_APIC_FREQUENCY_AVAILABLE`  | MSRs to retrieve LAPIC timer frequency and TSC frequency. |

> [!IMPORTANT]
> A conforming guest targeting VMBus integration **must** verify that **Bit 2**
> (`HV_X64_MSR_SYNIC_AVAILABLE`) is set. The SynIC is the mandatory signaling mechanism
> for all VMBus communications. Without it, the guest cannot receive device offers or
> data interrupts from the root partition.

### 2.2 Synthetic Model-Specific Registers (MSRs) and Privilege Validation

Once processor capabilities are verified, the OS must configure the hypervisor
environment. Hyper-V relies heavily on **synthetic MSRs** to control hypervisor-specific
features and state variables. These MSRs fall entirely outside the standard x86/x64
architectural MSR range.

#### 2.2.1 Guest OS Identity Registration

The guest must identify itself by writing a 64-bit identifier to `HV_X64_MSR_GUEST_OS_ID`
at address **`0x40000000`**. This identifier encodes:

- **Upper 32 bits**: OS vendor ID
- **Lower 32 bits**: OS version and build number

> [!WARNING]
> Writing a value of **zero** to this MSR is interpreted as a teardown/disconnect request,
> immediately disabling the hypercall execution interface.

#### 2.2.2 Hypercall Page Registration

Configure the physical address of the hypercall execution page by writing to
`HV_X64_MSR_HYPERCALL` (**`0x40000001`**). See [Section 3](#3-the-hypercall-interface-configuration).

#### 2.2.3 Virtual Processor Index Discovery

Read the read-only MSR `HV_X64_MSR_VP_INDEX` (**`0x40000002`**) to retrieve the
virtual processor (VP) index of the currently executing logical CPU. This index is
essential for targeting synthetic interrupts to specific CPUs during VMBus channel
assignment and multi-queue device scaling.

---

## 3. The Hypercall Interface Configuration

The hypercall interface provides a secure, low-latency mechanism for the guest OS to
request privileged operations from the hypervisor. Unlike traditional system calls
(INT 0x80, SYSCALL, SYSENTER), hypercalls transition from guest kernel mode to the
hypervisor ring.

### 3.1 Mapping the Hypercall Execution Page

Hypercalls are **not** invoked via a static interrupt vector. Instead, the hypervisor
dynamically writes an executable instruction sequence — the **hypercall trampoline** —
into a memory page provided by the guest.

**Setup procedure:**

1. Allocate a single contiguous physical page (4 KB) and zero its contents
2. Construct a 64-bit mapping value combining:
   - Guest Physical Address (GPA) of the allocated page
   - `HV_HYPERCALL_ENABLE` flag (bit 0)
3. Execute `wrmsr` to write this value to `HV_X64_MSR_HYPERCALL` (`0x40000001`)
4. The hypervisor validates the GPA, maps it, and injects processor-specific trampoline
   code (`VMCALL` for Intel VT-x or `VMMCALL` for AMD-V)
5. Invoke hypercalls via a standard `CALL` instruction targeting the page address

### 3.2 x64 Hypercall Calling Convention (ABI)

The Hyper-V hypercall ABI for x64 uses register-based parameter passing:

| Register | Purpose |
|----------|---------|
| **RCX**  | Hypercall code (operation identifier) + control flags (including "Fast" bit) |
| **RDX**  | GPA of input parameter block (8-byte aligned, should not cross page boundary) |
| **R8**   | GPA of output parameter block |
| **RAX**  | Return value — 64-bit status code (set by hypervisor on return) |

#### 3.2.1 Fast Hypercalls (XMM Register Optimization)

Setting the **"Fast" flag** (bit 16) in RCX activates a zero-memory-copy calling
convention:

- Input parameters are packed directly into **XMM0–XMM5** (up to 112 bytes)
- The hypervisor does **not** read from the GPA in RDX
- Output parameters are returned in remaining XMM registers
- Eliminates nested page table traversal overhead (EPT on Intel, RVI on AMD)

### 3.3 Hypercall Status Codes

| Hex Value  | Macro Definition                      | Description |
|------------|---------------------------------------|-------------|
| `0x0000`   | `HV_STATUS_SUCCESS`                   | Hypercall executed successfully. |
| `0x0002`   | `HV_STATUS_INVALID_HYPERCALL_CODE`    | Unrecognized opcode in RCX. |
| `0x0003`   | `HV_STATUS_INVALID_HYPERCALL_INPUT`   | Invalid input parameters or rep count. |
| `0x0004`   | `HV_STATUS_INVALID_ALIGNMENT`         | Input/output blocks not 8-byte aligned. |
| `0x0005`   | `HV_STATUS_ACCESS_DENIED`             | Partition lacks required privileges. |
| `0x0011`   | `HV_STATUS_INVALID_PORT_ID`           | Port ID is invalid or deleted. |
| `0x0012`   | `HV_STATUS_INVALID_CONNECTION_ID`     | Connection ID is invalid. |
| `0x0013`   | `HV_STATUS_INSUFFICIENT_BUFFERS`      | Target port has no available message buffers. |

> [!NOTE]
> The two most frequently used hypercalls for VMBus integration are
> **`HvCallPostMessage`** and **`HvCallSignalEvent`**. These are the foundation of
> inter-partition communication and ring buffer flow control.

---

## 4. Synthetic Interrupt Controller (SynIC)

In physical x86/x64 hardware, interrupts are managed by the APIC subsystem (IOAPIC +
local APICs). In heavily virtualized environments, emulating hardware interrupt
controllers introduces massive latency because every APIC register access triggers a
VM exit.

The **Synthetic Interrupt Controller (SynIC)** is an architectural extension of the
virtualized local APIC. It provides a paravirtualized, memory-mapped signaling mechanism
that facilitates all VMBus notifications, intercepts, and synthetic timer events. Every
virtual processor maintains its own distinct, private SynIC instance.

### 4.1 SynIC MSR Architecture

| MSR Macro Name           | Address        | Description |
|--------------------------|----------------|-------------|
| `HV_X64_MSR_SCONTROL`   | `0x40000080`   | Global SynIC Control Register — enable/disable SynIC per VP. |
| `HV_X64_MSR_SVERSION`   | `0x40000081`   | Read-only — SynIC architectural version. |
| `HV_X64_MSR_SIEFP`      | `0x40000082`   | Physical address of Event Flags Page (bit-based signaling). |
| `HV_X64_MSR_SIMP`       | `0x40000083`   | Physical address of Message Page (structured payload delivery). |
| `HV_X64_MSR_EOM`        | `0x40000084`   | End of Message — acknowledge message processing. |
| `HV_X64_MSR_SINT0`–`HV_X64_MSR_SINT15` | `0x40000090`–`0x4000009F` | 16 Synthetic Interrupt Source Registers. |

### 4.2 SINT Register Bitfield Layout

Each 64-bit SINT register defines routing behavior for a specific interrupt source:

| Bit Range | Field Name | Description |
|-----------|------------|-------------|
| 0:7       | `Vector`   | Local APIC vector (`0x10`–`0xFF`) injected when a message/event arrives. |
| 8:15      | `RsvdP`    | Reserved — must be zero. |
| 16        | `Masked`   | If 1, SINT is masked. Messages queue but interrupt is not delivered. |
| 17        | `AutoEOI`  | If 1, hypervisor performs implicit EOI. **Eliminates costly APIC EOI writes.** |
| 18        | `Polling`  | If 1, hardware interrupts suppressed; guest must poll SIMP/SIEFP. |
| 19:63     | `RsvdP`    | Reserved — must be zero. |

> [!WARNING]
> **AutoEOI and Hardware Conflicts**: Setting AutoEOI is highly recommended for VMBus
> channels (improves I/O throughput), but it **breaks compatibility** with hardware-assisted
> APIC virtualization (Intel APICv / AMD AVIC). The hypervisor will typically disable
> APICv for that VM when SynIC AutoEOI is active.

### 4.3 SIMP and SIEFP Memory Page Mechanics

The SynIC delivers data directly into guest RAM using two dedicated 4 KB overlay pages
per virtual processor:

**Synthetic Interrupt Message Page (SIMP):**
- 4 KB page formatted as a **16-element array**
- Each element = **256 bytes** (16 × 256 = 4096)
- Each element corresponds to one SINT register
- Hypervisor writes 256-byte payload directly into guest RAM, then raises the APIC vector

**Synthetic Interrupt Event Flags Page (SIEFP):**
- Used for ultra-lightweight, high-frequency, **bit-based event signaling**
- Bypasses overhead of copying 256-byte structures
- Atomic bit toggles notify the guest that data is waiting in a shared ring buffer

#### SynIC Initialization Sequence (per VP)

1. Allocate two page-aligned 4 KB memory blocks and zero their contents
2. Obtain Guest Physical Addresses (GPAs) of both blocks
3. Write `SIMP GPA | HV_SIMP_ENABLE` to `HV_X64_MSR_SIMP`
4. Write `SIEFP GPA | HV_SIEFP_ENABLE` to `HV_X64_MSR_SIEFP`
5. Write `HV_SCONTROL_ENABLE` to `HV_X64_MSR_SCONTROL`

> [!CAUTION]
> After processing a message in a SIMP slot, the guest **must** write to
> `HV_X64_MSR_EOM` to signal the hypervisor that the slot is free. Failure to write
> to the EOM register will result in the hypervisor **indefinitely stalling** message
> delivery for that SINT.

---

## 5. Virtual Machine Bus (VMBus) Architecture

The **Virtual Machine Bus (VMBus)** is the software-defined data construct providing
high-bandwidth, channel-based communication between root partition VSPs and the guest OS.
It is the foundational transport layer for all synthetic devices:

- **storvsc** — Synthetic storage controller
- **netvsc** — Synthetic network interface
- **hv_utils** — All Integration Services

The VMBus is modeled as a system bus (similar to PCI), with individual synthetic devices
registering as child nodes.

### 5.1 Protocol Version Negotiation and Connection Handshake

Initialization begins by establishing a dedicated control path. The guest constructs and
posts a `CHANNELMSG_INITIATE_CONTACT` message to the hypervisor.

The VMBus specification has evolved across multiple Windows generations (Server 2008 →
Windows 8 → Windows 10 → Server 2025), requiring **version negotiation**. The guest
submits its desired protocol version in `vmbus_version_requested`. Depending on the
version (e.g., protocol 5.0), structural requirements for subsequent messages may change
(e.g., mandatory Connection ID fields).

Upon successful negotiation, the guest enters a listening state, waiting for device
**Offers**.

### 5.2 Device Offers and Channel Mapping

The hypervisor exposes virtual hardware by sending
`vmbus_channel_offer_channel` messages via the control channel. Each offer identifies
a specific virtual device instance.

**Key fields in the offer message:**

| Field | Description |
|-------|-------------|
| **Interface Type GUID (Class ID)** | Device category (e.g., Synthetic SCSI, Synthetic NIC, Heartbeat). |
| **Instance GUID** | Unique identifier for this specific device instance. |
| **`child_relid`** | Channel Relative ID — **primary identifier** for all subsequent operations on this channel. |
| **`monitorid`** | Decomposed into `monitor_grp` + `monitor_bit` for event signaling via the monitor page. |

### 5.3 Ring Buffers and GPADL Memory Management

Once the guest accepts an offer, it allocates physical memory for data transfer. VMBus
channels use two separate, unidirectional, circular queues:

- **Inbound ring buffer** — Host → Guest
- **Outbound ring buffer** — Guest → Host

#### 5.3.1 Guest Physical Address Descriptor List (GPADL)

Contiguous virtual memory may be backed by fragmented physical pages. The guest maps
this gap by sending a complete list of underlying physical pages to the host:

1. Send `CHANNELMSG_GPADL_HEADER` — specifies `gpadl_handle` and initial PFN array
2. If PFN list exceeds header capacity, append `CHANNELMSG_GPADL_BODY` messages
3. Host maps these physical pages into its address space → **zero-copy, double-mapped
   memory region**
4. Host acknowledges with `CHANNELMSG_GPADL_CREATED`
5. Guest sends `CHANNELMSG_OPENCHANNEL` with `child_relid`, `openid`, and
   `ringbuffer_gpadlhandle`
6. On close, GPADL is torn down via `CHANNELMSG_GPADL_TEARDOWN`

#### 5.3.2 Ring Buffer Header Mechanics and Lockless Synchronization

Each ring buffer is prefixed by a control header (`struct hv_ring_buffer`):

| Offset | Type    | Field              | Description |
|--------|---------|--------------------|-------------|
| `0x00` | `u32`   | `write_index`      | Byte offset where producer writes next packet. |
| `0x04` | `u32`   | `read_index`       | Byte offset where consumer reads next packet. |
| `0x08` | `u32`   | `interrupt_mask`    | If 1, consumer suppresses producer interrupts (polling mode). |
| `0x0C` | `u32`   | `pending_send_sz`  | Advanced flow control — receiver interrupts sender only when enough space frees up. |
| `0x10` | `u32`   | `reserved1`        | Padding for future expansion. |
| `0x40` | `union` | `feature_bits`     | Flags for advanced features (e.g., `feat_pending_send_sz`). |
| `0x44` | `u8[]`  | `reserved2`        | Padding to ensure payload begins on a **4 KB page boundary**. |

> [!IMPORTANT]
> **Memory ordering is critical.** Modern processors reorder instructions aggressively.
> The guest **must** use hardware memory barriers (`smp_read_barrier_depends()` or
> `virt_load_acquire()`) **before** fetching `write_index` from mapped GPADL memory
> to prevent reading stale payload data.

**Empty check:** `read_index == write_index` → ring buffer is empty.

---

## 6. Hyper-V Integration Services (Guest Additions)

With VMBus operational, the hypervisor transmits offers for various **Integration
Services (ICs)**. These provide the core Guest Additions functionality. In Linux
reference implementations, they are handled by `hv_utils` (in-kernel) and user-space
daemons (`hv_kvp_daemon`, `hv_vss_daemon`).

### 6.1 Integration Services Protocol Negotiation

All ICs use a standardized message header (`struct icmsg_hdr`). Before operational
payloads can be exchanged, host VSP and guest VSC must negotiate the protocol version.

**Negotiation payload** (`struct icmsg_negotiate`):

| Type               | Field               | Description |
|--------------------|---------------------|-------------|
| `u16`              | `icframe_vercnt`    | Count of framework versions offered by host. |
| `u16`              | `icmsg_vercnt`      | Count of message protocol versions offered. |
| `u32`              | `reserved`          | Padding. |
| `struct ic_version` | `icversion_data[]` | Dynamically sized array of proposed version numbers. |

The guest analyzes proposed versions, mutates the packet in the ring buffer (setting
status to accept/reject + injecting its supported version), and transmits back to host.

### 6.2 Standard IC Message Header

Every data packet on an IC channel is prepended with a mandatory header:

| Type               | Field                | Description |
|--------------------|----------------------|-------------|
| `struct ic_version` | `icverframe`        | Negotiated framework version. |
| `u16`              | `icmsgtype`          | Message payload type identifier. |
| `struct ic_version` | `icvermsg`          | Negotiated message structure version. |
| `u16`              | `icmsgsize`          | Size in bytes of the functional payload. |
| `u32`              | `status`             | `HV_S_OK` (`0x00000000`) on success, or error code. |
| `u8`               | `ictransaction_id`   | Sequence ID for request/response correlation. |
| `u8`               | `icflags`            | Direction/response bitflags. |
| `u8`               | `reserved`           | Alignment padding. |

### 6.3 Heartbeat Service (`HV_HEARTBEAT_GUID`)

- **Windows Service**: `vmicheartbeat`
- **Linux Equivalent**: In-kernel via `hv_utils`

**Purpose:** Allows the root partition to monitor guest OS health. Proves the guest
kernel is alive, scheduling threads, and responsive to interrupts. Without heartbeat
responses, Hyper-V reports "No Contact" or "Lost Communication."

**Payload:**

| Type  | Field       | Description |
|-------|-------------|-------------|
| `u64` | `seq_num`   | Monotonically incrementing sequence number. |
| `u32` | `reserved`  | Padding. |

**Execution:** Host sends a heartbeat with incrementing `seq_num`. Guest must:
1. Read the payload
2. Increment `seq_num` by one
3. Set `icmsg_hdr.status` to `HV_S_OK`
4. Write modified packet to outbound ring buffer

### 6.4 Time Synchronization Service (`HV_TIME_SYNC_GUID`)

- **Windows Service**: `vmictimesync`
- **Linux Equivalent**: In-kernel via `hv_utils`

**Purpose:** Corrects severe clock drift in VMs caused by virtual processor preemption.
Critical after VM boot or resume from saved/paused state.

**Payload:**

| Type  | Field           | Description |
|-------|-----------------|-------------|
| `u64` | `parenttime`    | Authoritative time from root partition. |
| `u64` | `childtime`     | Time currently perceived by guest. |
| `u64` | `roundtriptime` | Message transmission latency. |
| `u8`  | `flags`         | Command directives from host. |

**Time Conversion:** `parenttime` is in **100-nanosecond intervals** from the
**Windows NT epoch (January 1, 1601)**. For POSIX time (Unix epoch, January 1, 1970):

```
WLTIMEDELTA = 116444736000000000  (100ns units between 1601 and 1970)
unix_time = (parenttime - WLTIMEDELTA) / 10000000
```

**Flags:**

| Flag                        | Value | Action |
|-----------------------------|-------|--------|
| `ICTIMESYNCFLAG_PROBE`     | `0`   | Latency test only — do **not** alter system clock. |
| `ICTIMESYNCFLAG_SYNC`      | `1`   | **Force hard immediate clock update** regardless of delta. |
| `ICTIMESYNCFLAG_SAMPLE`    | `2`   | Periodic sample — gently slew PLL clock to avoid abrupt jumps. |

> [!TIP]
> In Active Directory environments, provide a mechanism for admins to **disable** this
> service on virtualized Domain Controllers. Inheriting time from an unsynchronized
> host can trigger cascading time-drift failures across the enterprise.

### 6.5 Guest Shutdown Service (`HV_SHUTDOWN_GUID`)

- **Windows Service**: `vmicshutdown`
- **Linux Equivalent**: In-kernel via `hv_utils`

**Purpose:** Enables graceful shutdown requests from the management console. Without
this service, a stop command forces a hard power-off, risking filesystem corruption
and data loss.

**Payload:**

| Type  | Field             | Description |
|-------|-------------------|-------------|
| `u32` | `reason_code`     | Numeric code indicating shutdown reason. |
| `u32` | `timeout_seconds` | Duration before forced hard power-off. |
| `u32` | `flags`           | Modifiers (restart vs. power off). |
| `u8[]`| `display_message` | Human-readable text (up to 2048 bytes, UTF-8/UTF-16). |

**Execution:** Upon receipt, the guest driver must:
1. Trigger the OS's native soft-shutdown sequence
2. Safely unmount file systems
3. Transition to ACPI S5 power state
4. Log `display_message` to system logs

### 6.6 Key-Value Pair (KVP) Data Exchange (`HV_KVP_GUID`)

- **Windows Service**: `vmickvpexchange`
- **Linux Equivalent**: Requires user-space daemon `hv_kvp_daemon`

**Purpose:** Out-of-band metadata transport between host and guest. Operates entirely
over VMBus — **no network interface or TCP/IP stack required**. Used in cloud
environments to inject provisioning configs (IP addresses, hostnames) on first boot,
or report guest status to host management console.

**Payload:**

| Type            | Field   | Description |
|-----------------|---------|-------------|
| `unsigned char` | `key`   | String identifier (max `HV_KVP_EXCHANGE_MAX_KEY_SIZE`). |
| `unsigned char` | `value` | String content (max `HV_KVP_EXCHANGE_MAX_VALUE_SIZE`). |

**KVP Pool Files:**

| Pool File       | Windows Registry Namespace       | Direction |
|-----------------|----------------------------------|-----------|
| `.kvp_pool_0`   | `Virtual Machine\External`       | Host → Guest |
| `.kvp_pool_1`   | `Virtual Machine\Guest`          | Guest → Host |
| `.kvp_pool_2`   | `Virtual Machine\Auto`           | Automatic |
| `.kvp_pool_3`   | `Virtual Machine\Guest\Parameter` | Guest parameters |
| `.kvp_pool_4`   | Reserved                         | Reserved |

### 6.7 Volume Shadow Copy Requestor (`HV_VSS_GUID`)

- **Windows Service**: `vmicvss`
- **Linux Equivalent**: Requires user-space daemon `hv_vss_daemon`

**Purpose:** Enables application-consistent, live VM backups. Without this, hypervisor
snapshots produce crash-consistent states, risking database corruption.

**Payload:**

| Type | Field       | Description |
|------|-------------|-------------|
| `u8` | `operation` | VSS command (Freeze, Thaw, etc.). |
| `u8` | `reserved`  | Alignment padding. |

**Operations:**

| Operation                | Value | Action |
|--------------------------|-------|--------|
| `VSS_OP_REGISTER`       | `128` | Initial daemon handshake. |
| `VSS_OP_REGISTER1`      | `129` | Extended capability handshake. |
| `VSS_OP_FREEZE`         | `5`   | **Freeze all filesystems** — flush caches, block writes (`FIFREEZE` ioctl). |
| `VSS_OP_THAW`           | `6`   | **Thaw filesystems** — unblock I/O after snapshot secured (`FITHAW` ioctl). |
| `VSS_OP_AUTO_RECOVER`   | `7`   | Fallback — respond with non-support flag if unsupported. |

> [!WARNING]
> If the guest fails to freeze and return success within the strict timeout, the
> hypervisor **unilaterally fails** the backup. The maximum VSS freeze message can
> reach **6,260 bytes** — ring buffer handlers must accommodate large multi-page reads.

---

## 7. Advanced VMBus Channel Operations and Memory Optimizations

### 7.1 Multi-Page Buffer Packets and Scatter-Gather I/O

For synthetic SCSI or NIC drivers transferring megabytes of data, copying into ring
buffers is architecturally impossible. Two mechanisms resolve this:

**Pagebuffer:** Associates a byte offset and length with an array of independent GPAs.
Foundation of **scatter-gather I/O** — aggregates physically discontinuous pages into
a single logical frame, passing only the pointer map to the host.

**MPB (Multi-Page Buffer) Descriptor:** Associates a single unified offset and length
with a GPA list. The GPAs **must** describe a single contiguous logical area.

APIs: `vmbus_sendpacket_pagebuffer` and `vmbus_sendpacket_mpb_desc`.

### 7.2 Asynchronous Event Signaling and Polling Suppression

When the guest places data in the outbound ring buffer:

1. Locate the channel's `monitor_bit` and `monitor_grp` (from the initial offer)
2. Execute an **atomic test-and-set** on the corresponding bit in `HV_X64_MSR_SIEFP`
3. **Only if** the bit transitions from 0 → 1, issue `HvCallSignalEvent` hypercall

**Polling suppression:** If the host sets `interrupt_mask = 1` in the ring buffer
header (indicating active polling), the guest **must suppress** the `HvCallSignalEvent`
hypercall entirely — achieving **zero signaling overhead**.

### 7.3 Virtual Address Space Management Enlightenments

Enlightened guests can improve memory management by utilizing Hyper-V **TLB flush
enlightenments**. Normally, modifying a page table entry affecting multiple processors
requires IPIs to flush TLBs — in virtualized environments this causes **VM exit storms**.

**Recommended hypercalls:**

- `HvFlushVirtualAddressSpace`
- `HvFlushVirtualAddressSpaceEx`

These offload TLB invalidation to the hypervisor, which performs global invalidation
without software IPIs transitioning through the guest kernel.

---

## 8. Conclusion

Developing a custom OS as a fully enlightened Hyper-V guest requires uncompromising
adherence to the TLFS. While legacy emulation provides functional fallback, bypassing
it is **strictly mandatory** for production-level performance.

### Three Architectural Pillars

1. **Synthetic Interrupt Controller (SynIC)** — Overlay pages and synthetic MSRs bypass
   APIC emulation latency for instantaneous, asynchronous notifications.

2. **Virtual Machine Bus (VMBus)** — Double-mapped GPADLs and lockless ring buffers with
   strict memory barriers achieve true zero-copy data transmission between partitions.

3. **Integration Services** — Kernel-level drivers (TimeSync, Heartbeat) and user-space
   daemons (KVP, VSS) map host management functions directly into the guest environment.

By methodically implementing CPUID discovery, aligning hypercall memory mappings to
strict calling conventions, and implementing byte-level packet headers as specified
throughout this document, an OS development team can achieve native-level,
high-fidelity interoperability with the Microsoft Hyper-V virtualization stack.
