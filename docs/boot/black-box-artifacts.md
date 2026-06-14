# BlackBox Diagnostic Artifacts (`X:\Diag\`)

The kernel writes structured diagnostic artifacts to the BlackBox service
partition (`X:\Diag\`) during late boot (Phase 3, after VFS + IXFS mount and
the BlackBox partition is up). They are offline-triage records: a developer or
support engineer can read them without a running registry or a live debugger.
Every writer is best-effort and degrades to a `klog` line if the partition is
unavailable; a missing file is never fatal.

Producers are wired in `src/kernel/main/boot_desktop.c` at the desktop-ready
diagnostics-dump point unless noted otherwise.

| Artifact | Producer | Contents |
|---|---|---|
| `boot-loader-identity.txt` | `boot_loader_identity_dump_to_blackbox()` (`boot_version.c`) | Healthy-boot bootloader build identity: git_sha (40-hex), build_unix_time (decimal + ISO-8601), build_label. All-zero git_sha renders `unavailable` (the ABI no-loader sentinel), never a fake commit. |
| `boot-proto-fault.txt` | `boot_version_blackbox_transcribe()` (`boot_version.c`) | Conditional: only when the prior boot halted on a boot-protocol ABI fault. Transcribes the NVRAM fault record (observed/expected magic, version, size, loader identity), then clears the NVRAM slot. |
| `boot-timeline.json` | `boot_timeline_dump_json()` (`boot_progress.c`) | Per-stage boot timeline: stage/phase/post/start_ms/duration_ms/target_ms/source(`fpdt`\|`tsc`)/unreliable. |
| `attestation.json` | `tpm_attest_report_export()` (`tpm_attest_report.c`) | TPM-rooted boot attestation report: manifest sha256, handoff provenance, SHA-256-quoted PCR bank, TPM2_Quote + AK pub + EK cert, trust-status fields. |
| `tpm-events.json` | `tpm_evlog_export_cel()` (`tpm.c`) | Parsed TCG measured-boot event log (CEL-JSON subset). |
| `firmware-tables.json` | firmware-tables writer (`firmware_tables_json.c`) | ACPI/SMBIOS firmware-table inventory + conformance profile; `generated_at_utc` from the loader build time. |
| `firmware-advisor.json` | firmware advisor (`firmware_advisor.c`) | Firmware quirk / advisory findings. |
| `boot-health.json` | `boot_health_publish_json()` (`boot_health.c`) | Per-phase perf budgets + threshold alarms (owner TODO-29). |
| `entropy.json` | `entropy_publish_json()` (`entropy_registry.c`) | Early-entropy / seed diagnostics (owner TODO-12). |
| `lvfs-metadata.json` | (not kernel-written) input cache read by `firmware_advisor_init()` -> `fa_read_cache_text()` (`firmware_advisor.c`) | Offline LVFS firmware metadata cache the advisor CONSUMES; staged onto X:\Diag out-of-band, not produced by a boot writer. |
| `boot-reserved.json` | `boot_reserved_blackbox_dump()` | Physical ranges the PMM kept reserved at boot. |
| `hwdump.txt` | `hw_dump_write_file()` | Hardware inventory (PCI / ACPI / CPU). |

> Adding a new `X:\Diag\` artifact: add its producer to the late-boot dump
> sequence in `boot_desktop.c` and a row here in the same commit.
>
> `lvfs-metadata.json` is the one row that is an INPUT, not a kernel-written
> output: the firmware advisor reads it as an offline cache. It is listed here
> so a developer browsing `X:\Diag\` knows what wrote (or, here, what reads) it.
