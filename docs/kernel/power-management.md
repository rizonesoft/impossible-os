<!-- docs: covers=todo/02-kernel-core/TODO-26-power-management.md sources=src/kernel/acpi.c,include/kernel/acpi.h,src/kernel/pm_idle.c,include/kernel/pm.h,src/kernel/pm/power_callback.c,include/kernel/pm/power_callback.h,src/kernel/drivers/pci_pm.c,include/kernel/drivers/pci_pm.h,src/kernel/smp/smp.c,include/kernel/smp.h,src/kernel/test/test_acpi_power.c,src/kernel/test/test_pm_idle.c,src/kernel/test/test_smp_rendezvous.c reviewed=2026-09-28 order=26 -->
# Power Management

## What is it?

The ACPI-driven power management stack: sleep-state discovery, processor idle, the power and sleep hardware buttons, PCI device power states, S0ix and MWAIT capability detection, the SMP barrier every system-sleep transition needs, and integrity checks on the firmware tables all of this reads. The roadmap is large (forty sections); this page documents the part that ships and runs today. Suspend, hibernation, thermal management, CPU frequency scaling and the power-plan UI are still open and listed below.

## How does it work?

`acpi_power_init()` parses the `\_S1_`, `\_S3_` and `\_S4_` sleep objects out of the DSDT at Phase 2 boot and records each state's `SLP_TYPa`/`SLP_TYPb` values. `acpi_enter_sleep_state()` performs the generic ACPI PM1 sleep sequence (mask interrupts, preserve the rest of PM1_CNT, write `SLP_TYP` and `SLP_EN`, halt, confirm the wake through `WAK_STS`), but it refuses S3 and S4 today because none of the surrounding orchestration (device quiesce, cache flush, firmware waking vector, hibernation image) exists yet, and it refuses S5, which must go through `acpi_shutdown()` so the storage durability barrier runs ([`acpi.c`](../../src/kernel/acpi.c)). `acpi_enable_fixed_events()` and `acpi_register_sci()` wire the PM1 power-button and sleep-button events into a shared ACPI SCI, acknowledging in hard-IRQ context and deferring the policy action (a registry-configured sleep, hibernate, shutdown, lock or ignore per button) to a threaded DPC so the SCI handler itself never logs or blocks.

`pm_idle_c1()` is the one C1 idle primitive every idle site calls instead of a bare `sti; hlt` ([`pm_idle.c`](../../src/kernel/pm_idle.c)): it always halts when the caller's interrupts are enabled, because a stalled AP has no other way to notice new work, and it accumulates per-CPU idle cycles from the TSC. `pm_deep_idle_allowed()` is its advisory readiness signal for a future idle governor. A parallel MWAIT capability layer decodes CPUID leaf 5 sub-state counts into legal hint encodings and an interrupt-break rule, entirely as pure functions over supplied CPUID words, so it is provable without MWAIT-capable hardware; nothing yet executes `MONITOR`/`MWAIT`.

PCI device power is a config-space-only D0 to D3hot state machine ([`pci_pm.c`](../../src/kernel/drivers/pci_pm.c)): it discovers the PCI Power Management capability, enforces the legal transition table, applies the mandated recovery delay after each transition, and serializes every access to a device across the whole write, recover and verify sequence so a `PMCSR` write from one CPU cannot interleave with another's.

A system sleep transition needs every other processor provably parked, so `smp_rendezvous_begin()`/`smp_rendezvous_end()` implement a resumable, generation-tagged stop-the-world barrier on IPI vector `0xF9` ([`smp.c`](../../src/kernel/smp/smp.c)): the initiator snapshots the online-CPU set, IPIs every target, returns only once each has acknowledged the current generation, closes CPU admission for the duration, and fails closed on a timeout. Inside the window no lock, allocation, log call or synchronous cross-CPU operation is permitted. The primitive has shipped, but the sleep path does not consume it yet because S3 and S4 entry are still refused.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `acpi_power_init()`, `acpi_enter_sleep_state()`, `acpi_sleep_supported()` | Sleep-state discovery, generic PM1 sleep entry, per-state support query ([`acpi.h`](../../include/kernel/acpi.h)) |
| `acpi_power_button_event()`, `acpi_sleep_button_event()` | Registry-driven power and sleep button action dispatch |
| `pm_idle_c1()`, `pm_deep_idle_allowed()` | The race-safe C1 halt primitive and its advisory readiness predicate ([`pm.h`](../../include/kernel/pm.h)) |
| `pm_mwait_hint_encode()`, `pm_mwait_deepest_hint()`, `pm_mwait_idle_allowed()` | Pure MWAIT hint encoding and refusal rules over supplied CPUID words |
| `acpi_fadt_s0ix_capable()`, `acpi_s0ix_supported()` | FADT `LOW_POWER_S0` firmware advertisement |
| `pci_pmcap_find()`, `pci_set_d_state()`, `pci_get_d_state()` | PCI PM capability discovery and the D-state machine ([`pci_pm.h`](../../include/kernel/drivers/pci_pm.h)) |
| `pm_register_power_callback()` | Priority-ordered sleep and wake driver callback registration ([`power_callback.h`](../../include/kernel/pm/power_callback.h)) |
| `smp_rendezvous_begin()`, `smp_rendezvous_end()` | Resumable, generation-tagged stop-the-world CPU barrier ([`smp.h`](../../include/kernel/smp.h)) |

## How do I use it?

Power management is always active from Phase 2 boot; there is no flag to enable ACPI sleep-state or PCI PM discovery. The button actions are configured through `HKLM\SYSTEM\PowerControl` (`PowerButtonAction`, `SleepButtonAction`), read once at boot.

```bash
bash scripts/test.sh SUITE=boot   # ACPI power, button, PCI PM, MWAIT, S0ix suites
bash scripts/test.sh SUITE=x86    # SMP rendezvous barrier suite
```

The serial boot log reports what firmware advertised (`Sleep states: S1=... S3=... S4=... S5=yes`) and `stop-the-world rendezvous armed (vector 0xf9)`.

## What is not implemented yet?

- **S3 suspend to RAM.** The state is discovered and its `SLP_TYP` values are known, but entering it is refused: ACPI `_PTS`/`_WAK` evaluation, CPU state save and restore, and the firmware waking vector do not exist yet ([S3: Suspend to RAM](../../todo/02-kernel-core/TODO-26-power-management.md#3-s3-suspend-to-ram)).
- **The ACPI Embedded Controller for real traffic.** EC discovery and transaction code exists but is gated off pending GPE handling, which also blocks battery, lid and AC-adapter status ([ACPI Embedded Controller (EC) Driver](../../todo/02-kernel-core/TODO-26-power-management.md#5-acpi-embedded-controller-ec-driver)).
- **Runtime device idle management.** No per-device idle timer or `runtime_suspend`/`runtime_idle` pair exists to put an unused device into a low-power state outside a full system sleep ([Runtime Device Idle Management](../../todo/02-kernel-core/TODO-26-power-management.md#12-runtime-device-idle-management)).
- **CPU frequency and idle governors.** No P-state (HWP/CPPC) or C-state governor exists, and there is no `powercfg` or Power Options panel to configure one ([CPU Frequency Scaling Governor Framework](../../todo/02-kernel-core/TODO-26-power-management.md#15-cpu-frequency-scaling-governor-framework)).
- **S4 hibernation's write and resume path.** The on-disk image format and its codec are unit-tested, but nothing writes an image to disk or resumes from one; the boot-side half is described in [Hibernation Resume and Fast Startup Handoff](../boot/hibernation-resume-handoff.md) ([S4 Orchestration: Hibernation Image Write and Resume Path](../../todo/02-kernel-core/TODO-26-power-management.md#28-s4-orchestration-hibernation-image-write-and-resume-path)).

## How does it compare with Windows 11 and Linux?

For the pieces that ship, Impossible OS follows the shape of both operating systems: ACPI S-state and PM1 discovery, fixed-event SCI dispatch, PCI PM capability and D-state transitions, and S0ix firmware advertisement parallel Windows `ACPI.sys` and the Linux ACPI and PCI PM cores. The stop-the-world rendezvous matches the role of Windows `KeIpiGenericCall` and Linux `stop_machine()`, built ahead of the sleep code that will consume it. Everywhere else the roadmap's own comparison table records the gap: Windows and Linux both suspend to RAM, hibernate to disk, scale CPU frequency and idle depth, and offer a user-facing power-plan surface, none of which Impossible OS can do yet.

## See also

- [Power Management roadmap](../../todo/02-kernel-core/TODO-26-power-management.md)
- [Hibernation Resume and Fast Startup Handoff](../boot/hibernation-resume-handoff.md)
- [IRQL, DPCs and APCs](irql-dpc.md)
- [x86-64 Architecture Features](x86-64-architecture.md)
