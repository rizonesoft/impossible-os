# UEFI Specification Release 2.10: Architectural and Technical Analysis

The Unified Extensible Firmware Interface (UEFI) Specification Release 2.10,
formally published in August 2022, represents a foundational architectural
realignment for the global computing ecosystem. Maintained by the UEFI
Forum—a consortium of industry-leading hardware and software vendors—the
UEFI standard fundamentally defines the critical software interface between
a computing platform's operating system (OS) and its underlying hardware
firmware. Originating as the Intel Extensible Firmware Interface (EFI) 1.10
specification in 2002 before its stewardship was transferred to the UEFI
Forum in 2005, the architecture has entirely superseded the legacy Basic
Input/Output System (BIOS) that previously dominated IBM PC-compatible
personal computers. While UEFI remains platform and programming-language
independent by design, the reference implementation, TianoCore EDKII, is
predominantly constructed in the C programming language and widely adapted
by independent BIOS vendors such as AMI Aptio, Phoenix SecureCore, and
InsydeH2O.

Historically, the UEFI specification was heavily optimized for traditional
computing platforms utilizing general-purpose operating systems, such as
enterprise servers, high-performance desktops, and consumer laptops.
However, the proliferation of the specification across diverse market
segments revealed significant scalability challenges. Version 2.10
confronts these challenges directly, instituting massive structural changes
to support the contradictory demands of modern technology. On one end of
the spectrum, the specification introduces sophisticated, highly constrained
"reduced models" to support minimal-footprint embedded edge devices,
Internet of Things (IoT) sensors, and automotive microcontrollers.
Conversely, for enterprise environments, UEFI 2.10 introduces rigorous,
heavyweight protocols designed to secure high-performance computing against
advanced persistent threats, hypervisor compromises, and the looming
horizon of quantum cryptographic decryption.

The specification documentation itself carries stringent legal disclaimers,
emphasizing that all interface designs are provided on an "AS IS" basis
without warranties of merchantability or fitness for a particular purpose.
A critical directive for hardware designers embedded within these legal
frameworks is the explicit warning against relying on the absence or
specific characteristics of any features or instructions currently marked
as "reserved" or "undefined" within the tables. The Unified EFI Forum
reserves these explicitly for future definition, and platform instability
resulting from the unauthorized utilization of reserved bits remains the
sole liability of the firmware developer.

This comprehensive analysis systematically deconstructs the architectural
mandates, protocol introductions, algorithm exchanges, and deprecations
formalized within UEFI 2.10. By examining the integration of UEFI
Conformance Profiles, the sophisticated Cryptographic Agility framework,
the Confidential Computing (CC) extensions, advanced W^X memory
protections, and the deep expansion into emerging Instruction Set
Architectures (ISAs) such as RISC-V and LoongArch, this document serves
as an exhaustive technical specification analysis for systems architects,
firmware developers, and operating system kernel engineers.

## 1. Architectural Streamlining and the "Reduced Model"

For over a decade, the UEFI specification mandated a monolithic set of
required interfaces. To achieve compliance, a platform was theoretically
required to implement a vast array of boot services, runtime services,
cryptographic protocols, human interface infrastructure (HII) components,
and network stacks, regardless of whether the physical hardware actually
required them. As UEFI adoption expanded into non-traditional, highly
constrained environments, firmware developers in the embedded, IoT, and
automotive sectors found the burden of implementing these unused
interfaces—and managing the associated massive codebase footprints—to
be economically and technically unviable.

### 1.1 The EFI_CONFORMANCE_PROFILE_TABLE Framework

UEFI 2.10 resolves this systemic structural bloat through the formal
introduction of UEFI Conformance Profiles (Mantis# 2271), fundamentally
redefining how a platform proves compliance. This mechanism allows
platform manufacturers to selectively implement highly specialized subsets
of UEFI-required interfaces, permitting the creation of a "Reduced Model"
firmware implementation that still maintains strict, standardized
compliance for its specific device class.

The technical realization of this modular feature is the
`EFI_CONFORMANCE_PROFILE_TABLE`, which is formally integrated into the
broader EFI Configuration Table & Properties Table (Section 4.6.5). This
table allows a platform's firmware to explicitly advertise its
specification conformance to the loaded software environment, such as a
boot manager, an OS loader, or a low-level diagnostic tool, utilizing an
array of pre-defined Profile Identifiers represented as standard GUIDs.

The operational logic governing this table is strictly defined to ensure
absolute backward compatibility with legacy operating systems that predate
the 2.10 specification:

- **Implicit Full Conformance:** If the
  `EFI_CONFORMANCE_PROFILE_TABLE` is entirely absent from the system's
  configuration tables, the operating system is architecturally required
  to assume that the platform implementation is fully conformant with the
  complete, unreduced UEFI specification requirements as defined in
  Section 2.6.
- **Explicit Full Conformance:** Alternatively, a platform may explicitly
  declare full, traditional conformance by publishing the table and
  populating it with the
  `EFI_CONFORMANCE_PROFILES_UEFI_SPEC_GUID`.
- **Targeted Reduced Profiles:** Systems utilizing constrained, reduced
  models will publish specific, standardized GUIDs that dictate precisely
  which architectural protocols are guaranteed to be present, and
  crucially, which heavy protocols have been intentionally omitted to
  save Non-Volatile Random-Access Memory (NVRAM) and pre-boot execution
  space.

### 1.2 The Evolution of EBBR and Industry Standards

The integration of Conformance Profiles in UEFI 2.10 coincides with a
critical cleanup of the specification's approach to embedded systems.
Prior to 2.10, the industry attempted to manage embedded system
requirements through external specifications, most notably the Embedded
Base Boot Requirements (EBBR). The EBBR target market featured a
drastically reduced set of requirements, completely omitting complex UEFI
features deemed unnecessary for headless or highly specialized embedded
controllers.

In earlier iterations, a system complying with EBBR was required to
declare compliance by advertising specific EBBR profiles via the
`EFI_CONFORMANCE_PROFILE_TABLE`. However, with the maturation of the
profile system within the core UEFI document, Version 2.10 formally
removed the external EBBR Conformance profile from the core specification
(Mantis# 2320), actively consolidating the approach to embedded and IoT
edge systems under a unified, UEFI-native profiling structure.

Similarly, the UEFI standard dictates strict rules regarding parallel
industry configuration tables. A compliant system might provide an
Advanced Configuration and Power Interface (ACPI) table or a Devicetree
(DTSPEC) system description. However, to prevent OS-level collision and
confusion, the architecture strictly mandates that systems must never
provide both ACPI and Devicetree tables to the OS simultaneously; a
configuration mechanism must exist to mutually exclude one based on the
boot environment. Furthermore, the base Boot Requirement Specification
(BRS) mandates that a "dummy" Device Tree must only be exposed to the OS
if no actual hardware description is included, serving purely to pass
hand-off info, such as RAM disk locations, via a UEFI configuration table
entry of type `EFI_DTB_TABLE_GUID`.

## 2. Cryptographic Agility and the 2026 Secure Boot Horizon

The most extensive, technically complex, and globally critical updates
formalized within UEFI 2.10 pertain to system security, specifically the
mandated transition toward cryptographic agility in preparation for a
post-quantum computing landscape. Hardcoded cryptographic algorithms
embedded deep within platform firmware represent a severe, systemic
vulnerability. If an underlying hashing algorithm or asymmetric key
protocol is mathematically compromised, millions of fielded devices
instantly become unpatchable at the hardware root-of-trust level.

### 2.1 The Crisis of Expiration: The 2026 Secure Boot Rollover

The theoretical security enhancements of UEFI 2.10 are being implemented
against a backdrop of urgent, absolute operational necessity. Secure Boot
is the foundational security standard developed by the PC industry to
ensure that a device boots utilizing solely software trusted by the
Original Equipment Manufacturer (OEM). When a modern computer initiates,
the UEFI firmware intercepts and verifies the cryptographic signature of
every subsequent piece of boot software, encompassing UEFI option ROMs,
EFI applications, and the OS bootloader.

However, the foundational Secure Boot certificates—originally issued by
Microsoft in 2011 and which underpin the root of trust for an
overwhelming majority of the world's x86 hardware—are slated to begin
expiring in June 2026 (the Microsoft Corporation KEK CA 2011 expires on
June 24, 2026; the Windows Production PCA 2011 expires in October 2026).
If devices do not receive updated UEFI firmware containing the new
Platform Keys (PK) and Key Exchange Keys (KEK) prior to this expiration,
they face catastrophic boot failures. Specifically, enterprise
environments face the threat of widespread BitLocker recovery incidents
upon encountering newly signed OS loaders that no longer match the expired
keys resident in the hardware's NVRAM.

While software tools and remediation scripts exist to attempt to push
these updated certificates via OS-level endpoint management solutions
(such as Microsoft Intune or WSUS), large-scale field deployments have
consistently demonstrated that many devices require comprehensive,
low-level BIOS/UEFI firmware updates to correctly process, validate, and
permanently store the new keys without breaking the trust chain. UEFI
2.10's profound emphasis on crypto agility is thus not merely an academic
exercise, but a mandatory, standardized architectural framework explicitly
designed to prevent systemic hardware obsolescence during such massive
cryptographic rollover events.

### 2.2 The Firmware/OS Algorithm Exchange Mechanism

To achieve this required flexibility, UEFI 2.10 establishes a
standardized, dynamic negotiation interface between the Operating System
and the platform firmware, allowing the system to seamlessly transition
between cryptographic standards without requiring complete, disruptive
firmware overhauls. This dynamic exchange is managed through the newly
defined `EFI_CRYPTO_INDICATION` structure and a tightly regulated state
machine involving three distinct UEFI variables:

- **CryptoIndicationsSupported (Firmware-Owned):** During the early
  initialization phase, the firmware populates this variable with a
  complete, exhaustive registry of every cryptographic algorithm it is
  mathematically capable of executing. Crucially, this variable is
  dynamically recreated on every single boot cycle and is flagged as
  strictly read-only for the Operating System. This prevents a
  compromised OS from permanently masking hardware capabilities.
- **CryptoIndications (OS-Owned):** When the OS identifies a critical
  need to shift cryptographic algorithms—for example, deprecating an
  older, vulnerable hashing standard in favor of a newer one due to
  emerging threat models or expiring keys—it constructs a formal request
  and submits it to the firmware via a standard `SetVariable()` runtime
  service call.
- **CryptoIndicationsActivated (Firmware-Owned):** Upon receiving the
  OS's CryptoIndications request, the firmware intercepts the call and
  systematically validates the requested algorithm bits against its
  internal CryptoIndicationsSupported list. If the requested algorithms
  represent a valid, executable subset, the firmware activates them for
  the current and future sessions, reflecting this updated operational
  state in the CryptoIndicationsActivated variable. Like the supported
  list, this activated list is recreated every boot to ensure transient
  OS-level compromises cannot permanently alter the hardware's deep
  cryptographic posture.

### 2.3 Supported Cryptographic Algorithms and Post-Quantum Readiness

The specification explicitly enumerates the baseline algorithms required
to support a modern, agile security posture. The updates formalized in
Mantis# 2247 and 2291 force a deliberate shift away from legacy,
collision-vulnerable algorithms.

The recognized and explicitly supported asymmetric algorithms include:

- **RSASSA-PKCS1-v1_5:** Support is mandated for 2048, 3072, and
  4096-bit keys. Notably, RSA 4k support was explicitly re-added to the
  specification in Mantis# 2339 to bolster long-term agility and provide
  a stopgap before full quantum-resistant algorithms are standardized.
- **RSASSA-PSS:** Support is required for 3072 and 4096-bit keys. PSS
  provides a mathematically more robust padding scheme than the older
  PKCS#1 v1.5 standard, reducing the surface area for specific classes
  of cryptographic attacks.
- **ECDSA (Elliptic Curve Digital Signature Algorithm):** The
  specification requires support for NIST standard curves P256 and P384,
  offering high security with significantly smaller key sizes than RSA
  equivalents.

For cryptographic hashing, UEFI 2.10 permanently elevates the baseline.
While SHA-256 remains heavily utilized for legacy compatibility, the
specification enables explicit, native support for the SHA-384 and
SHA-512 signing schemes, particularly mandated for the validation of
Authenticated Variables (Mantis# 2205).

However, a critical security directive embedded within the specification
warns platform designers against over-accommodating legacy systems in the
name of interoperability. While the `EFI_CRYPTO_INDICATION` protocol
theoretically allows maximum flexibility, firmware that continues to
support deeply deprecated algorithms like isolated SHA-256 or weak
RSA-2048 exposes the platform to sophisticated downgrade attacks. In such
an attack, a malicious actor operating at the OS level forces the
firmware to negotiate down to a weaker, breakable cipher suite, bypassing
the protections of the newer algorithms. Therefore, the specification
strongly implies that a secure configuration should aggressively prune
deprecated algorithms from the CryptoIndicationsSupported variable.

### 2.4 Advanced Secure Boot Infrastructure and Strict dbx Matching

The security framework for UEFI images relies on a highly robust,
cryptographic validation process involving deeply nested signature
databases. Firmware maintains an Authorized UEFI Signature Database
(known as `db`) and a Forbidden UEFI Signature Database (known as `dbx`).
For any image to be approved for execution, its signature or hash must
explicitly exist in the authorized database, and crucially, it must not
possess any matching characteristics within the forbidden database.

Recent industry events have highlighted the vital importance of the dbx.
The notorious "BootHole" vulnerability, a flaw discovered in the GRand
Unified Bootloader (GRUB), enabled maliciously malformed configuration
files to grant arbitrary code execution during the boot phase. This
vulnerability gave malicious actors a direct mechanism to bypass Secure
Boot entirely. Mitigating BootHole required a massive expansion of dbx
records across the industry to revoke multiple generations of affected
GRUB binaries. In older systems, the physical NVRAM simply did not have
enough memory to accommodate all the necessary revocations.

To prevent such bypasses, UEFI 2.10 introduces the "Strict Forbidden
List Matching" rules. The criteria for rejection based on the dbx have
been rendered exceptionally rigid. Verification of a boot binary must
immediately fail, and execution must be halted, if the dbx contains any
of the following parameters:

- A direct SHA-256 hash of the binary executable itself.
- A "To-Be-Signed" hash of any individual certificate present anywhere
  within the binary's signing chain. This covers hashes generated via
  SHA-256, SHA-384, or SHA-512, meaning a single compromised
  intermediary certificate invalidates all binaries downstream.
- An entry containing a certificate that shares the exact Issuer, Serial
  Number, and To-Be-Signed hash as any certificate located in the
  image's signing chain.

Furthermore, to ensure the temporal validity of digital signatures across
decades of operation, UEFI 2.10 stipulates that if the platform firmware
supports X509-based certificates, it should concurrently implement the
RFC 3161 timestamp specification. This timestamping capability ensures
that a signature generated while a specific certificate was valid remains
trusted by the system even after that certificate naturally reaches its
expiration date, provided it has not been actively revoked in the dbx.

| Key / Database       | Function in Trust Chain              | Management                          |
| -------------------- | ------------------------------------ | ----------------------------------- |
| **Platform Key (PK)**| Root of trust: platform owner ↔      | New PK: Setup → User Mode.          |
|                      | firmware relationship.               | Clear PK: reverts to Setup Mode.    |
| **KEK**              | OS vendor ↔ firmware trust.          | Required for db/dbx updates.        |
| **db (Authorized)**  | Hashes/keys of authorized binaries.  | Must match here to execute.         |
| **dbx (Forbidden)**  | Revocation list of revoked binaries. | Match overrides db — exec denied.   |

UEFI 2.10 also introduces support for the Device Authentication Signature
Database, further expanding the variables and protocols required to
validate the origin and integrity of attached peripherals and their
associated Option ROMs (Mantis# 2217). Finally, the specification defines
complex system states such as Audit Mode and Deployed Mode, dictating
precisely how the PK and KEK are utilized during different lifecycle
stages of the hardware, from factory provisioning to end-user deployment.

## 3. The Confidential Computing (CC) Extension

As enterprise computational workloads rapidly migrate from isolated,
on-premises datacenters to massive, multi-tenant cloud environments, the
traditional perimeter of trust has collapsed inward. It is no longer
sufficient to merely secure the operating system against external network
threats; the highly sensitive workload must now be cryptographically
secured against the host hypervisor, neighboring virtual machines, and
the underlying physical hardware infrastructure itself.

To address this modern paradigm, UEFI 2.10 formally introduces the
Confidential Computing (CC) extension (Section 38, Mantis# 2317).

### 3.1 Hardware TEEs and Virtual Firmware Abstraction

The CC extension provides a standardized, deeply integrated software
abstraction layer for hardware-based Trusted Execution Environments
(TEEs), specifically architectured to support emerging silicon
technologies such as the Intel Trust Domain Extension (TDX) and AMD
Secure Encrypted Virtualization (SEV). Within a confidential computing
architecture, a virtual machine operates not as a standard guest, but as
a highly isolated, cryptographically segregated "Trust Domain."

The UEFI 2.10 CC extension defines the precise interface between the
specialized virtual firmware injected into this domain and the virtual
guest Operating System. This interface facilitates highly secure
cryptographic measurement and event logging based on the host hardware's
native TEE capabilities.

Crucially, the specification mandates that these CC interfaces operate on
a principle of optionality. They are strictly only to be published by the
firmware if the virtual firmware is actively utilizing a true,
hardware-backed TEE. If the environment lacks hardware CC capabilities
and relies instead on a software-emulated virtual Trusted Platform Module
(vTPM), the firmware must default to the legacy Trusted Computing Group
(TCG) defined event log protocol (`EFI_TCG2_PROTOCOL`) and must actively
suppress the new confidential computing interfaces to prevent OS-level
confusion regarding the actual security posture of the execution
environment.

### 3.2 The EFI_CC_MEASUREMENT_PROTOCOL

The operational core of this abstraction is the
`EFI_CC_MEASUREMENT_PROTOCOL` (uniquely identified by the GUID
`{0x96751a3d, 0x72f4, 0x41a6,
{0xa7, 0x94, 0xed, 0x5d, 0x0e, 0x67, 0xae, 0x6b}}`).
This protocol exposes a carefully curated suite of functions that allow
the guest OS to securely interact with the underlying hardware
measurement mechanisms without requiring proprietary, hypervisor-specific
or vendor-specific drivers:

- **GetCapability:** This foundational service exposes the operational
  parameters of the CC environment to the querying OS. It details the
  active CC type, the supported event log formats, and the supported
  hashing algorithms (such as the highly recommended SHA-384 standard).
- **GetEventLog:** This service allows the caller to retrieve the exact
  physical memory address of the CC event log, enabling the OS to
  independently verify the complete boot chain sequence and attest its
  integrity. It returns the memory location, the pointer to the last
  entry, and a boolean indicating if the log was truncated due to space
  constraints.
- **HashLogExtendEvent:** This is a critical active function utilized
  extensively by OS loaders to measure Portable Executable/Common Object
  File Format (PE/COFF) image binaries before execution. It allows the
  caller to hash an image, extend that measurement into the secure
  hardware register, and log the event securely—all without needing to
  understand the underlying CC vendor's specific, proprietary command
  syntax.
- **MapPcrToMrIndex:** Because standard OS software and legacy
  attestation agents are deeply designed to interact with TPM Platform
  Configuration Registers (PCRs), this function provides a vital
  translation layer. It provides a mapping mechanism to translate legacy
  TPM PCR indexes into the CC environment's specific, vendor-defined
  Measurement Registers (MRs).

### 3.3 Event Logging Formats and Intel TDX Mapping

When a virtual firmware possessing CC capabilities successfully measures
an event, it logs the data utilizing the exact format established by the
TCG Platform Firmware Profile, specifically adhering to the
`EFI_TCG2_EVENT_LOG_FORMAT_TCG_2` structure. This ensures immediate
structural compatibility with existing fleet management and log analysis
tools, easing enterprise adoption. Events that are generated late in the
boot sequence—specifically, any events occurring after the initial
`GetEventLog` function has been invoked by the OS—are securely stored in
a segregated `EFI_CONFIGURATION_TABLE` identified by the
`EFI_CC_FINAL_EVENTS_TABLE_GUID`. This table explicitly records the
version, the total number of recorded events, and a sequential list of
`CC_EVENT` data structures.

For environments specifically utilizing Intel Trust Domain Extension
(TDX), UEFI 2.10 provides a highly specific, standardized mapping logic
between legacy TPM PCRs and TDX Measurement Registers. This mapping
ensures that OS logic expecting specific data in specific PCRs finds the
equivalent cryptographic data in the correct TDX register:

| Legacy TPM PCR Index | TDX MR Index | Intel TDX Hardware Register Function                |
| -------------------- | ------------ | --------------------------------------------------- |
| 0                    | 0            | MRTD — Core initialization and early hypervisor     |
|                      |              | state.                                              |
| 1, 7                 | 1            | RTMR — Platform configuration and early OS loader   |
|                      |              | data.                                               |
| 2 through 6          | 2            | RTMR — Operational state measurements and           |
|                      |              | transitional firmware states.                       |
| 8 through 15         | 3            | RTMR — OS-defined measurements, user-space app      |
|                      |              | logic, and late-stage runtime configurations.       |

## 4. Advanced Memory Protection and W^X Paradigms

Memory management within the pre-boot and runtime firmware phases has
historically been an area deeply vulnerable to exploitation, particularly
via buffer overflows, stack smashing, and the execution of malicious
payloads in unconstrained memory spaces. UEFI 2.10 implements sweeping,
aggressive changes to memory protections, permanently deprecating
outdated models and instituting rigorous, attribute-based execution
controls.

### 4.1 The Deprecation of EFI_PROPERTIES_TABLE

A highly visible and structurally significant change in UEFI 2.10 is the
formal, absolute deprecation of the `EFI_PROPERTIES_TABLE` (Section
4.6.3). This legacy structure previously attempted to manage runtime
memory protection by indicating—via a simplistic bitmask like
`EFI_PROPERTIES_RUNTIME_MEMORY_PROTECTION_NON_EXECUTABLE_PE_DATA`—whether
runtime code and runtime data sections were separated.

However, this legacy approach proved structurally inflexible and prone to
severe interoperability issues. Specifically, splitting runtime code
memory map descriptors into underlying code and data sections caused
critical failures with operating systems that invoked the
`SetVirtualAddressMap()` function without realizing there was a complex,
undocumented relationship between these fragmented runtime descriptors.
Because the table failed to provide the necessary granularity, it has
been marked as deprecated, with warnings that it should no longer be used
and will be entirely removed from future versions of the specification.

In its place, UEFI 2.10 elevates the `EFI_MEMORY_ATTRIBUTES_TABLE`
(Section 4.6.4) as the exclusive, modern mechanism for declaring
fine-grained memory protections. This advanced table allows the firmware
to annotate specific, highly targeted sub-regions within the broader
memory blocks returned by `GetMemoryMap()`. Each descriptor within this
table can actively dictate which pages are read-only, writeable, or
executable, providing the OS with a precise, non-overlapping
topographical map of memory execution privileges.

### 4.2 The EFI_MEMORY_ATTRIBUTE_PROTOCOL and W^X Enforcement

To facilitate dynamic, run-time control over these memory page
permissions, UEFI 2.10 introduces the `EFI_MEMORY_ATTRIBUTE_PROTOCOL`
(Section 37.7.1, Mantis# 2262).

During the complex UEFI boot process, the memory attributes of specific
physical regions often require rapid, secure modification. A prime
example is the loading of a compressed OS kernel or a heavily obfuscated
firmware payload. Initially, the target memory region where the binary
will be unpacked must be aggressively writeable to allow the
decompression algorithm to extract the raw binary data. However,
immediately after extraction, leaving that region writeable while
executing it constitutes a massive security vulnerability. The region
must be instantly locked down—transitioned from writeable to
executable—to safely execute the code.

The `EFI_MEMORY_ATTRIBUTE_PROTOCOL` provides the standardized,
architecture-agnostic API to execute these transitions securely via its
`GetMemoryAttributes`, `SetMemoryAttributes`, and
`ClearMemoryAttributes` functions. For instance, `SetMemoryAttributes`
takes the base address, length, and the new attribute mask, allowing
precise manipulation of the page tables.

Furthermore, standardizing on the UEFI 2.10 memory models enables
operating systems and platform vendors (such as Microsoft's Project Mu)
to enforce rigorous Enhanced Memory Protections. The overarching security
paradigm is **W^X (Write XOR Execute)**—a strict mandate ensuring that
absolutely no address range in the system memory map can be
simultaneously readable, writable, and executable.

Under these enhanced protections, compliant UEFI 2.10 implementations
must adhere to the following strict rules:

- **Null Pointer Protection:** Page 0 in physical system memory must be
  explicitly marked as `EFI_MEMORY_RP` (Read-Protected) to immediately
  trap and halt upon null pointer dereferences, a common vector for
  exploits.
- **Stack Protection:** Application Processor (AP) and Boot Strap
  Processor (BSP) execution stacks must be strictly marked as
  `EFI_MEMORY_XP` (Execute-Protected) to completely neutralize
  stack-smashing code execution. Furthermore, these stacks must feature a
  read-protected guard page (`EFI_MEMORY_RP`) at the absolute bottom to
  instantly catch and halt stack overflow conditions.
- **Default Allocation Security:** Any calls to foundational memory
  services, such as `EFI_BOOT_SERVICES.AllocatePages` or `AllocatePool`,
  must, by default, return memory flagged with the `EFI_MEMORY_XP`
  attribute. This forces developers to explicitly, consciously request
  execution privileges via the `SetMemoryAttributes` protocol only when
  absolutely necessary, drastically reducing the overall executable
  surface area of the firmware.
- **Code vs. Data Separation:** Loaded image sections marked with the
  data characteristic must be forced to `EFI_MEMORY_XP`, while sections
  holding the actual code must be forced to `EFI_MEMORY_RO` (Read-Only).

Additionally, UEFI 2.10 introduces support for ISA-specific memory
attributes within these descriptors (Mantis# 2229), allowing the
firmware to pass highly specific, architecture-dependent cache or
execution hints directly to the OS memory manager. A runtime indicator
for Forward Control Flow Guard Instructions (Mantis# 2292) was also
added to further harden the runtime execution environment against
return-oriented programming (ROP) attacks.

## 5. Expansion of Processor Architecture Bindings

As the global semiconductor industry rapidly diversifies beyond the
traditional x86 and ARM duopoly—driven by geopolitical pressures,
open-source initiatives, and specialized computing needs—UEFI must
evolve to provide standardized initialization frameworks for emerging
Instruction Set Architectures (ISAs). Version 2.10 achieves this by
introducing comprehensive, native architectural bindings for both RISC-V
and LoongArch.

### 5.1 RISC-V Platform Integration and Handoff

RISC-V, a highly modular, open-standard ISA, has gained immense global
traction across both deeply embedded systems and high-performance
computing domains. UEFI 2.10 fully integrates RISC-V hardware
initialization, updating corresponding specifications to align with the
latest RISC-V architectural manuals (Mantis# 2139) and defining strict,
unforgiving handoff states between the firmware and the operating system
(Section 2.3.7).

When a UEFI 2.10 compliant firmware transfers ultimate control to a
RISC-V operating system, the system environment must adhere to precise
architectural constraints. The execution must occur with the RISC-V boot
hardware thread (hart) operating in Supervisor mode (or HS mode if
hypervisor extensions are present). Furthermore, the memory addressing
must be locked into Bare mode, ensuring that no virtual page table entry
translation or protection mechanisms are active during the critical
handoff boundary.

To manage the passing of critical boot parameters, UEFI 2.10 dictates a
highly specific utilization of the RISC-V C calling conventions. Unlike
legacy architectures that often relied on complex stack pushes, RISC-V
utilizes a register-heavy approach. When the UEFI image entry point is
invoked, it receives exactly two parameters, heavily structured into the
standard architectural registers:

- **Register x10 (a0):** Contains the `EFI_HANDLE`, representing the
  UEFI image handle for the executing binary.
- **Register x11 (a1):** Contains the physical pointer to the
  `EFI_SYSTEM_TABLE`, the fundamental gateway to all Boot and Runtime
  services.
- **Register x1 (ra):** Contains the Return Address, preserving the
  execution flow if the binary intends to exit and return control to the
  boot manager.

Because RISC-V systems are frequently heterogeneous—featuring highly
asymmetric core designs and varied capabilities across different
harts—the firmware is mandated to convey this complex topology to the
OS accurately. If the target bootable image demands it, the firmware must
expose detailed hardware capabilities via SMBIOS (specifically utilizing
record type 44) or by publishing a Flattened Device Tree Blob (DTB)
directly into the EFI Configuration Table. To prevent architectural
conflicts and race conditions, launched EFI binaries must utilize the
newly implemented `RISCV_EFI_BOOT_PROTOCOL.GetBootHartId()` function to
determine their execution thread, explicitly ignoring any legacy boot
hart IDs previously derived from SMBIOS or Device Trees as described in
older 2.9 specifications.

Furthermore, systems implementing PCIe on RISC-V must adhere to strict
Boot Requirement Specifications (BRS), such as rule UIO_010, which
dictates that the firmware must always initialize all root complex
hardware and perform resource assignments for all endpoints, even in a
scenario where the system is booting from a non-PCIe device.

### 5.2 LoongArch Platform Integration

Simultaneously, UEFI 2.10 integrates comprehensive, end-to-end support
for the LoongArch architecture (Section 2.3.8, Mantis# 2313, 2337),
ensuring that Chinese-developed processors have a standardized path to
boot modern, secure operating systems. LoongArch operates as a highly
modular Reduced Instruction Set Computer (RISC) architecture,
fundamentally divided into 32-bit (LA32) and 64-bit (LA64)
implementations.

To facilitate absolute binary compatibility and rapid execution
validation by the OS loader, the UEFI specification defines strict
Machine Type Identifiers for LoongArch PE/COFF image headers:

- `EFI_IMAGE_MACHINE_LOONGARCH32` mapped to the hexadecimal value
  `0x6232`.
- `EFI_IMAGE_MACHINE_LOONGARCH64` mapped to the hexadecimal value
  `0x6264`.

The handoff state for LoongArch heavily mirrors the RISC-V and broader
RISC paradigms, optimizing for rapid, register-based parameter passing
rather than relying on slower, memory-intensive stack-based operations.
Upon the invocation of a LoongArch UEFI image entry point, the firmware
passes the essential data through specific Application Binary Interface
(ABI) registers:

- **Register r4 (a0):** Contains the `EFI_HANDLE`.
- **Register r5 (a1):** Contains the physical pointer to the
  `EFI_SYSTEM_TABLE`.
- **Register r1 (ra):** Contains the Return Address.

This comprehensive architectural integration ensures that low-level
firmware drivers (such as PCI Option ROMs), UEFI pre-boot diagnostic
applications, and complex operating system debuggers can now be developed
natively for LoongArch environments using standardized,
architecture-agnostic UEFI C-code.

## 6. Network Booting and Advanced Device Path Innovations

The physical topology of modern enterprise computing is fundamentally
shifting from monolithic, chassis-based local storage to highly
disaggregated, network-attached architectures. To robustly support the
booting of operating systems located on high-performance fabric arrays,
UEFI 2.10 heavily updates its Messaging Device Path structures, moving
beyond traditional local disk paradigms.

### 6.1 NVMe over Fabrics (NVMe-oF) Device Paths

Non-Volatile Memory Express (NVMe) has become the undisputed de facto
standard for high-speed storage, leveraging the PCIe bus to bypass legacy
SATA bottlenecks. While previous UEFI versions supported local
PCIe-attached NVMe booting, UEFI 2.10 introduces the NVMe over Fabric
(NVMe-oF) Namespace Device Path (Section 10.3.4.33, Mantis# 2315).

This critical enhancement allows the platform firmware to locate,
address, and execute a boot sequence from an NVMe namespace that resides
entirely on a remote storage subsystem over an RDMA or TCP/IPv4 network
fabric. Because the boot drive is not physically attached to the
motherboard, the device path structure in UEFI must dynamically
encapsulate the complex network routing required to reach the target
across switching layers.

For example, an NVMe-oF namespace device path structure might appear as
`PciRoot(0)/Pci(19|0)/Mac(001320F5FA77,0x01)`. This structure deeply
intertwines the local hardware PCI root bridge with the physical MAC
address of the Network Interface Card (NIC) utilized to access the
fabric, pointing toward the remote NVM Subsystem UUID. Because UEFI
Device Path nodes are inherently variable-length, byte-packed data
structures that can theoretically appear on any byte boundary in memory,
the specification demands that all code interacting with these new
NVMe-oF paths must assume the fields are strictly unaligned. This
design constraint ensures that the UEFI boot manager can sequentially
traverse and decode the device nodes—jumping from the local PCI root,
through the NIC, across the IPv4 configuration, to the remote storage
target—without triggering fatal hardware alignment faults during the
fragile pre-boot phase.

### 6.2 Advanced Network Protocol Bindings

Beyond storage fabrics, UEFI 2.10 continues to refine its massive suite
of network protocols to support massive datacenter deployments. The
specification details intricate service binding protocols for IPv6
(`EFI_IP6_SERVICE_BINDING_PROTOCOL`) and robust REST service device paths
(corrected in Mantis# 2327). Furthermore, the
`EFI_REDFISH_DISCOVER_PROTOCOL` definitions have been heavily revised to
match real-world implementations, allowing out-of-band baseboard
management controllers (BMCs) to seamlessly provision and update UEFI
settings over the network using DMTF Redfish standards.

## 7. Boot Services, Runtime Services, and the OS Handoff

At its core, the entire UEFI architecture is defined by its strict
delineation of operational phases, centrally managed via the
`EFI_SYSTEM_TABLE`. This table serves as the central directory for all
platform-related information and exposes the two primary classes of
function calls: Boot Services and Runtime Services. Understanding the
interplay between these services, and the restrictions placed upon them
by UEFI 2.10, is essential for OS kernel development.

### 7.1 Boot Services Restrictions and TPLs

**Boot Services (Section 7):** These functions encompass memory
allocation, event timers, generic protocol handling, and image loading
capabilities. Examples include `CreateEvent`, `WaitForEvent`, and
`AllocatePages`. They execute at varying Task Priority Levels (TPLs),
ranging from the lowest priority `TPL_APPLICATION` (value 4, used for
general UI and execution), through `TPL_CALLBACK` (value 8), up to the
high-priority `TPL_NOTIFY` (value 16).

Boot Services are inherently transient; they exist solely to initialize
hardware, present the pre-boot UI, and load the OS into memory. The OS
loader is explicitly and mandatorily required to call
`ExitBootServices()`. This is an irreversible, pivotal function that
permanently terminates the pre-boot environment, reclaims the firmware's
vast memory pages for OS usage, and permanently strips the system of all
Boot Service capabilities. Any attempt by the OS to call a Boot Service
after this boundary results in a catastrophic system crash.

### 7.2 Runtime Services and the EFI_RT_PROPERTIES_TABLE

**Runtime Services (Section 8):** In stark contrast, Runtime Services
survive the `ExitBootServices()` transition. They provide ongoing, highly
structured, OS-accessible interfaces for critical, low-level hardware
interactions. These encompass Variable Services (such as `GetVariable`
and `SetVariable`, absolutely essential for reading and writing
non-volatile Secure Boot keys), Time Services (interacting with the
Real-Time Clock via `GetTime` and `SetTime`), Capsule Update mechanisms
(`UpdateCapsule` for flashing the BIOS from the OS), and Virtual Memory
Services.

However, UEFI 2.10 acknowledges that not all platforms—especially highly
constrained IoT profiles defined in the Conformance Profiles section—can
or should support every single Runtime Service after the OS assumes
control. Implementing full NVRAM writing capabilities on a simple sensor,
for example, is unnecessary and increases the attack surface.

If a specific `EFI_RUNTIME_SERVICES` call is not supported at runtime,
the firmware must publish an `EFI_RT_PROPERTIES_TABLE` (Section 4.6.2)
indicating precisely which services remain functional. This table
utilizes a bitmask attribute (e.g.,
`#define EFI_RT_SUPPORTED_GET_TIME 0x0001`,
`#define EFI_RT_SUPPORTED_SET_VARIABLE 0x0040`) to act as an
architectural hint to the OS. The OS is theoretically free to ignore this
hint, but to prevent system crashes, the platform firmware is strictly
mandated to provide safe, callable stub implementations of any
unsupported runtime services. If the OS calls a stubbed service, it must
gracefully return an `EFI_UNSUPPORTED` status code rather than causing a
system halt or unhandled exception.

Furthermore, unlike the opaque System Management Mode (SMM) which
interrupts the OS asynchronously and halts all hardware threads to handle
an event, UEFI Runtime Services only execute when explicitly and
intentionally invoked by the operating system, operating at the exact
same privilege level as the caller and avoiding execution hiccups.

| Service Category            | Key Functions                       | Notes & Constraints                          |
| --------------------------- | ----------------------------------- | -------------------------------------------- |
| **Variable Services**       | `GetVariable()`, `SetVariable()`    | Essential for Crypto Agility; restricted by  |
|                             |                                     | Secure Boot auth descriptors.                |
| **Time Services**           | `GetTime()`, `SetTime()`,           | Often stubbed in IoT Profiles; relies on     |
|                             | `GetWakeupTime()`                   | `EFI_RT_PROPERTIES_TABLE` bitmasks.          |
| **Virtual Memory Services** | `SetVirtualAddressMap()`,           | Interacts with `EFI_MEMORY_ATTRIBUTES_TABLE` |
|                             | `ConvertPointer()`                  | to maintain W^X protections post-boot.       |
| **Firmware Update**         | `UpdateCapsule()`,                  | Critical for 2026 Secure Boot key updates;   |
|                             | `QueryCapsuleCapabilities()`        | relies on Firmware Mgmt Protocol.            |

## Conclusion

The UEFI Specification Release 2.10 is not a mere iterative or
administrative update; it is a profound structural realignment of the
computing industry's foundational software layer. By introducing
Conformance Profiles, the specification intelligently bifurcates its
evolutionary path, offering a stripped-down, highly efficient vector for
the exploding IoT, automotive, and embedded markets, thereby preventing
the monolithic bloat that plagued legacy BIOS systems. Simultaneously,
the profound, uncompromising enhancements to Cryptographic Agility and
the introduction of the hardware-backed Confidential Computing extension
address the existential threats facing modern enterprise
architecture—namely, the advent of quantum decryption capabilities and
the collapse of the traditional datacenter security perimeter. As the
global industry approaches the critical June 2026 expiration of legacy
Secure Boot certificates, the highly structured, agile frameworks
mandated within UEFI 2.10 transition from theoretical architectural
ideals into urgent, operational necessities for maintaining the integrity
of global computing platforms.
