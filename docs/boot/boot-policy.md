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

Updates use FAT32's atomic single-cluster directory-entry rename: write the new filename via `EFI_FILE_PROTOCOL.SetInfo`-driven rename, no read-modify-write of any shared file. The worst post-crash state is the old filename remaining (counters un-decremented), which the next boot will detect as a stale try via the `tries_done` peak heuristic in the watchdog rollback path.

Counters are explicitly NOT in:

- `bootentries.json` -- that store is CRC-pinned at `schema_version=1`. Rewriting the CRC every boot would defeat the integrity check AND expand the corruption blast radius from one counter to the whole entry list.
- NVRAM -- entry-store doctrine is "NVRAM is exceptional-only" (no per-boot writes).
- A shared sidecar JSON file -- a sidecar would have the same blast-radius problem at smaller scale.

The bootloader-side glue (directory scan, `SetInfo` rename) lives in `bootx64.c`; the pure-C parse + format helpers (`boot_counter_parse_filename`, `boot_counter_format_filename`) live in [`boot_policy.c`](../../src/boot/uefi/boot_policy.c) and are unit-tested round-trip.

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

Expected: `selection_reason = BOOT_SELECTION_FALLBACK_STORE_INVALID`, `selected_entry_id = ""` (the fallback envelope's id is the canonical synth id; bootx64 copies it after the ladder returns).

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

## 6. Diagnostic surface

After the ladder runs, the bootloader writes the decision into `boot_info`:

| Field | Source | Notes |
| --- | --- | --- |
| `selected_entry_id[64]` | `boot_policy_decision_t.selected_entry_id` | NUL-terminated. Empty iff store was invalid AND fallback envelope chosen (the bootloader copies the fallback id in afterward). |
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
| `bootentries.json` ESP read in bootx64.c | tracked sub-item under `§3` (Branch B follow-up) |
| Boot#### `OptionalData` parse for `BootCurrent`-to-id mapping | tracked sub-item under `§3` (Branch B follow-up) |
| Counter directory scan + atomic rename | tracked sub-item under `§3` (Branch B follow-up) |
| `mark-good` / health-gated promotion | health gate (`§11`) |
