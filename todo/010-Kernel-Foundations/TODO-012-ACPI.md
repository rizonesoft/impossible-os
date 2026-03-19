# 042-ACPI — Advanced Configuration & Power Interface Subsystem

> **Goal:** Evolve from the current minimal ACPI stub (RSDP/XSDT/MADT parsing in
> `acpi.c`) into a full ACPI subsystem with an AML interpreter, namespace
> construction, GPE event routing, thermal zone management, processor power/
> performance states, and ACPI 6.5 platform capabilities — enabling the OS to
> take complete OSPM (OS-directed Power Management) ownership of the hardware.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (ACPI table copies, AML workspace, RSDT/XSDT). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All section numbers, register offsets, and state definitions reference the
> [ACPI 6.5 Specification](file:///home/derickpayne/impossible-os/specs/acpi-6.5.md)
> (UEFI Forum, 2022). The spec summary is in the repo at `specs/acpi-6.5.md`.

> [!NOTE]
> **Cross-references:**
> - [TODO-065-Power-Management.md](../060-Hardware-Drivers/TODO-065-Power-Management.md) — User-facing power (shutdown sequence, Start Menu, profiles)
> - [TODO-063-Drivers.md §9](../060-Hardware-Drivers/TODO-063-Drivers.md) — ACPI shutdown/reboot registers, S3, battery
> - [TODO-063-Drivers.md §2.2](../060-Hardware-Drivers/TODO-063-Drivers.md) — APIC/IOAPIC (depends on MADT)

---

## 1. ACPI Table Infrastructure

### 1.1 Complete Table Discovery & Validation ✅ (Partial)

**Prompt:** The current `acpi.c` locates the RSDP via UEFI and parses basic XSDT/MADT entries. Extend this to validate all table checksums, parse every XSDT entry by signature, and store a table registry for lookup by signature. Map each table's physical memory via identity-mapping. Implement `acpi_find_table(signature)` to return a pointer to any table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: complete table discovery"`. Add notes directly in this TODO section.

- [x] Locate RSDP via UEFI System Table (EFI_ACPI_TABLE_GUID) *(done in boot_info)*
- [x] Validate RSDP checksum (first 20 bytes for v1, full 36 bytes for v2+)
- [x] Parse XSDT: iterate 64-bit pointers to all system description tables
- [ ] Validate each table: verify `length` field, compute and verify checksum (sum of all bytes = 0)
- [ ] Build table registry: array of `{ signature, physical_addr, length }` for all discovered tables
- [ ] Implement `acpi_find_table(const char sig[4])` — search registry, return mapped pointer
- [ ] Parse required tables by signature:
  - [x] `APIC` (MADT) — interrupt controller topology *(done, used for APIC/IOAPIC)*
  - [ ] `FACP` (FADT) — fixed hardware description, PM registers, boot flags
  - [ ] `DSDT` — main differentiated definition block (AML bytecode)
  - [ ] `SSDT` — supplementary definition blocks (additional AML)
  - [ ] `HPET` — high-precision event timer base address
  - [ ] `MCFG` — PCI Express memory-mapped config space base
  - [ ] `BGRT` — boot graphics resource (OEM splash image)
- [ ] Log: `[ACPI] Found N tables: FACP APIC DSDT SSDT HPET MCFG ...`
- [ ] Commit: `"acpi: complete table discovery"`

### 1.2 FADT Parsing (Fixed ACPI Description Table)

**Prompt:** The FADT (signature `"FACP"`) is the central repository for fixed ACPI hardware parameters. Parse all critical fields: PM1a/b event and control block addresses (64-bit `X_PM1a_EVT_BLK`, `X_PM1a_CNT_BLK`), PM timer block, GPE block addresses, FACS pointer, DSDT pointer, SCI interrupt number, `SMI_CMD` port, `ACPI_ENABLE`/`ACPI_DISABLE` commands, Fixed Feature Flags (`WBINVD`, `HW_REDUCED_ACPI`, `PERSISTENT_CPU_CACHES`), `Preferred_PM_Profile`, `RESET_REG` with `RESET_VALUE`, and IA-PC Boot Architecture Flags. Store in a `struct acpi_fadt_info` for kernel-wide use. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: FADT parsing"`. Add notes directly in this TODO section.

- [ ] Parse FADT from table registry (`acpi_find_table("FACP")`)
- [ ] Extract PM register addresses (prefer 64-bit `X_*` fields over 32-bit legacy):
  - [ ] `X_PM1a_EVT_BLK` / `X_PM1b_EVT_BLK` — PM1 event status/enable registers
  - [ ] `X_PM1a_CNT_BLK` / `X_PM1b_CNT_BLK` — PM1 control (SLP_TYP, SLP_EN)
  - [ ] `X_PM2_CNT_BLK` — PM2 control (arbiter disable)
  - [ ] `X_PM_TMR_BLK` — PM timer (24-bit or 32-bit counter)
  - [ ] `X_GPE0_BLK` / `X_GPE1_BLK` — General Purpose Event registers
- [ ] Extract key fields:
  - [ ] `SCI_INT` — System Control Interrupt number (routed to IOAPIC)
  - [ ] `SMI_CMD` — I/O port for SMI commands
  - [ ] `ACPI_ENABLE` / `ACPI_DISABLE` — values to write to `SMI_CMD`
  - [ ] `RESET_REG` + `RESET_VALUE` — for ACPI reboot
  - [ ] `Preferred_PM_Profile` — Desktop/Mobile/Workstation/Server/Tablet
  - [ ] `IAPC_BOOT_ARCH` — legacy device flags (8042, CMOS RTC, VGA, MSI)
- [ ] Parse Fixed Feature Flags:
  - [ ] `WBINVD` / `WBINVD_FLUSH` — cache invalidation behavior
  - [ ] `HW_REDUCED_ACPI` — hardware-reduced platform (no fixed registers)
  - [ ] `HEADLESS` — no display attached
  - [ ] `LOW_POWER_S0_IDLE` — Modern Standby capable
- [ ] Store all in `struct acpi_fadt_info` (global, read-only after init)
- [ ] Expose via Registry: `HKLM\HARDWARE\ACPI\FADT\*` (PM profile, revision, flags)
- [ ] Commit: `"acpi: FADT parsing"`

### 1.3 MCFG Parsing (PCIe ECAM)

**Prompt:** The MCFG table (PCI Express Memory-mapped Configuration space) provides the base address for PCIe Extended Configuration Access Mechanism (ECAM). Each entry maps a segment group + bus range to a physical base address. This enables accessing PCI configuration space beyond the legacy 256-byte limit (up to 4096 bytes per function) via MMIO. The current PCI code uses legacy I/O ports (`0xCF8`/`0xCFC`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: MCFG and PCIe ECAM support"`. Add notes directly in this TODO section.

- [ ] Parse MCFG table: extract base address, segment group, start/end bus numbers
- [ ] Map ECAM region via identity-mapping (typically 256 MB per segment for buses 0–255)
- [ ] Implement `pci_ecam_read(seg, bus, dev, fn, offset)` — MMIO-based config read
- [ ] Implement `pci_ecam_write(seg, bus, dev, fn, offset, value)` — MMIO config write
- [ ] Extended config space: access offsets 0x100–0xFFF (PCIe capabilities, AER, etc.)
- [ ] Fallback: if MCFG not present, use legacy I/O port mechanism (current behavior)
- [ ] Commit: `"acpi: MCFG and PCIe ECAM support"`

---

## 2. ACPI Mode Transition

### 2.1 Enable ACPI Mode (SCI_EN)

**Prompt:** After parsing the FADT, the OS must transition from legacy mode (SMI-based) to ACPI mode (SCI-based). Check `PM1a_CNT.SCI_EN` — if already set, the platform is in ACPI mode (common for UEFI platforms). If not, write `ACPI_ENABLE` to the `SMI_CMD` port, then poll `SCI_EN` until it sets (up to 3 seconds). Once in ACPI mode the OS owns all power management hardware. Register the SCI as an interrupt handler. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: enable ACPI mode"`. Add notes directly in this TODO section.

- [ ] Read `PM1a_CNT_BLK` → check `SCI_EN` bit (bit 0)
- [ ] If `SCI_EN == 0` (legacy mode):
  - [ ] Write `ACPI_ENABLE` value to `SMI_CMD` I/O port
  - [ ] Poll `PM1a_CNT.SCI_EN` until set (timeout 3 seconds)
  - [ ] Log: `[ACPI] Transitioned to ACPI mode`
- [ ] If `SCI_EN == 1` (already ACPI mode — typical for UEFI):
  - [ ] Log: `[ACPI] Already in ACPI mode (UEFI platform)`
- [ ] Register SCI interrupt handler via IOAPIC (IRQ = FADT `SCI_INT`)
- [ ] SCI handler: check PM1 status → check GPE status → dispatch events
- [ ] For Hardware-Reduced ACPI (`HW_REDUCED_ACPI` flag): skip PM register setup entirely
- [ ] Commit: `"acpi: enable ACPI mode"`

---

## 3. AML Interpreter (ACPICA Integration)

### 3.1 ACPICA Integration

**Prompt:** Implementing a full AML interpreter from scratch is impractical (~200K lines). The industry-standard solution is ACPICA (ACPI Component Architecture) — an OS-independent reference implementation maintained by Intel and released under a dual GPL-2.0/BSD-3 license. The BSD-3 license allows use in Impossible OS without GPL contamination. ACPICA provides: AML bytecode interpreter, namespace construction from DSDT/SSDT, control method evaluation (`_STA`, `_ON`, `_OFF`, `_PSx`, `_BST`, `_PRT`, `_CRS`, etc.), and event handling. Integrate ACPICA by implementing the OS Services Layer (OSL) — a ~40-function shim that provides memory allocation, I/O port access, PCI config access, spinlocks, semaphores, scheduling primitives, and interrupt handling. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: ACPICA integration"`. Add notes directly in this TODO section.

- [ ] Download ACPICA source (BSD-3 components only): `source/components/` + `source/include/`
- [ ] Place in `src/kernel/acpica/` with original license headers
- [ ] Implement OS Services Layer (`src/kernel/acpica/osl.c`):
  - [ ] Memory: `AcpiOsAllocate()` → `kmalloc()`, `AcpiOsFree()` → `kfree()`
  - [ ] Memory mapping: `AcpiOsMapMemory()` → identity-map, `AcpiOsUnmapMemory()`
  - [ ] I/O: `AcpiOsReadPort()` → `inb/inw/inl`, `AcpiOsWritePort()` → `outb/outw/outl`
  - [ ] PCI: `AcpiOsReadPciConfiguration()` → `pci_read_config_*`
  - [ ] Synchronization: `AcpiOsCreateLock()` → spinlock, `AcpiOsCreateSemaphore()` → semaphore
  - [ ] Scheduling: `AcpiOsSleep()` → `pit_sleep_ms()`, `AcpiOsStall()` → busy-wait µs
  - [ ] Interrupts: `AcpiOsInstallInterruptHandler()` → `idt_register_handler()`
  - [ ] Misc: `AcpiOsGetTimer()` → `pit_get_ticks()`, `AcpiOsPrintf()` → `printk()`
- [ ] Initialization sequence:
  - [ ] `AcpiInitializeSubsystem()` — init ACPICA internals
  - [ ] `AcpiInitializeTables()` — hand RSDP to ACPICA for table discovery
  - [ ] `AcpiLoadTables()` — load DSDT + all SSDTs
  - [ ] `AcpiEnableSubsystem()` — enable ACPI mode, install default handlers
  - [ ] `AcpiInitializeObjects()` — run `_INI` and `_STA` methods, build full namespace
- [ ] Verify: `AcpiGetTable("FACP")` returns valid FADT
- [ ] Verify: namespace walk shows devices (e.g., `\_SB.PCI0`, `\_SB.PCI0.SATA`)
- [ ] SPDX: `BSD-3-Clause` — verify no GPL source files included
- [ ] Commit: `"acpi: ACPICA integration"`

### 3.2 ACPI Namespace Browser

**Prompt:** After ACPICA initialization, the ACPI namespace is a tree of objects representing the platform's hardware. Implement a debug shell command `acpidump` that walks the namespace and prints device nodes, their `_HID` (Hardware ID), `_STA` (Status), and `_ADR` (Address). This is invaluable for debugging: it shows exactly what hardware ACPI exposes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: namespace browser debug tool"`. Add notes directly in this TODO section.

- [ ] Implement `acpi_walk_namespace()` callback that prints each node
- [ ] For each `ACPI_TYPE_DEVICE`:
  - [ ] Evaluate `_HID` → print hardware ID (e.g., `PNP0A08` for PCIe root)
  - [ ] Evaluate `_STA` → print status bits (Present, Enabled, Functioning)
  - [ ] Evaluate `_ADR` → print address (e.g., PCI device/function)
  - [ ] Evaluate `_CID` → print compatible IDs
- [ ] Shell command: `acpidump` → print full namespace tree
- [ ] Shell command: `acpidump \_SB.PCI0` → print specific subtree
- [ ] Log tree structure with indentation showing parent/child relationships
- [ ] Commit: `"acpi: namespace browser debug tool"`

---

## 4. Power State Management

### 4.1 Sleep State Discovery (\_S0–\_S5)

**Prompt:** The ACPI DSDT defines which sleep states the platform supports via `\_S0` through `\_S5` named objects. Each contains the `SLP_TYP` value to write to `PM1_CNT` when entering that state. Evaluate these objects via ACPICA to determine: which states are available, and the `SLP_TYP` values for each. Store the results for use by the shutdown (S5), sleep (S3), and hibernate (S4) code paths. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: sleep state discovery"`. Add notes directly in this TODO section.

- [ ] Evaluate `\_S0` through `\_S5` in DSDT namespace
- [ ] For each existing sleep state object, extract `SLP_TYPa` and `SLP_TYPb` values
- [ ] Store in table: `acpi_sleep_states[6] = { .supported, .slp_typ_a, .slp_typ_b }`
- [ ] Log: `[ACPI] Supported sleep states: S0 S3 S4 S5` (typical for QEMU)
- [ ] Wire `SLP_TYP_S5` to `acpi_poweroff()` (replaces hardcoded QEMU value)
- [ ] Wire `SLP_TYP_S3` to future `acpi_suspend()` (see TODO-100 §7)
- [ ] Commit: `"acpi: sleep state discovery"`

### 4.2 Device Power Management (\_PSx / \_PRx)

**Prompt:** ACPI defines device power states D0 (fully on) through D3 (off). The OS transitions devices via `_PS0` (enter D0), `_PS1`, `_PS2`, `_PS3` control methods. Power resources (`_PR0`, `_PR1`, etc.) define which shared power rails a device needs in each state. Implement a `acpi_set_device_power(handle, state)` function that evaluates the appropriate `_PSx` method and manages power resource reference counts. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: device power management"`. Add notes directly in this TODO section.

- [ ] Implement `acpi_set_device_power(ACPI_HANDLE dev, int state)`:
  - [ ] Evaluate `_PSx` method for the target state (x = 0, 1, 2, 3)
  - [ ] Check `_STA` to verify device supports the transition
- [ ] Power resource management:
  - [ ] Evaluate `_PRx` to find required power resources for each state
  - [ ] Reference-count shared power resources (`_ON`, `_OFF`, `_STA`)
  - [ ] Turn off power resources only when last consumer releases them
- [ ] Integrate with driver model: `driver_suspend(dev)` → `acpi_set_device_power(dev, D3)`
- [ ] Integrate with resume: `driver_resume(dev)` → `acpi_set_device_power(dev, D0)`
- [ ] Commit: `"acpi: device power management"`

---

## 5. GPE Event Framework

### 5.1 General Purpose Events

**Prompt:** GPEs are the ACPI mechanism for generic hardware events (thermal alerts, hot-plug notifications, battery status changes, lid open/close). The GPE registers (`GPE0_STS`/`GPE0_EN`, `GPE1_STS`/`GPE1_EN`) are located via the FADT. When a GPE fires, the SCI handler reads the GPE status registers, identifies the active bit, and dispatches to the corresponding AML method (`_Exx` for edge-triggered, `_Lxx` for level-triggered). ACPICA handles GPE dispatch internally; the OS just needs to wire the SCI and enable the desired GPEs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: GPE event framework"`. Add notes directly in this TODO section.

- [ ] Install GPE block handlers via `AcpiInstallGpeBlock()`
- [ ] Enable wake-capable GPEs: `AcpiSetupGpeForWake()` for power button, lid, etc.
- [ ] Enable runtime GPEs: `AcpiEnableGpe()` for thermal, battery notifications
- [ ] SCI handler flow (ACPICA-managed):
  - [ ] `AcpiEvGpeDetect()` → scan GPE status registers
  - [ ] `AcpiEvGpeDispatch()` → evaluate `_Exx` / `_Lxx` AML methods
- [ ] Implement `acpi_gpe_handler()` callback for custom GPE handling
- [ ] Test: verify GPE fires on QEMU power button press
- [ ] Commit: `"acpi: GPE event framework"`

### 5.2 Fixed Event Handling

**Prompt:** ACPI defines Fixed Events for standardized hardware events: Power Button, Sleep Button, and RTC Alarm. These use dedicated bits in the PM1 status/enable registers (not GPEs). Register handlers for each: power button triggers shutdown prompt, sleep button triggers suspend, RTC alarm triggers scheduled wake. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: fixed event handlers"`. Add notes directly in this TODO section.

- [ ] Register Power Button handler: `AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, ...)`
  - [ ] Short press → show shutdown dialog (or sleep if configured)
  - [ ] Long press (> 4s) → force power-off via hardware (not software-controllable)
- [ ] Register Sleep Button handler: `AcpiInstallFixedEventHandler(ACPI_EVENT_SLEEP_BUTTON, ...)`
  - [ ] Trigger suspend-to-RAM (S3) if supported
- [ ] Register RTC Alarm: `PM1_EN.RTC_EN` → scheduled wake from sleep
- [ ] Enable: set corresponding `PM1_EN` bits after handler registration
- [ ] Commit: `"acpi: fixed event handlers"`

---

## 6. Thermal Management

### 6.1 Thermal Zone Monitoring

**Prompt:** ACPI thermal zones define temperature thresholds and cooling policies. Each thermal zone (e.g., `\_TZ.THRM`) has: `_TMP` (current temperature, in tenths of Kelvin), `_PSV` (passive cooling threshold — throttle CPU), `_AC0`–`_AC9` (active cooling thresholds — engage fans), `_CRT` (critical threshold — emergency shutdown), and `_HOT` (hot threshold — enter S4). Implement periodic polling of `_TMP` and take action when thresholds are crossed. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: thermal zone monitoring"`. Add notes directly in this TODO section.

- [ ] Walk namespace for all `ACPI_TYPE_THERMAL` objects
- [ ] For each thermal zone, evaluate:
  - [ ] `_TMP` — current temperature (deci-Kelvin, convert to °C: `(val - 2732) / 10`)
  - [ ] `_CRT` — critical temperature (trigger emergency shutdown)
  - [ ] `_HOT` — hot temperature (trigger S4 hibernate)
  - [ ] `_PSV` — passive cooling threshold (throttle CPU via P-states)
  - [ ] `_AC0`–`_AC9` — active cooling thresholds (fan speed levels)
  - [ ] `_TC1`, `_TC2`, `_TSP` — cooling algorithm constants
- [ ] Implement polling timer: read `_TMP` every `_TSP` deciseconds (typically 10 = 1s)
- [ ] Thermal policy actions:
  - [ ] `_TMP < _PSV`: normal operation
  - [ ] `_TMP >= _PSV`: engage passive cooling (reduce CPU P-state)
  - [ ] `_TMP >= _ACx`: engage active cooling (turn on fan via `_ALx` device list)
  - [ ] `_TMP >= _HOT`: initiate S4 hibernate
  - [ ] `_TMP >= _CRT`: **emergency shutdown** — bypass clean shutdown, immediate S5
- [ ] Expose via Registry: `HKLM\HARDWARE\ACPI\ThermalZone\Temperature`, `CriticalThreshold`
- [ ] System tray: optional temperature indicator (if thermal zone exists)
- [ ] Commit: `"acpi: thermal zone monitoring"`

---

## 7. Processor Power & Performance States

### 7.1 Processor C-States (Idle Power)

**Prompt:** Processor C-states control idle power consumption. C0 = executing, C1 = halt (mandatory, `HLT` instruction), C2 and C3 = deeper idle (lower power, longer wake latency). The ACPI namespace defines available C-states via `_CST` (C-State Table) objects under each processor device. Each entry specifies: type (C1/C2/C3), latency (µs), power (mW), and the register to read to enter the state. The OS idle loop selects the appropriate C-state based on predicted idle duration. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: processor C-states"`. Add notes directly in this TODO section.

- [ ] Enumerate processor devices: `_HID ACPI0007` in namespace
- [ ] Evaluate `_CST` for each processor → list of available C-states
- [ ] For each C-state entry: type, register (FFH = MWAIT, I/O port), latency, power
- [ ] Implement idle loop C-state selection:
  - [ ] Short expected idle → C1 (`HLT` or `MWAIT C1`)
  - [ ] Medium idle → C2 (MWAIT C3 on modern hardware)
  - [ ] Long idle → C3 (deepest — requires cache flush on older CPUs)
- [ ] For `MWAIT`-based entry: use `MONITOR` + `MWAIT` with C-state hint
- [ ] For I/O-based entry: read from specified I/O port to enter C-state
- [ ] Track C-state residency for power profiling
- [ ] Commit: `"acpi: processor C-states"`

### 7.2 Processor P-States (DVFS)

**Prompt:** P-states (Performance States) control CPU frequency and voltage while actively executing (C0). `_PSS` (Performance Supported States) returns an array of { frequency, power, latency, control, status } tuples. `_PCT` (Performance Control) defines the register to write the desired P-state. `_PPC` (Performance Present Capabilities) limits the maximum P-state. On modern Intel/AMD CPUs, prefer Hardware P-states (HWP / CPPC) via `CPPC2` ACPI objects, where the hardware autonomously selects the frequency within OS-defined bounds. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: processor P-states (DVFS)"`. Add notes directly in this TODO section.

- [ ] Evaluate `_PSS` per processor → table of available P-states
- [ ] Evaluate `_PCT` → control/status register addresses
- [ ] Evaluate `_PPC` → current performance limit (may change at runtime via Notify)
- [ ] Implement `acpi_set_pstate(cpu, index)`:
  - [ ] Write P-state control value to `_PCT` control register
  - [ ] Read `_PCT` status register to confirm transition
- [ ] Intel HWP / CPPC2 support (modern CPUs):
  - [ ] Check for `CPPC2` device in namespace
  - [ ] Write desired performance bounds to `IA32_HWP_REQUEST` MSR
  - [ ] Let hardware autonomously select frequency within bounds
- [ ] Integrate with power profiles (TODO-100 §8):
  - [ ] Performance: max P-state, no C-state throttling
  - [ ] Balanced: let HWP auto-manage
  - [ ] Power Saver: lowest P-state, aggressive C-states
- [ ] Commit: `"acpi: processor P-states (DVFS)"`

---

## 8. PCI Interrupt Routing

### 8.1 `_PRT` (PCI Routing Table)

**Prompt:** ACPI defines how PCI interrupt pins (INTA#–INTD#) map to system interrupts via the `_PRT` (PCI Routing Table) method on PCI bridge devices. Each entry specifies: device address, pin (A/B/C/D), and either a fixed GSI (Global System Interrupt) or a link device that can be programmed. Without `_PRT`, PCI devices can't route interrupts correctly on real hardware. Evaluate `_PRT` under `\_SB.PCI0` and configure the IOAPIC routing table accordingly. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: PCI interrupt routing via _PRT"`. Add notes directly in this TODO section.

- [ ] Evaluate `\_SB.PCI0._PRT` → array of { address, pin, source, source_index }
- [ ] For each entry with direct GSI (source = 0): route pin to `source_index` via IOAPIC
- [ ] For each entry with link device (source = device handle):
  - [ ] Evaluate link device `_CRS` → current IRQ assignment
  - [ ] Evaluate `_PRS` → possible IRQ assignments
  - [ ] Program link device `_SRS` to assign an IRQ (avoid conflicts)
- [ ] Build PCI-to-GSI mapping table for use by PCI driver enumeration
- [ ] Update `pci_get_interrupt(dev)` to use ACPI routing instead of PCI config `Interrupt Line`
- [ ] Commit: `"acpi: PCI interrupt routing via _PRT"`

---

## 9. Embedded Controller (EC)

### 9.1 EC Driver (Laptop Support)

**Prompt:** Most laptops use an Embedded Controller (EC, `_HID PNP0C09`) for keyboard backlight, fan speed, battery charging, lid switch, and special function keys. The EC communicates via two I/O ports (typically `0x62` command and `0x66` data). The OS sends commands and reads data, but must respect the EC's Input Buffer Full (IBF) and Output Buffer Full (OBF) status bits. EC events fire via SCI → GPE → `_Qxx` methods. Without EC support, laptop-specific features are invisible. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: embedded controller driver"`. Add notes directly in this TODO section.

- [ ] Find EC device: namespace device with `_HID PNP0C09`
- [ ] Parse `_CRS` for command and data I/O port addresses
- [ ] Implement EC read/write protocol:
  - [ ] `ec_read(addr)`: write addr → command port, read data port (respect IBF/OBF)
  - [ ] `ec_write(addr, val)`: write addr+val → ports (respect IBF/OBF)
- [ ] Register EC GPE handler from `_GPE` method on EC device
- [ ] Handle EC SCI events → read EC query value → dispatch `_Qxx` AML methods
- [ ] Common EC events: lid open/close (`_LID`), AC adapter (`_PSR`), battery update (`_BST`)
- [ ] Commit: `"acpi: embedded controller driver"`

---

## 10. ACPI 6.5 Extended Features

### 10.1 Hardware-Reduced ACPI

**Prompt:** Hardware-Reduced ACPI platforms (indicated by FADT `HW_REDUCED_ACPI` flag set to 1) have **no fixed hardware registers** — no PM1, no PM2, no GPE blocks. All power management is done via GPIO-signaled events and AML control methods. This is the model for all ARM/ARM64 platforms and some modern x86 SoCs. When this flag is set, skip all PM register initialization and use GPIO-based event sources instead. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: hardware-reduced mode support"`. Add notes directly in this TODO section.

- [ ] Check FADT `HW_REDUCED_ACPI` flag during init
- [ ] If set:
  - [ ] Skip PM1/PM2/GPE register mapping entirely
  - [ ] Use GPIO-signaled interrupts for events (power button, lid, etc.)
  - [ ] Use `_AEI` (ACPI Event Information) objects for GPIO event sources
  - [ ] Evaluate `_EVT` methods for GPIO event handling
- [ ] Sleep transitions: use `_GTS` (Going To Sleep) and `_WAK` (Wake) methods only
- [ ] Log: `[ACPI] Hardware-Reduced mode — no fixed PM registers`
- [ ] Commit: `"acpi: hardware-reduced mode support"`

### 10.2 Platform Communications Channel (PCCT)

**Prompt:** PCCT (Platform Communications Channel Table) provides a high-speed shared-memory communication mechanism between the OS and platform firmware. Used by CPPC2 (for HWP P-state control), PCC OpRegions, and RASF. Each channel is a shared memory buffer with doorbell registers for signaling. ACPICA handles most of PCCT via its AML interpreter, but the OS must set up the shared memory regions and doorbell mechanism. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"acpi: PCCT shared memory channels"`. Add notes directly in this TODO section.

- [ ] Parse PCCT table: enumerate PCC subspace Type 0–4 entries
- [ ] For each subspace: map shared memory region, configure doorbell register
- [ ] Wire to ACPICA `PCC` OperationRegion handler
- [ ] Used by: CPPC2 (§7.2), RASF, platform telemetry
- [ ] Commit: `"acpi: PCCT shared memory channels"`

---

## Priority Order

| Priority | Section                         | Description                                                  |
|----------|---------------------------------|--------------------------------------------------------------|
| 🔴 P0    | 1.1 Table Discovery             | Foundation — must parse all tables before anything else      |
| 🔴 P0    | 1.2 FADT Parsing                | Required for PM registers, sleep states, reboot              |
| 🔴 P0    | 2.1 Enable ACPI Mode            | Must enter ACPI mode for OSPM ownership                      |
| 🟠 P1    | 3.1 ACPICA Integration          | AML interpreter — required for all advanced features         |
| 🟠 P1    | 4.1 Sleep State Discovery       | Needed for correct shutdown (S5) and sleep (S3) SLP_TYP      |
| 🟠 P1    | 5.2 Fixed Event Handlers        | Power button, sleep button — user interaction                |
| 🟠 P1    | 8.1 PCI Interrupt Routing       | Real hardware needs `_PRT` for correct IRQ routing           |
| 🟡 P2    | 1.3 MCFG / PCIe ECAM           | PCIe extended config space (AER, SR-IOV, etc.)               |
| 🟡 P2    | 5.1 GPE Event Framework        | Thermal, battery, hot-plug notifications                     |
| 🟡 P2    | 6.1 Thermal Zone Monitoring    | Prevent hardware damage, fan control                         |
| 🟡 P2    | 9.1 Embedded Controller        | Laptop keyboard backlight, fans, lid switch, battery         |
| 🟢 P3    | 7.1 C-States (Idle Power)      | Power efficiency in idle loop                                |
| 🟢 P3    | 7.2 P-States (DVFS)            | Dynamic frequency/voltage scaling                            |
| 🟢 P3    | 3.2 Namespace Browser          | Debug tool — helpful but not blocking                        |
| 🟢 P3    | 4.2 Device Power Management    | Per-device D0–D3 transitions                                 |
| 🔵 P4    | 10.1 Hardware-Reduced ACPI     | ARM/modern SoC support — stretch for x86 OS                  |
| 🔵 P4    | 10.2 PCCT Channels             | Firmware communication — needed for CPPC2                    |

---

## OS Comparison

| Feature                              | 🪟 Windows 11 (ACPI.sys)           | 🐧 Linux (ACPICA + drivers/acpi/)    | 🚀 Impossible OS                        |
| ------------------------------------ | ---------------------------------- | ------------------------------------ | --------------------------------------- |
| RSDP / XSDT parsing                 | ✅ HAL + ACPI.sys                   | ✅ Built-in                           | ✅ Done (basic)                          |
| MADT → APIC/IOAPIC                  | ✅                                  | ✅                                    | ✅ Done                                  |
| FADT parsing (PM registers)          | ✅ Full                             | ✅ Full                               | ⬜ §1.2 P0                              |
| MCFG → PCIe ECAM                    | ✅ PCI Express config               | ✅ `pci_mmcfg_init()`                 | ⬜ §1.3 P2                              |
| ACPI mode enable (SCI_EN)           | ✅                                  | ✅                                    | ⬜ §2.1 P0                              |
| AML interpreter                      | ✅ Microsoft AML engine             | ✅ ACPICA (BSD-3)                     | ⬜ §3.1 P1 — port ACPICA               |
| ACPI namespace                       | ✅ Full                             | ✅ Full (`/sys/firmware/acpi/`)        | ⬜ §3.1 P1                              |
| Sleep state discovery (\_Sx)         | ✅                                  | ✅                                    | ⬜ §4.1 P1                              |
| Device D-states (\_PSx)              | ✅ Full D0–D3                       | ✅ pm_runtime                         | ⬜ §4.2 P3                              |
| GPE event dispatch                   | ✅                                  | ✅ `acpi_ev_gpe_*`                    | ⬜ §5.1 P2                              |
| Power button / Sleep button          | ✅ ACPI button driver                | ✅ `button.c`                          | ⬜ §5.2 P1                              |
| Thermal zones                        | ✅ Windows Thermal Manager           | ✅ `thermal_zone_device`               | ⬜ §6.1 P2                              |
| C-states (processor idle)            | ✅ PPM + cpuidle                     | ✅ `intel_idle` / `acpi_idle`          | ⬜ §7.1 P3                              |
| P-states / HWP / CPPC               | ✅ PPM + Intel Speed Shift           | ✅ `intel_pstate` / `acpi-cpufreq`     | ⬜ §7.2 P3                              |
| PCI interrupt routing (\_PRT)        | ✅                                  | ✅ `acpi_pci_irq_*`                   | ⬜ §8.1 P1                              |
| Embedded Controller                  | ✅ ACPI EC driver                    | ✅ `ec.c`                              | ⬜ §9.1 P2                              |
| Hardware-Reduced ACPI                | ✅ (Surface, ARM)                   | ✅ (ARM64 only path)                   | ⬜ §10.1 P4                             |
| **AML interpreter (own vs ACPICA)** | ✅ **Custom Microsoft engine**      | ✅ **ACPICA (shared w/ BSD, Haiku)**   | ⬜ **§3.1 — port ACPICA (BSD-3)**       |
| **Full OSPM ownership**             | ✅                                  | ✅                                    | ⬜ **Requires §1–3 complete**           |
