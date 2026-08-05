---
schema_version: 1
id: ui-accessibility-automation-ime
domain: 08-graphics-ui
status: active
title: "TODO-07 -- UI Accessibility, Automation, and IME Foundation"
---

# TODO-07 -- UI Accessibility, Automation, and IME Foundation

> **Goal:** Build the reusable semantics, automation, and text-composition infrastructure that modern desktop UI stacks require. Impossible OS already plans high-contrast, magnifier, sticky keys, and large-cursor features, and it already has placeholder control accessibility stubs plus future Win32k IME/accessibility syscall ranges; this TODO provides the missing shared foundation under those features: semantic trees, role/state/value providers, accessibility events, inspection and automation hooks, and an input-method/composition layer for multilingual text entry.

> [!IMPORTANT]
> **Current state:** `TODO-05-widget-library-core.md §8` already plans simple `ctrl_get_accessible_name()` and `ctrl_get_accessible_role()` stubs, but there is no semantic tree, event model, or assistive-service bridge yet. `D06 T03` already owns the base key-event routing, focus, and hotkey pipeline. `TODO-15-win32k-shadow-ssdt.md` already reserves USER keyboard, IME, hook, DPI, and accessibility waves, but those syscall rows do not yet have a shared provider layer underneath them. `D10 T06` already owns user-facing assistive features like magnifier, sticky keys, reduced motion, and large cursor, but it does not own semantic roles, automation providers, or IME composition internals. The missing work is the foundation that lets screen readers, automation tools, Win32 accessibility APIs, and multilingual text input work correctly across the whole UI stack.

## Inputs

- [`include/desktop/controls.h`](../../include/desktop/controls.h) -- control metadata and future accessible-name/role seeds
- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- control draw/input code that will publish roles, states, and values
- [`include/desktop/wm.h`](../../include/desktop/wm.h) -- window identity, focus, z-order, and message routing
- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- focus changes, hit-testing, and event routing
- [`src/desktop/terminal.c`](../../src/desktop/terminal.c) -- text composition and selection consumer
- [`include/font_mgr.h`](../../include/font_mgr.h) -- composition and text-run consumers from the shared text stack
- -> XREF: `TODO-05-widget-library-core.md §8` -- control accessibility stubs are the seed data for the full semantics/provider layer
- -> XREF: `TODO-15-win32k-shadow-ssdt.md §21,§22` -- USER IME, accessibility, and automation-facing syscalls consume the shared foundation
- -> XREF: `D06 T03 §2,§3` -- IME and accessibility events build on the existing key-event and focus-routing pipeline
- -> XREF: `D00 T05 §14` -- WCAG sweep consumer; the test framework walks the §6 automation tree and gates CI on contrast / keyboard / focus rules

## Outcome

- Windows, controls, menus, lists, dialogs, and text surfaces publish one semantic tree with roles, names, states, values, and actions.
- Accessibility and automation consumers can query focus, hit-test, subscribe to events, and invoke actions without scraping pixels.
- IME/input-method composition has one shared pre-edit, candidate, and commit pipeline instead of per-control hacks.
- Win32k accessibility and IME syscall waves gain a real provider model underneath them.
- Platform accessibility features, screen readers, and automated UI tests all share the same semantic/automation substrate.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On        | Status |
| --- | :---: | ---------------------------------------- | ----------------- | :----: |
| 💎   |   1   | §1 Semantic node tree, roles, states, values, and action descriptors | --                |  [ ]   |
| 💎   |   2   | §2 Provider/query API, focus/hit-test lookup, and accessibility events | §1                |  [ ]   |
| 💎   |   3   | §3 Inspect bridge, screen-reader hooks, and automation subscription model | §1, §2            |  [ ]   |
| 💎   |   4   | §4 IME and text-composition framework    | §2, D06 T03 §2,§3 |  [ ]   |
| 💎   |   5   | §5 Win32 and platform-service wiring     | §1-§4             |  [ ]   |
| ⭐   |   6   | §6 Deterministic automation transport for testing and assistive tooling | §2-§5             |  [ ]   |

> 💎 = parity work -- matches the accessibility and input-method layers that Windows 11 and Linux desktops already provide.
> ⭐ = exclusive work -- Impossible OS gets a cleaner automation contract for both assistive tools and tests.

---

## 1. Semantic Node Tree, Roles, States, Values, and Action Descriptors

Create the canonical semantic tree that turns windows and controls into accessible UI objects rather than anonymous pixels.

- [ ] Add `ui_node_t`, `ui_role_t`, `ui_state_t`, `ui_action_t`, and `ui_value_t` in a new `include/ui_access.h` plus `src/desktop/ui_access_tree.c`
- [ ] Model parent/child relationships for top-level windows, child windows, controls, menu items, list rows, tree nodes, and text surfaces
- [ ] Extend control and window metadata so names, descriptions, checked/selected/expanded states, current values, and supported actions are published through one API
- [ ] Upgrade the simple `ctrl_get_accessible_name/role()` plan from `TODO-04 §8` into real provider-backed node data instead of loose string helpers
- [ ] Add explicit lifetime rules and stable node IDs so assistive and automation clients can survive redraws and focus changes
- [ ] Log node-tree rebuilds and invalid node references with `klog(LOG_INFO, "UIA", ...)` or `LOG_WARN` when invariants are violated
- [ ] Commit: `"uia: semantic tree foundation -- nodes, roles, states, values, action descriptors"`

**Test checkpoint:** Focusing a window or control yields a stable semantic node with expected role/name/state fields; rebuilding a window tree preserves stable IDs where intended; serial shows `"UIA: tree rebuild nodes=N"`. Test on: QEMU WHPX + TCG; bare metal.

## 2. Provider/Query API, Focus/Hit-Test Lookup, and Accessibility Events

Add the programmatic API that lets screen readers, automation tools, and Win32k consumers query and react to the semantic tree.

- [ ] Implement `ui_access_get_focused()`, `ui_access_hit_test(x, y)`, `ui_access_get_children(node)`, and `ui_access_invoke(node, action)` on top of the semantic tree
- [ ] Add event emission for focus changed, value changed, text selection changed, expanded/collapsed, live region update, and layout changed
- [ ] Add event coalescing and ordering rules so noisy controls do not flood clients during animation or bulk updates
- [ ] Add provider hooks to controls, menus, list views, trees, and text fields so state changes emit semantic events alongside visual redraw
- [ ] Define failure semantics for stale handles, out-of-tree nodes, and unsupported actions with explicit error codes
- [ ] Emit `klog(LOG_INFO, "UIA", "event type=%u node=%u")` throttled diagnostics during bring-up
- [ ] Commit: `"uia: provider API -- focus/hit-test query, invoke, and event emission"`

**Test checkpoint:** Changing focus and value on a control produces the expected event sequence; hit-testing a coordinate returns the expected node; serial shows `"UIA: event type="` for focus and value changes. Test on: QEMU WHPX + TCG; bare metal.

## 3. Inspect Bridge, Screen-Reader Hooks, and Automation Subscription Model

Provide the bridge that lets user-mode tooling inspect the semantic tree and subscribe to changes without baking assistive behavior into every widget.

- [ ] Add an inspector-facing service or syscall surface for enumerating nodes, reading properties, and subscribing to semantic events
- [ ] Add a small in-tree inspection tool contract (for example `uia_inspect` or equivalent shell/debug surface) so semantic bugs can be triaged without external guesswork
- [ ] Add screen-reader friendly navigation helpers: current focus path, next/previous semantic sibling, first actionable child, and live-region announcements
- [ ] Add automation subscription and cancellation APIs so user-mode test tooling and assistive tools can watch one subtree instead of polling the whole desktop
- [ ] Define permission and isolation boundaries so one app cannot casually introspect another app's sensitive text without an explicit policy decision
- [ ] Commit: `"uia: inspect bridge -- tree enumeration, event subscription, screen-reader navigation helpers"`

**Test checkpoint:** An inspector client can enumerate the current desktop tree and observe focus-change events without polling; serial shows `"UIA: subscriber add"` and `"UIA: subscriber remove"` during test flows. Test on: QEMU WHPX + TCG; bare metal.

## 4. IME and Text-Composition Framework

Build the shared pre-edit and commit pipeline needed for multilingual text entry and future Win32 IME APIs.

- [ ] Add `ime_context_t`, `ime_preedit_t`, and candidate-list structures in a new `include/ime.h` plus `src/desktop/ime.c`
- [ ] Integrate IME context ownership with the focus and key-event pipeline from `D06 T03`, including composition start/update/commit/cancel
- [ ] Add pre-edit rendering hooks for inline composition underlines, caret affinity, and candidate windows on top of the shared text foundation from `TODO-15`
- [ ] Define keyboard-layout and input-method module boundaries so basic layout switching and full IME engines can share one dispatch path
- [ ] Add `klog(LOG_INFO, "IME", "compose len=%u candidates=%u")` observability for bring-up and parity debugging
- [ ] Commit: `"ime: composition framework -- context ownership, preedit, candidate lists, commit/cancel flow"`

**Test checkpoint:** Starting composition on a focused text control produces a pre-edit range; committing or cancelling composition updates the target buffer correctly; serial shows `"IME: compose len="` and `"candidates="`. Test on: QEMU WHPX + TCG; bare metal.

## 5. Win32 and Platform-Service Wiring

Connect the semantics and IME foundation to the roadmap items that sit above it without duplicating ownership.

- [ ] Wire Win32k accessibility and IME-facing roadmap work in `TODO-12 §21,§22` to this provider model instead of inventing separate per-syscall metadata
- [ ] Wire `D10 T06` assistive features to semantic focus/value events where appropriate, but keep user-facing feature UX ownership in that platform-services TODO
- [ ] Add desktop and control-level notifications for `WM_GETOBJECT`-style accessibility queries, focus announcements, and `WM_INPUTLANGCHANGE`-style layout changes
- [ ] Update menu, dialog, list, tree, terminal, and text-control plans to consume provider and composition hooks where they touch semantics or IME
- [ ] Add one central "owner map" note in comments or docs so future TODOs know this file owns semantic providers, not the individual app or shell feature files
- [ ] Commit: `"uia: integration wiring -- win32k provider bridge, assistive-feature hooks, layout-change notifications"`

**Test checkpoint:** A Win32 or desktop accessibility query resolves through the shared provider layer; enabling assistive features still uses the same focused semantic node; serial shows `"UIA: win32 bridge query"` and `"IME: layout change"` during integration tests. Test on: QEMU WHPX + TCG; bare metal.

## 6. Deterministic Automation Transport for Testing and Assistive Tooling

Add a first-class automation transport that lets Impossible OS do better than pixel-scraping or brittle synthetic-click frameworks.

> [!TIP]
> Windows UI Automation and Linux AT-SPI already expose semantic trees, but automated test flows still often depend on framework-specific wrappers or race-prone user input synthesis. Impossible OS can do better by making deterministic semantic automation a core capability.

- [ ] Add a transport for semantic node lookup, action invocation, value set/get, and event replay that does not require simulated mouse coordinates when a semantic action exists
- [ ] Add ordering and acknowledgement rules so automated tests can wait for semantic completion instead of sleeping arbitrarily
- [ ] Add a small replayable event trace format for semantic actions and resulting UIA/IME events
- [ ] Keep raw input synthesis available for parity, but make the semantic transport the preferred path for tests and assistive tools
- [ ] Add `klog(LOG_INFO, "UIA", "automation action=%u ack=%u")` counters for timing review
- [ ] Commit: `"uia: automation transport -- deterministic semantic invoke/ack/event replay"`

**Test checkpoint:** An automation client invokes a button and waits for the semantic acknowledgement without synthetic clicks; the same flow emits a replayable event trace; serial shows `"UIA: automation action="` and `"ack="`. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐   | Feature                           | 🪟 Win11                | 🐧 Linux                  | 🚀 Impossible OS |
| --- | --------------------------------- | ---------------------- | ------------------------ | --------------- |
| 💎   | Semantic accessibility tree       | ✅ UIA/MSAA bridge      | ✅ AT-SPI roles/states    | ⬜ Planned - §1  |
| 💎   | Query + event provider API        | ✅ UIA providers/events | ✅ AT-SPI events          | ⬜ Planned - §2  |
| 💎   | Inspect + screen-reader hooks     | ✅ Inspect/Narrator     | ✅ Orca + inspectors      | ⬜ Planned - §3  |
| 💎   | IME + composition framework       | ✅ IMM/TSF              | ✅ IBus/fcitx toolkit     | ⬜ Planned - §4  |
| 💎   | Win32/accessibility bridge        | ✅ USER32/Win32k        | ✅ toolkit + desktop glue | ⬜ Planned - §5  |
| ⭐   | Deterministic semantic automation | ⚠️ UIA + app wrappers  | ⚠️ AT-SPI + app wrappers | ⬜ Planned - §6  |

After §1-§5, Impossible OS reaches parity with the semantics, automation, and IME layers exposed by modern Windows and Linux desktops. After §6, it gains a cleaner automation path that should be better for both accessibility tooling and regression tests.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ui_access()` -- register in `src/kernel/test/test_runner.c`.
> This TODO likely needs a new `TEST_CAT_DESKTOP` or `TEST_CAT_GFX` category because semantic accessibility and IME infrastructure do not fit the existing categories well.

- [ ] Create `src/kernel/test/test_ui_access.c` with:
  - semantic tree build returns expected roles/names/states for a sample control hierarchy
  - focus change and value change emit the expected event sequence
  - hit-testing returns the expected node for a known coordinate
  - IME composition start/update/commit updates the target text state correctly
  - automation invoke/ack flow returns the expected completion event
- [ ] Register in `test_runner_init()`: `test_register_ui_access()`
- [ ] Commit: `"test: add UI accessibility and IME foundation test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] Serial shows `"UIA: tree rebuild nodes="`, `"UIA: event type="`, and `"IME: compose len="` during semantic and composition tests
- [ ] Inspector tooling can enumerate the semantic tree and observe focus changes
- [ ] Automation transport can invoke a semantic action and wait for acknowledgement without raw coordinate clicks
- [ ] UI accessibility and IME tests pass via the new graphics/desktop test category
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
