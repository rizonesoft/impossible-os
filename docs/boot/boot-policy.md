<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md -->
# Boot Policy Merge Order

> Companion to [`boot-entry-schema.md`](boot-entry-schema.md). The schema doc pins the on-disk format; this doc pins the precedence ladder that turns a parsed store into a single selected entry id.

## 1. Two layers, deterministically composed

Boot policy is split into two distinct layers. Most other operating systems conflate them, which is why precedence becomes opaque the moment a user, watchdog, or recovery flow gets involved.

### Firmware layer (read-only inputs)

| UEFI variable | What it means | Where it lives in `boot_info` |
| --- | --- | --- |
| `BootCurrent` | The `Boot####` index the firmware actually launched in this boot. The firmware sets this BEFORE handing control to our `BOOTX64.EFI`. | `uefi_boot_current` |
| `BootNext` | One-shot operator override. **The firmware deletes this before launching us** (UEFI 2.10 spec section 3.1.5). We observe its result via `BootCurrent`, never override or re-set it. | `uefi_boot_next`, `uefi_boot_next_valid` |
| `BootOrder` | Ordered list of `Boot####` indices the firmware tries. Read-only diagnostic. | `uefi_boot_order[16]`, `uefi_boot_order_count` |
| `Boot####.Attributes` | Per-`Boot####` attribute word (active, force-reconnect, hidden, etc.). | `boot_current_attrs` |
| `Boot####.Description` | UCS-2 description string, ASCII-truncated to 63 bytes. | `boot_description[64]` |

These fields are populated pre-handoff by the existing UEFI boot-variable read in `bootx64.c`. The boot policy module reads them through a `boot_policy_inputs_t` view; it does NOT add a new `firmware_boot_provenance` aggregate -- the fields above ARE the provenance.

### OS layer (this section)

Once the firmware has selected a `Boot####` and launched our `BOOTX64.EFI`, the OS layer picks the internal entry id from the parsed store. Inputs are the firmware-layer view above plus per-section override flags; output is one selected entry id plus a reason and a list of rejected entries.

## 2. The precedence ladder

Highest priority first. The ladder runs to completion -- a higher priority always overrides lower.

| # | Layer | Wins when | Reason code | Owning section |
| --- | --- | --- | --- | --- |
| 1 | **Hotkey** | Operator pressed F8/F9 at the menu and picked an entry index. | `BOOT_SELECTION_HOTKEY` | boot-menu (`§4`) -- input wiring |
| 2 | **Watchdog rollback** | BlackBox sticky watchdog flag asserted after a previous boot failed. | `BOOT_SELECTION_WATCHDOG_ROLLBACK` | watchdog/policy-audit (`§9`) |
| 3 | **A/B try-state** | Slot prober selected an A/B slot index. | `BOOT_SELECTION_AB_TRY_STATE` | A/B + recovery integration (`§6`, `TODO-21`) |
| 4 | **Recovery request** | Recovery request asserted by firmware or operator. | `BOOT_SELECTION_RECOVERY_REQUEST` | A/B + recovery integration (`§6`, `TODO-22`) |
| 5 | **Store default** | Default = first ACTIVE non-hidden non-skipped entry whose `machine_id` matches the local machine (or empty `machine_id`), tiebreak by `sort_key` ascending then array order. **`BootCurrent`'s `OptionalData`-resolved entry id is a SOFT HINT**: the ladder promotes it above the lowest-`sort_key` candidate when both are viable. | `BOOT_SELECTION_STORE_DEFAULT` or `BOOT_SELECTION_BOOTNEXT_HINT` (when promotion fired) | this section (`§3`) |
| 6 | **Fallback** | Either the parsed store was rejected outright (`reject_code != OK`) or the filter-and-ladder produced no viable candidate. | `BOOT_SELECTION_FALLBACK_STORE_INVALID` or `BOOT_SELECTION_FALLBACK_NO_VIABLE` | parser (`§2`) -- `boot_entries_synthesize_fallback()` |

The ladder is a pure function over `boot_policy_inputs_t` + `boot_entries_parse_result_t` + a counter array. The implementation lives in [`src/boot/uefi/boot_policy.c`](../../src/boot/uefi/boot_policy.c) and is cross-included from `src/kernel/test/test_boot_policy.c` so the tests exercise the same code-path the bootloader links.

## 3. Policy filter (pre-ladder)

Every entry from the parsed store is run through a per-entry filter. The filter does not select; it only marks entries as candidates or records rejects.

| Filter | Reject reason | Notes |
| --- | --- | --- |
| `kind_skipped` (vendor / reserved-future numeric kind) | `BOOT_REJECT_REASON_KIND_SKIPPED` | Forward-compat: the parser already kept the entry around; the policy filter just refuses to promote it. |
| `BOOT_ENTRY_FLAG_ACTIVE` not set | `BOOT_REJECT_REASON_NOT_ACTIVE` | Inactive entries can still appear in the menu (`§4`), just never auto-selected. |
| `BOOT_ENTRY_FLAG_HIDDEN` set | `BOOT_REJECT_REASON_HIDDEN` | Hidden entries are filtered from both the menu and the auto-select. |
| `machine_id` non-empty AND not equal to local SMBIOS UUID | `BOOT_REJECT_REASON_MACHINE_ID_MISMATCH` | An empty `machine_id` field on the entry means "match any machine". An empty local SMBIOS UUID means "machine has no UUID -- match only entries with empty `machine_id`". |
| Counter file shows `tries_left == 0` | `BOOT_REJECT_REASON_TRIES_EXHAUSTED` | Demoted-but-visible: still in the menu, never auto-selected. |
| `kind == chainload`, `secure_boot_active`, `trusted_chainload` flag absent | `BOOT_REJECT_REASON_PATH_ESCAPE` | Mirrors the parser's path-escape gate; restated here so the diagnostic surfaces in `rejected_entries[]`. |
| `inputs.supported_kinds_mask != 0`, `kind >= 32 OR (mask & (1 << kind)) == 0` | `BOOT_REJECT_REASON_KIND_UNAVAILABLE` | Caller-side capability gate; closed-mask. The bootloader sets a mode-locked one-bit mask: `{UKI}` when invoked via the UKI launch path, `{SPLIT}` otherwise. Cross-mode selection (kind=uki under split, kind=split under UKI) would lie about which payload was loaded since the loader is mode-locked at `detect_uki_sections()` time and cannot switch modes from a policy decision. Same-mode payload metadata is recorded as the policy decision but not yet consumed by `load_kernel` -- the per-entry-kind handlers feature owns that. Kinds whose owner section has not yet shipped (RECOVERY needs the recovery-partition load path; SAFE/TEST/DIAGNOSTICS need boot_config materialization from entry flags; INSTALLER needs a distinct installer-image load path plus offline + first-install seeding; CHAINLOAD/NETWORK/RESUME need the per-entry-kind handler table) are filtered so the ladder never selects an entry the caller cannot finish booting -- the alternative (record selection but execute the default path) would lie to the kernel about which mode actually ran. Each owner section widens the mask as it ships. `mask == 0` leaves the gate disabled (test-suite back-compat). |

Filter misses go into `boot_policy_decision_t.rejected[]` as `(id, reason)` pairs; the count is `rejected_count` and the overflow flag is `rejected_overflow` (set if more than 64 rejects observed).

## 4. Counter persistence (BLS-style filename state)

`tries_left` and `tries_done` per entry are stored as one zero-byte file each under `\EFI\ImpossibleOS\counters\`. Filename grammar:

```
<entry-id>+<L>-<D>
```

| Token | Range | Notes |
| --- | --- | --- |
| `<entry-id>` | kebab-case 1..47 chars | Same id grammar as the boot entries store. |
| `<L>` | single decimal digit 0..9 | `tries_left`. 0 means demoted (still visible, no auto-select). |
| `<D>` | one or two decimal digits 0..99 | `tries_done`. |

Updates use a CRASH-TOLERANT protocol: **write-new + Flush() + Close(success) + delete-old**. The new file is treated as durable ONLY when BOTH Flush() and Close() return EFI_SUCCESS; delete-old is SKIPPED on either failure. UEFI `EFI_FILE_PROTOCOL.SetInfo` rename is NOT power-fail-atomic on FAT32 -- LFN entries can span multiple directory entries and a reset mid-rename can leave torn names, duplicates, or orphaned old names. The protocol:

1. `Open(new_filename, CREATE | WRITE)` -> `Flush()` -> `Close()`. The replacement is treated as durably committed ONLY when BOTH `Flush()` and `Close()` return `EFI_SUCCESS`. UEFI 2.10 spec section 13.5 documents `Close()` as flushing on its own, but firmware can still fail either call; if EITHER fails, step 2 is SKIPPED and the old file is preserved (next-boot scan resolves duplicates conservatively).
2. `Open(old_filename, READ | WRITE)` -> `Delete()` (removes the stale entry; ONLY if step 1 confirmed durable).

A reset between (1) and (2) leaves both files. The next-boot scan resolves duplicates **conservatively**: when the same `<entry-id>` appears twice (torn rename), retain `min(tries_left)` AND `max(tries_done)` -- worst-case demotion. A torn rename can never silently un-demote an entry or hide an apparent exhaustion record.

**First-boot bootstrap**: when `policy_counter_decrement()` is called for an entry that has no counter file yet (chosen entry with `counter_existed=0`), it creates `<id>+2-1` directly. This matches the systemd-boot BLS 3-try semantic (3 tries default; one consumed by this boot leaves 2 left, 1 done). Without this positive-budget bootstrap, single-entry installs would brick after one boot before the health-gated mark-good feature ships. mark-good will later DELETE the counter on success, returning the entry to the "no counter, no gate" state.

A counter file with `tries_left=0` means the entry has been demoted: it stays visible in the menu but is filtered from auto-select via the ladder's `BOOT_REJECT_REASON_TRIES_EXHAUSTED` gate.

Counters are explicitly NOT in:

- `bootentries.json` -- that store is CRC-pinned at `schema_version=1`. Rewriting the CRC every boot would defeat the integrity check AND expand the corruption blast radius from one counter to the whole entry list.
- NVRAM -- entry-store doctrine is "NVRAM is exceptional-only" (no per-boot writes).
- A shared sidecar JSON file -- a sidecar would have the same blast-radius problem at smaller scale.

The bootloader-side glue lives in `bootx64.c` (`policy_scan_counters()` for the directory walk + conservative duplicate dedupe; `policy_counter_decrement()` for the write-new + Flush() + Close(success) + delete-old protocol). The pure-C parse + format helpers (`boot_counter_parse_filename`, `boot_counter_format_filename`) live in [`boot_policy.c`](../../src/boot/uefi/boot_policy.c) and are unit-tested round-trip.

## 5. Worked examples

Each scenario lists inputs, the ladder evaluation, and the expected `selection_reason`. All assume a parsed store with `reject_code == BOOT_ENTRIES_OK` unless noted.

### 5.1 BootCurrent unmapped, store has two active entries

| Input | Value |
| --- | --- |
| `bootcurrent_known` | 1 |
| `bootcurrent_entry_id` | `""` (Boot####.OptionalData lacked the IPOS\x01 tag) |
| Active candidates after filter | `default-os` (sort_key 10), `secondary-os` (sort_key 20) |

Ladder: hotkey/watchdog/AB/recovery all empty -> store default. The BootCurrent hint did not resolve to an id, so the lowest-`sort_key` candidate wins.

Expected: `selection_reason = BOOT_SELECTION_UNKNOWN_BOOTCURRENT`, `selected_entry_id = "default-os"`. The selection still picks the right entry; the diagnostic captures that the firmware-side mapping was not present.

### 5.2 Watchdog overrides A/B

| Input | Value |
| --- | --- |
| `watchdog_rollback` | 1 |
| `ab_slot_index` | 0 (slot prober suggested A) |
| Counters | `slot-a+1-3`, `slot-b+1-1` |
| Active candidates | `slot-a` (`tries_done=3`), `slot-b` (`tries_done=1`) |

Ladder: watchdog (priority 2) fires before A/B (priority 3). Watchdog identifies `slot-a` as the peak-attempted entry, picks the next viable by `sort_key` (`slot-b`).

Expected: `selection_reason = BOOT_SELECTION_WATCHDOG_ROLLBACK`, `selected_entry_id = "slot-b"`. The A/B prober's choice is overridden, recorded in the audit log via `selection_reason`.

### 5.3 A/B fail forces recovery

| Input | Value |
| --- | --- |
| `ab_slot_index` | 0xFFFF (no A/B applies -- both slots exhausted, prober returns "no slot") |
| `recovery_requested` | 1 |
| Active candidates | `recovery` (`kind=recovery`) |

Ladder: watchdog/A/B miss; recovery (priority 4) fires. Picks the first `kind=recovery` candidate by `sort_key`.

Expected: `selection_reason = BOOT_SELECTION_RECOVERY_REQUEST`, `selected_entry_id = "recovery"`. The bootloader updates `boot_path = BOOT_PATH_RECOVERY` and `boot_reason = BOOT_REASON_RECOVERY_TRIGGER` because the chosen entry implies a path change.

### 5.4 Store invalid -> fallback synth

| Input | Value |
| --- | --- |
| Parser result | `reject_code = BOOT_ENTRIES_REJECT_CRC_MISMATCH` |

Ladder: short-circuit at the top. The caller (`bootx64.c`) calls `boot_entries_synthesize_fallback()` to build a single-entry envelope and proceeds with that.

Expected: `selection_reason = BOOT_SELECTION_FALLBACK_STORE_INVALID`, `selected_entry_id = ""` (the documented v19 ABI sentinel for STORE_INVALID -- per `include/kernel/boot_info.h` "selected_entry_id is empty in that case"). The bootloader still synthesizes the fallback envelope via `boot_entries_synthesize_fallback()` to capture the kind for the post-EBS path-changing override and to drive `load_kernel`, but the kind capture and the load-time identifier are separate concerns from the v19 policy decision id.

### 5.5 Hotkey overrides everything

| Input | Value |
| --- | --- |
| `hotkey_override_index` | 2 |
| Active candidates | `default-os`, `recovery`, `safe-mode` |

Ladder: hotkey (priority 1) fires first. Picks the 2nd visible candidate (1-based index post-filter): `recovery`.

Expected: `selection_reason = BOOT_SELECTION_HOTKEY`, `selected_entry_id = "recovery"`. The `boot_path` update logic stays the same as case 5.3 (the chosen entry is a recovery kind).

### 5.6 Path escape under Secure Boot

| Input | Value |
| --- | --- |
| `secure_boot_active` | 1 |
| Single active entry | `windows-on-other-disk` (`kind=chainload`, no `trusted_chainload` flag) |

Ladder: filter drops the only candidate with `BOOT_REJECT_REASON_PATH_ESCAPE`. No viable candidate remains; ladder returns fallback-no-viable.

Expected: `selection_reason = BOOT_SELECTION_FALLBACK_NO_VIABLE`, the bootloader synthesizes a fallback envelope. The audit log shows the rejected entry with `PATH_ESCAPE` so the operator sees why the chainload was suppressed.

## 5.7 Path-changing kind -> `boot_path` / `boot_reason` mapping

When the policy ladder picks an entry whose `kind` implies a non-normal flow, the bootloader's POST-EBS boot-decision populate block in `bootx64.c` overrides `boot_info.boot_path` + `boot_info.boot_reason` after the media-role switch. The mapping must satisfy the kernel-side `boot_decision_validate()` table in `src/kernel/main/boot_decision.c` (every reason has its required path + trigger flag).

| Selected entry kind | `boot_info.boot_path` | `boot_info.boot_reason` | `boot_info.boot_source_flags` add | Notes |
| --- | --- | --- | --- | --- |
| `BOOT_ENTRY_KIND_RECOVERY` | `BOOT_PATH_RECOVERY` | `BOOT_REASON_RECOVERY_TRIGGER` | `BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED` | Validator at `boot_decision.c` requires the trigger flag whenever reason = `RECOVERY_TRIGGER`; missing flag is BOOT_FATAL. |
| `BOOT_ENTRY_KIND_DIAGNOSTICS` | `BOOT_PATH_DIAGNOSTIC` | `BOOT_REASON_DIAGNOSTIC_REQUEST` | -- | No flag required. |
| `BOOT_ENTRY_KIND_NETWORK` | `BOOT_PATH_NETWORK` | `BOOT_REASON_USER_SELECTED` | -- | `NETWORK_INSECURE` is the post-validation outcome owned by the per-kind handlers feature; until it ships the ladder records "user/policy picked the network entry" without making a security claim. |
| `BOOT_ENTRY_KIND_RESUME` | `BOOT_PATH_RESUME` | `BOOT_REASON_USER_SELECTED` | -- | `RESUME_VALIDATED` / `RESUME_INVALIDATED` are owned by the resume-validator feature; the ladder records intent only. |
| Any other kind (split / uki / installer / safe / test) | unchanged | unchanged | unchanged | Media-role override (if any) wins; no kind-driven override. |

The override runs AFTER the media-role switch so a `kind=recovery` policy decision wins over a NORMAL media role, but a `media_role=installer` cold boot still records `BOOT_PATH_INSTALLER` if no path-changing kind was picked (the kind table above only fires for the four path-changing kinds). When both fire (e.g. media role = recovery AND policy ladder picked `kind=recovery`), the values agree and the order does not matter.

## 6. Diagnostic surface

After the ladder runs, the bootloader writes the decision into `boot_info`:

| Field | Source | Notes |
| --- | --- | --- |
| `selected_entry_id[64]` | `boot_policy_decision_t.selected_entry_id` | NUL-terminated. Empty for `FALLBACK_STORE_INVALID` -- the documented v19 ABI sentinel for an unusable parsed store. The bootloader still synthesizes a fallback envelope via `boot_entries_synthesize_fallback()` for kind capture and load-time identifier, but the v19 selected_entry_id stays empty. |
| `selection_reason` | `boot_policy_decision_t.reason` | `enum boot_selection_reason` -- 9 distinct values plus the `BOOT_SELECTION_UNSET` sentinel. |
| `rejected_entries[64]` + `rejected_entry_count` | `boot_policy_decision_t.rejected[]` | (id, reason) pairs. Cap = `BOOT_ENTRIES_MAX_ENTRIES`; overflow surfaces via `rejected_entry_overflow`. |

The kernel-side consumers are:

- Registry (`HKLM\SYSTEM\Boot\Selection\*`, `HKLM\SYSTEM\Boot\Rejected\*`) -- wired in the loader-variables feature (`§12`).
- Policy audit (per-boot history journal) -- wired in the policy-audit feature (`§9`).
- BlackBox crash log -- already records `boot_info` snapshot at panic, so the §3 fields surface in the panic dump for free.

## 7. What this section does NOT own

| Concern | Owner |
| --- | --- |
| Hotkey input capture (F8/F9 at menu) | boot-menu (`§4`) |
| Watchdog flag persistence + read | watchdog/policy-audit (`§9`) |
| A/B slot probe | A/B + recovery integration (`§6`), `TODO-21` |
| Recovery-request signal | A/B + recovery integration (`§6`), `TODO-22` |
| Per-kind payload validation | per-kind validators (`§10`) |
| `bootentries.json` ESP read in bootx64.c | ESP store read + policy wiring feature (shipped) |
| Boot#### `OptionalData` parse for `BootCurrent`-to-id mapping | ESP store read + policy wiring feature (shipped) |
| Counter directory scan + crash-tolerant decrement (write-new + Flush + Close + delete-old) | crash-tolerant counter protocol feature |
| SMBIOS table 1 UUID extraction for `local_machine_id` | OS-visible loader UEFI variables feature |
| `mark-good` / health-gated promotion | health gate (`§11`) |

> Menu UX (indicator legend, hotkey table, `hide_when_alone` semantics, F10 firmware-setup narrow path) is documented in [boot-menu.md](boot-menu.md).
