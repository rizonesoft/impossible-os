---
schema_version: 1
id: usb-hid-keyboard-mouse
domain: 01-boot-platform
status: active
title: "TODO-18 -- USB HID Boot-Protocol Keyboard & Mouse"
---

# TODO-18 -- USB HID Boot-Protocol Keyboard & Mouse

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** USB keyboards and mice work during boot and at the desktop on systems without PS/2 hardware. Most modern laptops and desktops (post-2015) have only USB input. Without this, the OS is unusable on real hardware. This TODO implements USB HID boot-protocol drivers for keyboard and mouse, interrupt endpoint polling, and input source coexistence with existing PS/2 drivers. The result: type commands, move the cursor, and click on any system with USB input.

> [!IMPORTANT]
> **Current state:** USB HID was split out of `TODO-17-xhci-usb-boot.md` into this file; no HID code exists yet. PS/2 keyboard and mouse work on hardware that has i8042 (detected via ACPI FADT). Modern laptops without i8042 show `"PS/2 keyboard: skipped (no i8042 in FADT)"` and have zero input. xHCI interrupt endpoint setup, HID report parsing, and input routing are all unimplemented.

---

## Inputs

- `src/kernel/drivers/xhci.c` -- xHCI controller (needs interrupt endpoint support)
- `src/kernel/drivers/xhci_ring.c` -- TRB ring management
- `src/kernel/drivers/keyboard.c` -- PS/2 keyboard driver (input sink interface)
- `src/kernel/drivers/mouse.c` -- PS/2 mouse driver (input sink interface)
- `include/kernel/drivers/keyboard.h` -- keyboard API (`keyboard_trygetchar()`)
- → XREF: `TODO-17-xhci-usb-boot.md` §5–§2 -- xHCI/USB MSC/boot handover foundation for this TODO
- → XREF: `04-drivers-hardware/TODO-11-input-system.md` -- unified input system (downstream consumer)

---

## Outcome

- USB keyboards produce keystrokes readable via `keyboard_trygetchar()` -- shell works.
- USB mice produce movement/button events readable by the compositor.
- Both coexist with PS/2 input -- whatever hardware is present works.
- Boot-protocol mode (no HID report descriptor parsing needed for basic operation).
- Works on QEMU with `-device usb-kbd` and on bare metal USB keyboards.

---

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI interrupt endpoint setup            | --         |  [x]   |
| 💎  |   2   | Interrupt transfer polling (periodic TRBs) | §1         |  [x]   |
| 💎  |   3   | USB HID boot-protocol keyboard driver    | §2         |  [x]   |
| 💎  |   4   | USB HID boot-protocol mouse driver       | §2         |  [x]   |
| 💎  |   5   | Input source coexistence (PS/2 + USB)    | §3, §4     |  [x]   |
| ⭐  |   6   | Hot-plug keyboard/mouse detection        | §5         |  [/]   |
| ⭐  |   7   | USB input diagnostic logging             | §1-§5      |  [x]   |

> 💎 = parity -- Windows HID minidriver and Linux usbhid both provide boot-protocol keyboard/mouse.
> ⭐ = exclusive -- hot-plug keyboard detection and diagnostic logging.

---

## 1. xHCI Interrupt Endpoint Setup

Add interrupt endpoint support to the xHCI driver (currently only bulk endpoints for MSC).

- [x] `xhci_hid_identify()` (`xhci_dev.c`) walks the config descriptor after SET_CONFIGURATION for the HID interface + its Interrupt-IN endpoint; wired into `xhci_enumerate_device` (probed when not MSC)
- [x] HID interface match: class 0x03 / subclass 0x01 (boot) / protocol 0x01 keyboard | 0x02 mouse (`USB_CLASS_HID`/`USB_SUBCLASS_HID_BOOT`/`USB_PROTO_HID_*`); untrusted EP fields validated (reject EP0, mask wMaxPacketSize bits 10:0, cap 64)
- [x] Configure Interrupt-IN endpoint: `ep0_ring_init` Transfer Ring + `xhci_hid_interval_encode()` (speed-correct EP-ctx Interval per xHCI 6.2.3.6: HS/SS=bInterval-1, FS/LS=floor(log2(bInterval))+3)
- [x] Endpoint added to slot context via Configure Endpoint command (TRB type 12, EP type `XHCI_EP_TYPE_INTERRUPT_IN`); ring freed on failure (`ep0_ring_free`)
- [x] Log: `usb-hid: HID <keyboard|mouse> found on port %u (EP%u, interval field=%u, MaxPkt=%u)` -- validated live (QEMU usb-kbd/usb-mouse)
- [x] Commit: `"drivers: xHCI interrupt endpoint setup for USB HID devices"`

**Test checkpoint:** QEMU with `-device usb-kbd` -- serial shows `"HID keyboard found on port X"`. No input yet.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` interval-encoding (9 asserts); HID enumeration validated live via QEMU usb-kbd/usb-mouse.
> **Notes:**
> - Shipped: `xhci_hid_identify()` + `xhci_hid_interval_encode()` (`xhci_dev.c`) -- config-descriptor walk for a HID boot interface + Interrupt-IN endpoint, Configure Endpoint with a speed-correct EP-ctx Interval.
> - Integrates: probed in `xhci_enumerate_device` when not MSC; new `struct xhci_device` HID fields + `XHCI_EP_TYPE_INTERRUPT_IN`; `ep0_ring_free` reclaims the ring on a definite Configure-Endpoint failure (quarantined on timeout).
> - Hardening: untrusted descriptor fields validated -- reject EP0, high-bandwidth wMaxPacketSize bits, zero/oversized packet size (avoids EP0-context overwrite + DMA-visible UAF).
> - Review: Codex 6x (design + adversarial x2 + re-adversarial + consistency + perf); 4H+3M fixed (incl Max ESIT Payload for bare-metal + MSC ring-cleanup consistency), 1H+1M accepted; live QEMU kbd+mouse enumerate (interval field=6).
> - Scope boundary: §1 configures the interrupt endpoint; periodic report polling, keyboard/mouse report parsing, and PS/2 coexistence are later sections.
> **Verified:** 2026-06-15 | review commit | 6/6 items | build OK | QEMU usb-kbd/usb-mouse PASS (kbd port5 + mouse port6, boot 1.63s); smoke PASS 2.54s; storage 13/13 PASS
> **Accepted:** [H] `xhci_enumerate_device` (-> `xhci_hid_identify`) callable from the hot-plug ISR (alloc + 500ms busy-wait, unlocked `devices[]`/rings) -> XREF: 04-drivers-hardware/TODO-10 §8 (item: "Event-ring ownership: ISR only acks + records the port-change, defers enumeration to a serialized worker")
> **Accepted:** [M] composite MSC+HID device initializes as storage only (HID iface unconfigured) -- deliberate: boot keyboards/mice are HID-only; documented in `xhci_enumerate_device`
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) | 4H+3M fixed, 1H+1M accepted | scope: kernel-code-quality

---

## 2. Interrupt Transfer Polling

Set up periodic interrupt transfers to receive HID reports from keyboard/mouse.

> [!NOTE]
> **Design review (2026-06-15) revised this section -- the original plan was a no-ship.** Two blockers: (1) `xhci_event_poll` (`xhci_ring.c`) *destructively* drains the SHARED interrupter-0 event ring -- a HID poller looping for its events would acknowledge command / MSC-transfer / hot-plug completions before their owners see them (the same shared-ring hazard the hot-plug ISR carries); (2) `timer_register_tick_callback` (`timer.h`) is a SINGLE global slot the boot-splash spinner owns until `spinner_stop()` (`spinner.c`), so HID polling registered during boot would overwrite the spinner, and registered after would lose boot-window input. Revised plan: a dedicated HID interrupter (own event ring) + a BSP tick multiplexer. The shared-ring fix overlaps the deferred event-ring-ownership work -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md §8`.

- [x] Dedicated HID interrupter (`xhci_ring.c` `hid_event_ring_init` + `xhci_hid_event_poll`) -- interrupter 1 own Event Ring + ERST, gated on `max_intrs >= 2`; HID Transfer Events never touch the shared interrupter-0 ring
- [x] Target HID Interrupt-IN TRBs at interrupter 1 via the Normal-TRB Interrupter Target field (`status | (1<<22)`) so completions land on the dedicated ring
- [x] BSP tick multiplexer (`timer.c` `timer_add/remove_tick_subscriber` + `tick_subs[4]`) -- additive to the singleton so the splash spinner + HID poll coexist; BSP-only writers, release/acquire ISR snapshot, status-returning remove
- [x] Per-HID-device report DMA buffer (`pmm_alloc_contiguous`); `xhci_hid_queue_report` queues a Normal TRB on `dev->int_in_ring` + rings the EP doorbell (allocated in `xhci_hid_identify`)
- [x] HID poll (`xhci_hid_poll`, ~10 ms via `timer_add_tick_subscriber`): drains the dedicated HID ring (bounded budget), delivers each report to `dev->int_in_report` (ISR-safe), re-queues -- validated live (QEMU usb-kbd usage codes)
- [x] Commit: `"drivers: xHCI dedicated HID interrupter + tick-mux interrupt polling"` **Verified 2026-07-31:** done as `1d947b72` "drivers: xHCI dedicated HID interrupter + tick-mux interrupt polling (TODO-18 section 2)"

**Test checkpoint:** Press keys on USB keyboard in QEMU -- Transfer Event TRBs arrive on the dedicated HID interrupter and each report lands in `dev->int_in_report` (no per-tick serial log -- the poll runs in ISR context); the boot-splash spinner keeps animating (tick mux); no `usb`/`nvme`/`ahci` command timeouts (shared ring untouched).
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` interval-encoding (pure); report polling is live-MMIO/ISR -- validated via QEMU usb-kbd + QMP send-key.
> **Notes:**
> - Shipped: dedicated HID interrupter (`xhci_ring.c` IR1 own Event Ring + `xhci_hid_event_poll`) + report polling (`xhci_dev.c` `xhci_hid_queue_report` + `xhci_hid_poll`) + per-device report DMA buffer; tick mux in `timer.c` (`448d78d2`).
> - Integrates: HID Normal TRBs carry Interrupter Target=1 so completions land on IR1, never the shared ring; poller armed after the device is published active; coexists with the splash spinner via the tick subscriber list.
> - Hardening: IR1 poll-only (no `IMAN.IE` -> no unacked interrupt storm); completion-code-gated re-queue (stops on halt/error/removal); events matched by owning controller + slot.
> - Review: Codex 6x (design no-ship redirect + adversarial x2 + re-adversarial + consistency + perf); 4H+4M fixed (incl ISR-context perf: no per-report serial log, per-tick event budget); validated live (QEMU usb-kbd usage codes on IR1, boot 1.7s).
> - Scope boundary: §2 delivers raw report bytes; report PARSING is §3/§4; the IRQ-driven model + hot-unplug slot teardown are owned by `04-drivers-hardware/TODO-10-usb-stack.md §8`.
> **Verified:** 2026-06-15 | review commit | 6/6 items | build OK | QEMU usb-kbd IR1 setup + report delivery (boot 1.7s); smoke PASS; storage 13/13
> **Accepted:** [H] `xhci_hid_poll` runs in BSP-tick-ISR context + `xhci_enumerate_device` reachable from the hot-plug ISR share device/ring state -> XREF: 04-drivers-hardware/TODO-10 §8 (item: "Event-ring ownership: ISR only acks + records the port-change, defers enumeration to a serialized worker")
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) | 4H+4M fixed, 1H accepted | scope: kernel-code-quality

---

## 3. USB HID Boot-Protocol Keyboard Driver

Parse boot-protocol keyboard reports (8 bytes) into keystrokes.

- [x] Boot report parsed in `xhci_hid_parse_keyboard` (`xhci_dev.c`): `[modifier, reserved, key1..key6]`, new-key diff vs `dev->hid_prev_report`, rollover (0x01-0x03) ignored
- [x] Modifier byte handled in `keyboard_inject_hid_key` (`keyboard.c`): L/R Shift (0x02/0x20) + L/R Ctrl (0x01/0x10), independent of the PS/2 latch
- [x] Key bytes = USB HID usage IDs; `hid_normal`/`hid_shifted[0x40]` tables cover the printable boot range (0x04-0x38) + Enter/Esc/BS/Tab/Space
- [x] HID usage -> ASCII via the dedicated `hid_normal`/`hid_shifted` tables (own modifier handling, not the PS/2 `shift_held` latch); Caps Lock (0x39) toggles case
- [x] Feeds the shared input path (`keyboard_inject_hid_key` -> `terminal_key_input` / `kb_buffer_push`, the SMP-safe ring) so input reaches the shell
- [x] `keyboard_trygetchar()` is source-blind -- USB + PS/2 both land in the same buffer/terminal ring (now irqsave-locked)
- [x] SET_PROTOCOL(0) issued in `xhci_hid_identify` to force boot protocol; keyboard failure logged
- [x] SET_IDLE(0) issued so the device reports only on change (no duplicate held-key reports)
- [x] Commit: `"drivers: USB HID boot-protocol keyboard -- type in shell via USB keyboard"`

**Test checkpoint:** QEMU with `-device usb-kbd` -- type `dir` at the `C:\>` prompt, output appears. Also test on bare metal USB keyboard.

**Regression risk:** LOW -- additive driver. PS/2 keyboard continues to work. If USB keyboard produces garbage, disconnect and use PS/2.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` keyboard usage->ASCII (9 asserts: shift/caps/rollover); live typing via QEMU usb-kbd.
> **Notes:**
> - Shipped: HID boot-keyboard parsing -- `keyboard.c` `hid_normal`/`hid_shifted` tables + `keyboard_inject_hid_key` (own modifier handling); `xhci_dev.c` `xhci_hid_parse_keyboard` (new-key diff, rollover-skip) wired into the §2 poll.
> - Integrates: routes to the shared `terminal_key_input`/`kb_buffer` path so `keyboard_trygetchar()` is source-blind (USB + PS/2); SET_PROTOCOL(0)/SET_IDLE(0) force boot protocol; report buffer zeroed per transfer.
> - Hardening: the terminal input ring is now SMP-safe (irqsave spinlock -- fixes a pre-existing PS/2+USB ISR-vs-thread race); keyboard EP MaxPkt < 8 rejected; Caps Lock (0x39) toggles case.
> - Review: Codex 5x (design redirect + adversarial x2 + consistency + perf); 2H+3M fixed (terminal-init lock ordering, poller-register-before-queue) + 3 design hazards adopted pre-code; 1M rejected (capslock RMW is BSP-serialized); validated live (typed `dir` at C:\> -> executed).
> - Scope boundary: §3 owns keyboard parsing; mouse parsing is §4, PS/2+USB coexistence is §5, non-boot report-protocol descriptor parsing is out of scope.
> **Verified:** 2026-06-15 | review commit | 9/9 items | build OK | QEMU usb-kbd typed `dir` -> executed; smoke PASS 2.5s; storage 22/22
> **Quality reviewed:** 2026-06-15 | Codex 5x (design, adversarial x2, consistency, perf) | 2H+3M fixed, 1M rejected | scope: kernel-code-quality + desktop-code-quality

---

## 4. USB HID Boot-Protocol Mouse Driver

Parse boot-protocol mouse reports (3–4 bytes) into cursor movement.

- [x] Boot mouse report decoded in `xhci_hid_decode_mouse` (`xhci_dev.c`, pure): `[buttons, dx, dy]`; a 4-byte `[..., wheel]` report parses the same first 3 bytes (wheel/trailing ignored -- scroll is report-protocol, not boot)
- [x] buttons = `report[0] & 0x07` (bit0=left/bit1=right/bit2=middle), 1:1 with `MOUSE_BTN_LEFT/RIGHT/MIDDLE`
- [x] dx/dy = signed 8-bit (`(int8_t)report[1]`/`report[2]`) relative movement
- [x] `mouse_update_relative(dx,dy,buttons)` (`mouse.c`) applies the delta to the shared cursor state under a new irqsave `mouse_lock`, clamped to fb; USB HID +Y is screen-down (dy applied directly). `xhci_hid_parse_mouse` wires it into the §2 poll
- [x] Compositor reads the shared cursor via `mouse_get_state` -- transparent on the relative-source path (§4 `-device usb-mouse`); an absolute source (VirtIO/VBox) overrides USB via its cascade, merged in §5
- [x] SET_PROTOCOL(0) issued for the mouse in `xhci_hid_identify`; a NAK now warns for mice too. Boot mouse EP `MaxPkt < 3` rejected; poll gates parse on the Transfer-Event actual length (`>= 3`)
- [x] Commit: `"drivers: USB HID boot-protocol mouse -- cursor movement via USB mouse"`

**Test checkpoint:** QEMU with `-device usb-mouse` -- mouse cursor moves on desktop. Click works on window title bars.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` mouse report decode (5 reports: spec case, button mask, +/-127/-128 boundaries, leftward signed dx, 4-byte wheel-ignored); cursor movement validated live via QEMU usb-mouse.
> **Notes:**
> - Shipped: HID boot-mouse parsing -- `xhci_dev.c` `xhci_hid_decode_mouse` (pure) + `xhci_hid_parse_mouse` wired into the §2 poll; `mouse.c` `mouse_update_relative` applies the delta to the shared PS/2 cursor state.
> - Integrates: USB writes the same `mouse_x/y/buttons` the compositor already reads via `mouse_get_state`, so cursor movement is transparent on the relative-source path; SET_PROTOCOL(0) forces boot protocol; mouse `MaxPkt < 3` rejected.
> - Hardening: a new irqsave `mouse_lock` guards every cursor reader/writer (PS/2 IRQ, USB tick-ISR, compositor) -- fixes a pre-existing unlocked RMW race; poll skips sub-boot-length reports; SET_PROTOCOL NAK warns for mice too.
> - Review: Codex 10x (design, test-coverage, adversarial x2, consistency, perf, re-adversarial x4); 1H+5M fixed across impl+review (boot-protocol gating, short-report length, cursor-state lock + seed ordering), 2H accepted (compositor merge + cursor-router -> §5), 2 test gaps added.
> - Scope boundary: §4 drives the cursor on the relative-source path; merging USB with an absolute VirtIO/VBox source, and PS/2+USB buffer coexistence, are §5; hot-plug is §6.
> **Verified:** 2026-06-15 | review commit | 6/6 items | build OK | storage 37 kernel + 16 user-mode PASS; smoke PASS 2.47s
> **Quality reviewed:** 2026-06-15 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 1H+3M fixed, 2H accepted (both closed by §5 source-merge) | scope: kernel-code-quality

---

## 5. Input Source Coexistence

Both PS/2 and USB input should work simultaneously without conflicts.

- [x] Keyboard already source-merged at ONE ring (§3): PS/2 IRQ + USB `keyboard_inject_hid_key` both push to `kb_buffer`/terminal, so `keyboard_trygetchar` is source-blind -- no separate PS/2/USB buffers to order
- [x] Mouse events merged: `compositor.c` feeds the active absolute source into `mouse_merge_absolute` (VirtIO) / `mouse_merge_absolute_position` (VBox) then reads `mouse_get_state` ONCE -- USB/PS2 relative deltas survive alongside an absolute source
- [x] Per-source-class button state OR-published under `mouse_lock` (`s_ps2`/`s_usb`/`s_abs`): a report from one class cannot release a button held on another; PS/2 overflow still publishes buttons; VBox position-only (per-USB-device split is §6)
- [x] PS/2 absent (no i8042): `mouse_init`/keyboard init gate on `acpi_hw_reduced` + port-0x64 probe -- no I/O to missing hardware; only USB input active
- [x] USB HID absent: HID poll gated on `hid_intr_active`; PS/2 path unchanged
- [x] VirtIO tablet continues to work: it is the button-bearing absolute source in the merge; cursor + clicks preserved
- [x] Commit: `"drivers: input source coexistence -- PS/2, USB, VirtIO work together"`

**Test checkpoint:** QEMU with VirtIO tablet + USB keyboard -- both work simultaneously. Bare metal with PS/2 keyboard + USB mouse -- both work.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` input-source coexistence merge (edge-trigger, per-source button OR, VBox feedback guard, PS/2+USB cross-device hold); multi-source validated live via QEMU VirtIO tablet + USB mouse.
> **Notes:**
> - Shipped: cursor source-merge -- `mouse.c` `mouse_merge_absolute` (VirtIO, edge-trigger + OR buttons) + `mouse_merge_absolute_position` (VBox, pos-only) + per-source button slots; `compositor.c` reads one merged cursor.
> - Integrates: keyboard was already merged at a single `kb_buffer` (§3); the mouse merge closes the §4 accepted compositor-cascade gap so USB relative motion + clicks survive alongside an absolute VirtIO/VBox source.
> - Hardening: per-source button OR-merge stops cross-device button-release; VBox treated position-only (its buttons echo PS/2) so a held button cannot latch into the absolute slot.
> - Review: Codex 7x (design, test-coverage, adversarial x2, consistency, perf, re-adversarial); H+M fixed across impl+review (per-source buttons, VBox button-latch x2, PS/2 overflow-release, test gaps), 1M accepted (per-USB-device slots -> §6).
> - Scope boundary: §5 merges PS/2/USB/VirtIO/VBox into one cursor + buffer; hot-plug of new devices is §6; input diagnostics is §7.
> **Verified:** 2026-06-15 | review commit | 6/6 items | build OK | storage 55 kernel + 16 user-mode PASS; smoke PASS 2.52s
> **Accepted:** [M] single USB-class button slot lets two simultaneous USB mice cross-release a held button (reason: needs per-device infra) -> XREF: 01-boot-platform/TODO-18 §6 (item: "Per-USB-device button slots in `mouse.c` ... so two USB mice don't cross-release a held button")
> **Quality reviewed:** 2026-06-15 | Codex 7x (design, test-coverage, adversarial x2, consistency, perf, re-adversarial) | 4H+2M fixed, 1M accepted | scope: kernel-code-quality + desktop-code-quality

---

## 6. Hot-Plug Keyboard/Mouse Detection

Detect USB keyboard/mouse plugged in after boot.

> [!WARNING]
> **Blocked on `04-drivers-hardware/TODO-10 §8` (Hot-Plug Interrupt Handling).** The "port-change event -> enumerate new device" core requires the serialized-worker enumeration that TODO-10 §8 owns (item: "Event-ring ownership: ISR only acks + records the port-change, defers enumeration to a serialized worker"). TODO-18 §1/§2 accepted exactly this ISR-context-enumeration hazard to TODO-10 §8; doing hot-plug enumeration here before §8 ships would re-introduce the in-ISR-enumerate bare-metal danger. Implement after TODO-10 §8. The per-USB-device button-slots item below is independent and can ship earlier.

- [ ] xHCI Port Status Change Events (already in Event Ring) trigger port scan
- [ ] New device on port: enumerate → if HID, configure interrupt endpoint → start polling
- [ ] Device removed: stop polling, clean up endpoint ring
- [ ] Per-USB-device button slots in `mouse.c` (aggregate USB buttons per device via a source id threaded through `mouse_update_relative`) so two USB mice don't cross-release a held button -- today `s_usb` is one shared USB-class slot (-> XREF: §5)
- [ ] Log: `"[USB] Hot-plug: %s on port %u"` / `"[USB] Removed: port %u"`
- [ ] Commit: `"drivers: USB HID hot-plug detection -- keyboard/mouse plug-and-play"`

**Test checkpoint:** Boot without USB keyboard. Plug in USB keyboard after desktop appears -- typing works within 1 second.
> **Test runner:** N/A (deferred -- blocked on the usb-stack hot-plug-interrupt section) | validation: deferred with the implementation.
> **Notes:**
> - Deferred (no code shipped): the port-change-to-enumerate core needs the serialized-worker enumeration owned by the usb-stack hot-plug-interrupt section (still open); in-ISR enumeration here would re-introduce the accepted bare-metal hazard.
> - Independent sub-item: per-USB-device button slots (`mouse.c`, the §5 accept target) does not depend on the hot-plug worker and can ship earlier.
> - Scope boundary: §6 owns HID-specific hot-plug attach/detach; the event-ring ownership + serialized-worker enumeration is the usb-stack hot-plug-interrupt section.
> **Verified:** 2026-06-15 | deferred | 0/5 items | build OK | no code shipped -- blocked, tracked
> **Deferred:** [M] hot-plug enumerate/teardown blocked on serialized-worker event-ring ownership (reason: infra in another domain) -> XREF: 04-drivers-hardware/TODO-10 §8 (item: "Event-ring ownership: ISR only acks + records the port-change, defers enumeration to a serialized worker" at line 196)

---

## 7. USB Input Diagnostic Logging

Comprehensive USB input status in serial log.

- [x] Boot summary `[INPUT] Sources: PS/2/USB_KBD/USB_MOUSE/VIRTIO=yes/no` via `usb_input_diag_report()` (`usb_input_diag.c`); PS/2 via new `keyboard_is_present()`/`mouse_is_present()`, USB via HID scan, VIRTIO via `virtio_input_available()`
- [x] Per-USB-HID device line: vendor/product ID + protocol + EP interval + port + maxpkt, from `xhci_device` fields, one line per enumerated HID device
- [x] HID-poll error stats: per-controller `hid_ep_errors`/`hid_requeue_fails`/`hid_reports` counters incremented in `xhci_hid_poll`, aggregated + emitted as `HID poll: reports/ep_errors/requeue_fails`
- [x] Commit: `"drivers: USB input diagnostic logging"`

**Test checkpoint:** Serial output shows input source summary at boot.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_hid.c` input-diag formatter (renders, exact length, all-no/all-yes, cap=1/0/negative + exact-fit truncation); the live `[INPUT] Sources:` + `HID poll:` lines confirmed in the smoke log.
> **Notes:**
> - Shipped: `usb_input_diag.c` (+ header) `usb_input_diag_report()` -- boot source summary + per-USB-HID identity + aggregate HID-poll error stats; pure `usb_input_diag_format()` for the source line.
> - Integrates: called from `boot_storage.c` after all input init; new `keyboard_is_present()`/`mouse_is_present()` flags + `xhci_controller` HID poll counters incremented in `xhci_hid_poll`.
> - Validated live: smoke boot emits `[INPUT] Sources: PS/2=yes USB_KBD=no USB_MOUSE=no VIRTIO=no` + `HID poll: reports=0 ep_errors=0 requeue_fails=0`.
> - Parity: exclusive (Win/Linux expose this only via Device Manager / dmesg, not a structured boot summary).
> - Scope boundary: §7 is diagnostic-only (no input behavior); hot-plug device add/remove is §6 (blocked on the usb-stack hot-plug-interrupt section).
> **Verified:** 2026-06-15 | review commit | 3/3 items | build OK | storage 71 kernel + 16 user-mode PASS; smoke PASS 2.51s ([INPUT] line in log)
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, test-coverage, adversarial x2, consistency, perf) | 2M fixed, 1M rejected | scope: kernel-code-quality (re-adversarial skipped: surgical legacy-init + comment fixes)

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11               | 🐧 Linux             | 🚀 Impossible OS              |
| --- | ------------------------ | ---------------------- | -------------------- | ----------------------------- |
| 💎  | USB HID interrupt EP     | ✅ usbxhci.sys         | ✅ xhci-hcd          | ✅ §1 Configure Endpoint      |
| 💎  | USB keyboard in boot     | ✅ HID minidriver      | ✅ usbhid + usbkbd   | ✅ §3 boot-proto parse        |
| 💎  | USB mouse in boot        | ✅ HID minidriver      | ✅ usbhid + usbmouse | ✅ §4 boot-proto cursor       |
| 💎  | PS/2 + USB coexistence   | ✅ Automatic           | ✅ Automatic         | ✅ §5 one merged cursor       |
| 💎  | USB HID hot-plug         | ✅ PnP Manager         | ✅ udev + usbhid     | ⬜ §6                         |
| ⭐  | Input source diagnostics | ❌ Device Manager only | ❌ dmesg only        | ✅ §7 [INPUT] boot summary 🚀 |

After §1–§5, USB input is at parity with Windows and Linux. §6–§7 add hot-plug and diagnostics.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_hid()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] `src/kernel/test/test_usb_hid.c` (TEST_CAT_STORAGE) covers interval encode, keyboard usage->ASCII, mouse decode, input-source coexistence merge, and the input-diag formatter (bounded-buffer edges)
  - HID interface classification (protocol 0x01 keyboard / 0x02 mouse) is validated live via QEMU enumeration -- a `USB_PROTO_HID_*` constant assert would be tautological (no-tautological-test policy)
- [x] Registered in `test_runner_init()`: `test_register_usb_hid()` (suites run under SUITE=storage)
- [x] `[INPUT] Sources:` boot line validated by grepping `build/smoke-test.stripped.log` (not a new test-smoke.sh fail-fast marker -- smoke-test stability prioritized; the line is always emitted)
- [x] Commit: `"test: add USB HID keyboard and mouse test suite"` (landed incrementally across §3-§7)

## Verification

- [ ] **QEMU USB keyboard**: `-device usb-kbd` → type commands in shell.
- [ ] **QEMU USB mouse**: `-device usb-mouse` → move cursor, click windows.
- [ ] **Bare metal USB keyboard**: type commands on real USB keyboard.
- [ ] **PS/2 + USB coexistence**: both input sources work simultaneously.
- [ ] **No PS/2 system**: laptop with no i8042 → USB keyboard is the only input → works.
- [ ] Commit: `"boot: USB HID keyboard and mouse complete -- input on any hardware"`
