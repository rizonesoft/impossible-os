---
schema_version: 1
id: ui-framework-engines
domain: 08-graphics-ui
status: active
title: "TODO-18 -- UI Framework Engines"
---

# TODO-18 -- UI Framework Engines

> **Goal:** Build the engines underneath a 1:1 Windows 11 interface, once, so every surface is assembled from the same parts instead of drawn by hand. Today each shell surface positions and paints itself with pixel arithmetic in `src/desktop/desktop.c` and `wm.c`. This TODO adds the base every modern interface stands on: an element tree with properties, a measure and arrange layout engine, styles and visual states, routed input and focus, commands, data binding and list virtualization, a markup compiler, a composition tree with materials, a popup layer, page navigation, and the shell's data engines (settings platform, shell namespace, property system, extension points, app model, resources, shell stores). One definition of a command, a style or a setting then feeds every place it appears, which is what makes the result more consistent than the original.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-14. [`src/desktop/controls.c`](../../src/desktop/controls.c) holds four immediate-mode controls (`CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR`) in a fixed per-window array with absolute pixel rectangles; there is no element tree, no layout pass, no style system, no focus scope and no binding. `src/desktop/desktop.c` and `src/desktop/wm.c` paint the taskbar, Start menu and window chrome directly. The graphics, text, theme, animation and accessibility foundations are planned in their own files in this folder and are consumed here, not rebuilt. No shell namespace, property system, settings platform or app model exists; the Win32 export tables in `10-platform-services` list the API names only.

> [!NOTE]
> **Owner direction, 2026-11-14:** the interface is modelled 1:1 on Windows 11 and made more consistent. These engines are original work: Windows' own framework is closed source and nothing is copied from it. Vendor verdict: no permissively licensed C UI framework fits a freestanding kernel-hosted compositor (the candidates need a C++ runtime, a GPU or a libc), so the engines are written here; text shaping, vector raster and SVG stay on the vendored libraries their owning files chose.

## Inputs

- [`src/desktop/controls.c`](../../src/desktop/controls.c), [`include/desktop/controls.h`](../../include/desktop/controls.h) -- the four existing controls, migrated onto the element tree in §1
- [`src/desktop/wm.c`](../../src/desktop/wm.c), [`include/desktop/wm.h`](../../include/desktop/wm.h) -- the window manager whose windows host element trees
- [`src/desktop/desktop.c`](../../src/desktop/desktop.c) -- the hand-painted shell surfaces these engines replace
- [`include/desktop/theme_tokens.h`](../../include/desktop/theme_tokens.h) -- generated tokens every style resolves to
- -> XREF: `TODO-17-win11-parity-inventory.md §15` -- assigns inventory records to the engines and controls that build them
- -> XREF: `TODO-19-fluent-component-library.md §1` -- the controls built on these engines
- -> XREF: `TODO-01-graphics-asset-foundation.md §1,§2` -- render targets and the clip and transform stack the composition tree draws with
- -> XREF: `TODO-02-text-font-internationalization.md §3,§4` -- shaping, paragraph layout, caret and hit-test services text elements call
- -> XREF: `TODO-03-theme-system.md §2,§8` -- `theme_get()` and hot reload, the source of §6's resource values
- -> XREF: `TODO-04-animation-engine.md §1,§7` -- tweens and compositor integration that §5 and §12 drive
- -> XREF: `TODO-07-ui-accessibility-automation-ime.md §1,§2` -- the semantic tree §24 fills from the element tree
- -> XREF: `D06 T03 §2,§3` -- the key event pipeline and focus model §7 routes through
- -> XREF: `D09 T05 §1,§2` -- the search index that §16 and §17 feed
- -> XREF: `D09 T13 §1,§3` -- the explorer shell host process that §22 generalises
- -> XREF: `D09 T02 §1,§4` -- extension-to-app mapping and shortcut files that §20 builds on
- -> XREF: `D11 T05 §14,§18` -- Recto, the bundled PDF app, draws through §6, §7, §12 and §24 and registers through §19 and §20

## Outcome

- A window's content is a tree of elements laid out by measure and arrange, restyled by state, and repainted by dirty region; no surface computes pixel positions by hand.
- One command definition feeds its menu item, toolbar button, context menu entry, shortcut and access key.
- One setting definition feeds its Settings page, its Control Panel entry, its search result, its deep link and its policy lock.
- The shell namespace, property system and extension points give File Explorer, the common dialogs, the desktop and search one model of "an item".
- Every element reports itself to the accessibility tree and carries the inventory id it implements, so the layout check of `TODO-17 §16` can compare the built OS with the reference.

## Engine Quick Reference

| Layer       | Engines                                                        | Sections        |
| ----------- | -------------------------------------------------------------- | --------------- |
| Element     | Tree, properties, layout, panels, scrolling, virtualization    | §1-§4           |
| Appearance  | Styles, states, theme resources, composition, materials, popup | §5, §6, §12-§14 |
| Interaction | Routed input, focus, gestures, commands                        | §7-§9           |
| Data        | Binding, markup compiler, navigation                           | §10, §11, §15   |
| Shell data  | Settings, namespace, properties, extensions, app model, stores | §16-§20, §23    |
| Platform    | Resources, process model, automation peers                     | §21, §22, §24   |

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On        | Status |
| --- | :---: | ---------------------------------------------- | ----------------- | :----: |
| 💎  |   1   | §1 Element tree and property system            | --                |  [ ]   |
| 💎  |   2   | §2 Layout engine: measure and arrange          | §1                |  [ ]   |
| 💎  |   3   | §3 Layout panels                               | §2                |  [ ]   |
| 💎  |   4   | §4 Scrolling and virtualization engine         | §2, §3            |  [ ]   |
| 💎  |   5   | §5 Style, template and visual state engine     | §1, T04 §1        |  [ ]   |
| 💎  |   6   | §6 Theme resource resolution                   | §5, T03 §2        |  [ ]   |
| 💎  |   7   | §7 Routed input, focus and keyboard navigation | §1, D06 T03 §2,§3 |  [ ]   |
| 💎  |   8   | §8 Pointer, touch and gesture recognition      | §7                |  [ ]   |
| ⭐  |   9   | §9 Command engine                              | §7                |  [ ]   |
| 💎  |  10   | §10 Data binding and observable collections    | §1                |  [ ]   |
| ⭐  |  11   | §11 UI markup compiler                         | §3, §5, §9, §10   |  [ ]   |
| 💎  |  12   | §12 Composition visual tree                    | §1, T01 §1,§2     |  [ ]   |
| 💎  |  13   | §13 Backdrop materials engine                  | §12               |  [ ]   |
| 💎  |  14   | §14 Popup, flyout and light-dismiss layer      | §7, §12           |  [ ]   |
| 💎  |  15   | §15 Navigation framework                       | §10, §11          |  [ ]   |
| ⭐  |  16   | §16 Settings platform                          | §10               |  [ ]   |
| 💎  |  17   | §17 Shell namespace engine                     | §10               |  [ ]   |
| 💎  |  18   | §18 Property and column system                 | §17               |  [ ]   |
| 💎  |  19   | §19 Shell extension points                     | §9, §17, §18      |  [ ]   |
| 💎  |  20   | §20 App model and activation                   | §17, D09 T02 §1   |  [ ]   |
| 💎  |  21   | §21 Resource and localization engine           | §6                |  [ ]   |
| ⭐  |  22   | §22 Shell process model and crash recovery     | §12, §14          |  [ ]   |
| 💎  |  23   | §23 Shell data stores                          | §20               |  [ ]   |
| ⭐  |  24   | §24 Automation peers and inventory ids         | §1, §7, T07 §1    |  [ ]   |

> 💎 = parity work: Windows 11 and the Linux toolkits have an equivalent engine.
> ⭐ = exclusive work: one definition per command and per setting, a compiled markup with no runtime parser, crash-isolated shell surfaces, and inventory ids on every element.

---

## 1. Element Tree and Property System

**Design:** n/a -- an engine, it draws nothing by itself

The base type every control and surface is made of, and the one way a value gets onto it.

> [!WARNING]
> **Threading:** an element tree belongs to one UI thread. Other threads post work to that thread's dispatcher queue and never touch an element. The queue itself is the only shared state: it takes a spinlock with interrupts enabled and is safe from any CPU.

- [ ] Define `ui_element_t` in `include/desktop/ui/element.h`: parent, first child, next sibling, type descriptor, flags (visible, enabled, hit-testable, focusable), layout slot, and the property store
- [ ] Define `ui_type_t`: name, base type, size, vtable (`measure`, `arrange`, `render`, `on_event`, `destroy`); register types in a static table so a type is found by name
- [ ] Implement the property store: `ui_prop_id_t` registered per type with a default, a value kind (int, fixed 16.16, colour, string, object, enum) and flags (affects measure, affects arrange, affects render, inherits)
- [ ] Implement value precedence in one function, `ui_prop_effective()`: animation, local value, template binding, style setter, inherited value, default
- [ ] Invalidate from flags: a changed property marks measure, arrange or render dirty on the element and propagates measure upward to the first fixed-size ancestor
- [ ] Implement `ui_dispatcher_t` per UI thread: `ui_post(fn, arg)` from any thread, drained at the top of the thread's frame; assert on element access from a foreign thread in test builds
- [ ] Allocate elements from a per-tree slab (`kmalloc` blocks of at most 4 KiB, `pmm_alloc_contiguous` never needed); destroying a tree frees the slab in one pass
- [ ] Migrate `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX` and `CTRL_SCROLLBAR` to element types behind the existing `controls.h` functions, so current callers keep working
- [ ] Commit: `"ui: element tree, property system and dispatcher"`

**Test checkpoint:** `test_ui_element` passes: a child inherits `foreground` from its parent, a local value overrides a style setter, and changing an affects-measure property marks exactly the ancestors up to a fixed-size parent. Serial log shows `ui: element types registered (N)`. Test on: QEMU TCG and KVM at 1 and 2 CPUs.

## 2. Layout Engine: Measure and Arrange

**Design:** [`shell.md#display-scaling`](../../docs/design/shell.md#display-scaling)

Two passes: each element says how big it wants to be given what is available, then is told where it goes.

- [ ] Implement `ui_measure(el, available)` and `ui_arrange(el, final_rect)` in `src/desktop/ui/layout.c`, with sizes in device-independent units as fixed 16.16
- [ ] Honour the common layout properties in the base class: width, height, minimum and maximum, margin, padding, horizontal and vertical alignment (start, centre, end, stretch), visibility (visible, hidden, collapsed)
- [ ] Round to device pixels at arrange using the window's scale (100, 125, 150, 175, 200 and above), so an edge never lands on a half pixel and adjacent elements never gap or overlap
- [ ] Run layout only for dirty subtrees: keep the last available size and desired size per element and skip a subtree whose inputs did not change
- [ ] Detect a layout cycle: an element invalidated more than 250 times in one frame stops, logs its type and path with `klog(LOG_WARN)`, and keeps its last size
- [ ] Mirror the horizontal axis when the element's flow direction is right to left: start and end swap, margins swap, and the arranged rectangle is reflected inside the parent
- [ ] Expose `ui_layout_stats()` (elements measured, arranged and skipped per frame) through the frame statistics pseudo-file of `D09 T14 §6`
- [ ] Commit: `"ui: measure and arrange layout engine"`

**Test checkpoint:** `test_ui_layout` passes: a 100 by 20 child with margin 8 in a 300 wide stretch parent arranges at x 8, width 284; at 150 percent the device rectangle is x 12, width 426; a second layout with no change measures 0 elements. Test on: QEMU TCG and KVM.

## 3. Layout Panels

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout)

The containers every page is built from.

- [ ] Stack panel: vertical or horizontal, with a spacing property
- [ ] Grid: rows and columns sized fixed, auto or proportional (star), with row and column span, and row and column spacing
- [ ] Wrap panel and uniform grid: items flow and wrap; uniform grid gives every cell the same size with a maximum columns property
- [ ] Dock panel and canvas: dock children to edges with the last child filling; canvas places children at explicit offsets for overlays
- [ ] Relative panel: a child is placed against a sibling or the panel by named rules (left of, below, align right with, align centre with panel)
- [ ] Split container: two panes with a draggable splitter, minimum sizes, and a collapsed state for either pane
- [ ] Adaptive rule: a panel may declare width breakpoints that switch a named layout state, so a page reflows at the widths recorded in the inventory
- [ ] Commit: `"ui: layout panels"`

**Test checkpoint:** `test_ui_panels` passes: a grid with columns `auto, *, 2*` in 400 units with a 100 wide first child gives 100, 100, 200; a wrap panel of ten 90 wide items in 300 units makes four rows; a split container never shrinks a pane below its minimum. Test on: QEMU TCG and KVM.

## 4. Scrolling and Virtualization Engine

**Design:** [`controls.md#scroll-bar`](../../docs/design/controls.md#scroll-bar), [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

Long lists stay fast because only what is visible exists.

- [ ] Scroll viewer element: a viewport over content of any size, horizontal and vertical offsets, wheel, touch pan, keyboard paging, and the overlay scroll bar of `TODO-05 §9` as its indicator
- [ ] Smooth scrolling: wheel and key scrolls animate to their target with the curve recorded in the inventory; a new input retargets the running animation
- [ ] Inertia and boundary feedback for touch: velocity decay after release and the bounce at the content edge, both from recorded values
- [ ] Scroll anchoring: when content above the viewport changes size, the visible content stays where it is
- [ ] Items repeater: given a data source and an item template, it creates elements only for items in the viewport plus one screen of buffer, and recycles them through a pool
- [ ] Virtualizing layouts for the repeater: stack (uniform and variable height with estimated extents), grid and wrap, each answering "which items intersect this rectangle" without touching every item
- [ ] Bring into view: any element can ask to be scrolled into view, with a margin, used by focus movement and find
- [ ] Commit: `"ui: scroll viewer and virtualized items repeater"`

**Test checkpoint:** `test_ui_virtualize` passes: a repeater over 100,000 items with a 600 unit viewport holds fewer than 80 live elements, and scrolling to item 50,000 creates no more than one screen plus buffer. Serial log shows `ui: repeater recycle pool N`. Test on: QEMU TCG and KVM.

## 5. Style, Template and Visual State Engine

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

How a control looks is data: a style sets properties, a template builds the parts, and states change both.

- [ ] Style: a named list of property setters for a type, with a based-on style; implicit styles apply to every element of a type within a resource scope
- [ ] Control template: a control's visual tree is built from a template of named parts; the control finds its parts by name and works with any template that supplies the required ones
- [ ] Visual states: named states in groups (common: rest, hover, pressed, disabled; focus: focused, unfocused; selection: unselected, selected), each a list of setters on named parts
- [ ] State transitions: a transition between two states animates the changed properties through `TODO-04 §1` with a duration and easing; reduce-motion snaps
- [ ] One state machine for every control: `ui_state_update()` derives the common, focus and selection states from pointer, keyboard and enabled inputs, so no control invents its own hover logic
- [ ] Focus visual as a system part: the two-ring keyboard focus rectangle is drawn by the engine around any focused element, never by a control
- [ ] Density: a `compact` resource scope swaps the size resources (heights, paddings) so one template serves both densities
- [ ] Commit: `"ui: styles, control templates and visual states"`

**Test checkpoint:** `test_ui_style` passes: an implicit button style applies to a new button, a based-on style overrides one setter, and moving the pointer over then pressing yields states rest, hover, pressed in order with one transition each. Test on: QEMU TCG and KVM.

## 6. Theme Resource Resolution

**Design:** [`shell.md#materials`](../../docs/design/shell.md#materials), [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

Every colour, size and duration in a style is a name, resolved for the current theme.

- [ ] Resource dictionary: name to value, with merged dictionaries and scopes (system, app, window, element); lookup walks up the element tree then to the app and system
- [ ] Theme dictionaries: `light`, `dark` and `contrast` variants of the same names; a theme resource reference re-resolves when the theme changes
- [ ] Generate the system dictionary from `docs/design/tokens.json` with the existing generator, so `THEME_*` constants and resource names never disagree
- [ ] Live switch: on the theme change event of `TODO-03 §8`, every element with a theme resource reference is marked render dirty in one pass, with no relaunch
- [ ] Accent: derive the accent light and dark ramps once per accent change and publish them as resources
- [ ] Per-element requested theme: a subtree can be forced light or dark (a dark flyout over a light app) and its lookups use that theme
- [ ] Contrast themes: the eight contrast colour slots map to resource names, and a style may not use a colour outside them while a contrast theme is active (asserted in test builds)
- [ ] Commit: `"ui: theme resource dictionaries and live theme switch"`

**Test checkpoint:** `test_ui_resources` passes: the same button resolves a different fill in light and dark, a theme switch re-renders it without recreating it, and a subtree forced dark keeps dark values while the app is light. Serial log shows `ui: theme switch, N elements restyled`. Test on: QEMU TCG and KVM.

## 7. Routed Input, Focus and Keyboard Navigation

**Design:** [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

One input path for every element, and one focus model, so keyboard use is complete everywhere.

- [ ] Routed events: an event tunnels from the root to the target, then bubbles back; a handler may mark it handled; class handlers run before instance handlers
- [ ] Hit testing: find the topmost hit-testable element at a point, honouring clip, transform and the popup layer of §14
- [ ] Focus manager per window: one focused element, focus scopes (a menu or toolbar remembers its own focused child), and focus restoration when a popup closes
- [ ] Tab navigation: tab order from tree order and a tab index, with tab navigation modes per container (local, cycle, once)
- [ ] Directional navigation: arrow keys move focus to the nearest focusable element in that direction inside containers that opt in (grids, toolbars, tile grids)
- [ ] Access keys: Alt shows key tips, a letter invokes the matching element in the current scope, and scopes nest (a menu's keys shadow the window's)
- [ ] Accelerators: a key chord registered on an element or window invokes its command when the focus is inside its scope, checked after the focused element declines the key
- [ ] Keyboard focus is distinct from pointer focus: the focus visual shows only after keyboard use, as recorded in the inventory
- [ ] Commit: `"ui: routed events, focus manager, access keys and accelerators"`

**Test checkpoint:** `test_ui_input` passes: a click on a nested child reaches handlers in tunnel then bubble order and stops at a handler that marks it handled; Tab visits five controls in order and wraps in a cycle container; Alt then a letter invokes the right button. Test on: QEMU TCG and KVM.

## 8. Pointer, Touch and Gesture Recognition

**Design:** n/a -- an input engine, it draws nothing

Raw pointer input becomes the gestures the inventory catalogues, with one set of thresholds.

- [ ] Unified pointer events (mouse, touch, pen) with pointer id, kind, position, buttons, pressure and tilt; pointer capture and release
- [ ] Gesture recognizer per element: tap, double tap, right tap (right button or press and hold), hold, drag, with the time and distance thresholds read from the system metrics
- [ ] Manipulations for touch and precision touchpad: translate, scale and rotate deltas with inertia after release
- [ ] Wheel routing: vertical and horizontal wheel and the Ctrl-wheel zoom request, delivered to the nearest scroll viewer or zoomable element
- [ ] Hover: enter, move and leave with the hover delay used by tooltips and thumbnails
- [ ] Drag and drop: a drag source offers data formats and allowed effects, a drop target answers with the effect, modifier keys choose between copy, move and link, and Esc cancels
- [ ] Edge gestures: swipes from each screen edge are delivered to the shell before any app
- [ ] Commit: `"ui: pointer events, gesture recognition and drag and drop"`

**Test checkpoint:** `test_ui_gesture` passes on injected pointer traces: two presses 300 ms apart inside the distance threshold give one double tap, a press held past the hold time gives hold then right tap on release, and a drag past the threshold starts a drag and suppresses the tap. Test on: QEMU TCG and KVM.

## 9. Command Engine

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

A command is defined once and appears wherever it is needed.

> [!TIP]
> In Windows 11 a verb's label, icon, shortcut and enabled rule are restated by each menu, toolbar and context menu that shows it, and they drift. Here they cannot: every surface renders the same definition.

- [ ] Define `ui_command_t`: id, label, access key, icon name, description, default accelerator, `can_execute` and `execute` callbacks, and a toggle or checked state
- [ ] Command sources: a button, menu item, toolbar item or context menu entry bound to a command takes its label, icon, key hint and enabled state from it
- [ ] Re-query: `can_execute` is re-evaluated on selection change, focus change and an explicit invalidate, and every source updates
- [ ] Routing: a command executes on the nearest element in the focus path that handles it, so Copy acts on the focused text box and not the window
- [ ] Standard commands defined once in `src/desktop/ui/commands_std.c`: cut, copy, paste, undo, redo, select all, delete, rename, find, refresh, properties, new, open, save, print, close
- [ ] Command groups: an ordered list of commands and separators, declared once and rendered as a menu, a toolbar or a context menu
- [ ] The accelerator table is built from command definitions; a chord defined twice in one scope fails the build check
- [ ] Commit: `"ui: command engine with single-definition commands"`

**Test checkpoint:** `test_ui_command` passes: one Copy command bound to a menu item and a toolbar button disables both when the selection empties, Ctrl-C in a focused text box runs the text box's handler, and a duplicate chord in one scope is rejected by `scripts/ui/check-commands.py`. Test on: QEMU TCG and KVM.

## 10. Data Binding and Observable Collections

**Design:** n/a -- a data engine, it draws nothing

The link between data and elements, so a page never copies values by hand.

- [ ] Observable object: a struct with a change notification list; `ui_notify(obj, field)` tells every binding on that field
- [ ] Binding: source object and field path to a target element property, one way or two way, with an optional converter function and a fallback value
- [ ] Observable list: insert, remove, move, replace and reset notifications carrying the index range, consumed by the items repeater of §4 without a full rebuild
- [ ] Bindings are resolved to field offsets when the template is compiled (§11), so a binding update is a load and a store, not a name lookup
- [ ] Batched updates: changes made inside `ui_batch_begin()` and `ui_batch_end()` notify once
- [ ] Lifetime: a binding holds a weak reference to its source; destroying either end removes the binding with no dangling callback
- [ ] Cross-thread sources: a background thread changes data by posting to the owning dispatcher (§1); a direct change from a foreign thread asserts in test builds
- [ ] Commit: `"ui: data binding and observable collections"`

**Test checkpoint:** `test_ui_binding` passes: changing a bound field updates the element's text, a two-way bound text box writes back, inserting at index 3 of an observable list creates exactly one new item element, and destroying the source leaves no binding. Test on: QEMU TCG and KVM.

## 11. UI Markup Compiler

**Design:** n/a -- a build tool and a file format, it draws nothing

Pages and templates are declared in a markup file and compiled to C tables at build time. No parser runs in the OS.

> [!TIP]
> A declarative page is short, reviewable against an inventory record list, and hard to get subtly wrong, which is what lets a smaller model build a page correctly. Compiling at build time also means a mistake is a build error with a line number, not a blank window.

- [ ] Specify the format in `docs/design/markup.md`: an element per tag, properties as attributes, and child elements
  - Also named parts, resource references, bindings, command references, layout states, and the `inventory` attribute naming the record an element implements
- [ ] Write `tools/uic/uic.py`: parses a `.iui` file with a real XML parser, validates every tag against the type table and every attribute against the property table, and emits a `const` C table plus a header of part names
- [ ] Errors name the file, line and column and the nearest valid name ("unknown property `paddng` on `stack`, did you mean `padding`")
- [ ] Runtime loader: `ui_build(table, data_context)` instantiates the element tree from the table in one pass with no string lookups
- [ ] Check references at build time: a missing resource, command, style, binding field or inventory id fails the build
- [ ] Wire into `scripts/build.sh`: `.iui` files under `src/desktop/` and `src/apps/` compile before the C sources that include their headers, with dependency tracking so one changed page rebuilds one table
- [ ] Add a fuzz target for `uic.py`'s input and host tests in `tools/uic/tests/`, wired into `scripts/test-tooling.sh`
- [ ] Commit: `"ui: markup compiler and build integration"`

**Test checkpoint:** `python3 tools/uic/uic.py tests/ok.iui` emits a table that `test_ui_markup` builds into the expected 12-element tree; `tests/bad-prop.iui` exits 1 with the file, line and suggestion; `bash scripts/build.sh` ends with `=== BUILD OK ===`.

## 12. Composition Visual Tree

**Design:** [`shell.md#materials`](../../docs/design/shell.md#materials)

Below elements sits a retained tree of visuals the compositor draws, so moving, fading or scaling something costs no repaint.

- [ ] Define `ui_visual_t` in `include/desktop/ui/visual.h`: offset, size, opacity, 2D transform, clip (rectangle or rounded rectangle), and a content surface or solid brush
- [ ] Each element owns a visual; render fills its content surface only when render dirty, and offset, opacity, transform and clip changes skip render entirely
- [ ] Dirty region tracking per visual feeds the compositor's damage list of `D06 T02 §1`, so a frame redraws only what changed
- [ ] Implicit animations: a visual property change can animate through `TODO-04 §7` on the compositor side, continuing smoothly while the UI thread is busy
- [ ] Rounded corner clipping for windows and popups with the radii of `tokens.json`, antialiased, with a fast path for the square interior
- [ ] Shadows: a visual may cast a shadow with the elevation tokens, drawn from a cached nine-patch per radius and elevation and never re-blurred per frame
- [ ] Surface budget: content surfaces come from the render-target allocator of `TODO-01 §1`; a visual off screen for more than 2 seconds releases its surface and re-renders on return
- [ ] SMP: the visual tree is written by its UI thread and read by the compositor through a per-window commit (a swapped snapshot under one spinlock), never by walking live elements
- [ ] Commit: `"ui: retained composition visual tree"`

**Test checkpoint:** `test_ui_visual` passes: changing a visual's offset renders 0 elements and damages only the old and new rectangles; a 300 ms opacity animation advances on compositor ticks while the UI thread sleeps. Serial log shows `ui: visual commit, N visuals`. Test on: QEMU TCG and KVM at 2 CPUs.

## 13. Backdrop Materials Engine

**Design:** [`shell.md#materials`](../../docs/design/shell.md#materials)

The translucent and wallpaper-tinted backdrops, as a service a window asks for by name.

- [ ] A window or popup requests a backdrop kind: none, the wallpaper-tinted material, its alternate (stronger) form, the blurred translucent material, or the thin form used by menus
- [ ] Wallpaper-tinted material: sample a heavily blurred, downscaled copy of the wallpaper made once per wallpaper change, tinted by the theme, with no per-frame blur
- [ ] Blurred translucent material: blur what is behind the popup; cache the blurred backdrop and re-blur only the damaged region behind it
- [ ] Wire both to the existing `gfx_mica` and `gfx_acrylic` effects in `src/kernel/gfx/gfx_effects.c`, replacing direct calls from shell code
- [ ] Fallbacks, each resolving to an opaque theme colour: transparency turned off, a contrast theme, battery saver, an inactive window, and a frame budget overrun measured over 30 frames
- [ ] Noise and luminosity layers from the material tokens, generated once and tiled
- [ ] Record the cost: `ui_material_stats()` reports blur time and cache hits per frame to the frame statistics pseudo-file
- [ ] Commit: `"ui: backdrop materials with cached blur and fallbacks"`

**Test checkpoint:** `test_ui_material` passes: a static popup over a static desktop blurs once and then reports cache hits only; turning transparency off switches every backdrop to its opaque fallback in one frame. Test on: QEMU TCG and KVM; bare metal for the frame budget.

## 14. Popup, Flyout and Light-Dismiss Layer

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus), [`controls.md#tooltip`](../../docs/design/controls.md#tooltip)

One placement and dismissal engine for every menu, flyout, tooltip, combo list and dialog.

- [ ] Popup host: a top-level layer above a window's content, and windowed popups that may extend past the owning window's bounds
- [ ] Placement: a target element or point, a preferred side with fallbacks, alignment, and an offset; the popup flips or slides to stay on the work area of the display it is on
- [ ] Light dismiss: a click outside, Esc, focus leaving, or the window deactivating closes the popup; a click outside is consumed or passed through per popup kind
- [ ] Nesting: a submenu is a child popup, dismissing a parent dismisses its children, and the pointer may cross the gap to a submenu without it closing (the safe triangle)
- [ ] Open and close transitions per popup kind from the motion tokens, anchored to the side the popup opens from
- [ ] Modal layer: a dialog dims and disables its owner, traps focus, and returns focus to the element that opened it
- [ ] Tooltip service: one tooltip at a time, the show delay, the reshow delay when moving between targets, placement near the pointer or the focused element, and dismissal on any key
- [ ] Z order: tooltips over menus over flyouts over dialogs over content, one fixed order
- [ ] Commit: `"ui: popup placement, light dismiss, modal layer and tooltip service"`

**Test checkpoint:** `test_ui_popup` passes: a flyout preferring below with 10 units of room below flips above; Esc closes only the innermost of three nested menus; a modal dialog returns focus to its opener. Test on: QEMU TCG and KVM.

## 15. Navigation Framework

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame)

Pages, a back stack and deep links, for Settings, File Explorer and any app with more than one page.

- [ ] Frame element: hosts one page at a time, with `navigate(page_id, parameter)`, back and forward stacks, and a cap on stack depth
- [ ] Page lifecycle callbacks: navigated to, navigating from (may cancel), navigated from, with page state saved and restored on back
- [ ] Page transitions (entrance, drill in, slide from side) from the motion tokens, chosen by the navigation kind
- [ ] Page registry: page id to compiled markup table and data context factory, so a page is found by id from a deep link
- [ ] Deep links: a URI scheme per app maps to a page id and parameter; the Settings scheme resolves every page name recorded in the `settings` baseline of `TODO-17 §5`
- [ ] Breadcrumb model: the trail of parent pages for the current page, feeding the breadcrumb control
- [ ] Back handling: the system back gesture, Alt-Left, the mouse back button and the title bar back button all call the same function
- [ ] Commit: `"ui: frame, page lifecycle, deep links and back stack"`

**Test checkpoint:** `test_ui_nav` passes: navigating A, B, C then back twice lands on A with A's scroll offset restored; a deep link to a nested page builds the breadcrumb of its parents; a page that cancels navigating-from stays current. Test on: QEMU TCG and KVM.

## 16. Settings Platform

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

A setting is defined once: its page, its search entry, its deep link and its storage all come from that definition.

> [!TIP]
> This is the engine behind the "one setting, one page" rule of the consistency ledger. The Control Panel frame and the Settings app become two views of one table.

- [ ] Define the settings schema in `docs/design/settings-schema.md` and `src/desktop/settings/defs/*.sdef`
  - Fields: id, page, group, label and description resource names, control kind, value type, range or choices, default, Registry path
  - Also required privilege, restart or sign-out requirement, search keywords, and inventory id
- [ ] Write `tools/sdefc/sdefc.py`: compiles the definitions to a `const` table and fails on a duplicate id, a missing resource, an unknown page or a Registry path used twice
- [ ] Settings service: `settings_get(id)`, `settings_set(id, value)` with validation against the definition, and a change notification other processes subscribe to
- [ ] Store values in the Registry at the declared path, per user or per machine by the definition; machine settings need the declared privilege and go through elevation
- [ ] Policy: a setting may be locked by a policy value, in which case `settings_set` refuses and the page shows the managed state
- [ ] Generated pages: a Settings page is built from its definitions by one generic page (card, expander, group heading), with a hand-written page allowed only where the definition table cannot express the layout
- [ ] Search provider: every definition feeds the settings search index with its label, keywords and deep link
- [ ] Applying: a setting declares its apply function, called on set and at sign-in, so the stored value and the live system never disagree
- [ ] Commit: `"settings: single-definition settings platform"`

**Test checkpoint:** `test_settings_platform` passes: setting a value outside its range is refused, a locked setting refuses a write and reports managed, a subscriber receives one notification per change, and `tools/sdefc/sdefc.py --check` rejects a duplicate id. Serial log shows `settings: N definitions loaded`. Test on: QEMU TCG and KVM.

## 17. Shell Namespace Engine

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

One model of "an item" for File Explorer, the desktop, the common dialogs and search: files, and also things that are not files.

- [ ] Define the shell item in `include/shell/item.h`: an opaque id list (a path of segments, each owned by a folder provider), a display name, a parsing name, and attributes (folder, link, hidden, removable, can copy, can delete, can rename)
- [ ] Folder provider interface: enumerate children, get an item's attributes, bind to a child folder, compare two ids for sorting, parse a display name; providers register under the namespace root
- [ ] File system provider over the VFS, with change notifications from the file system's change stream
- [ ] Known folders: a table of ids to paths (Desktop, Documents, Downloads, Music, Pictures, Videos, and the system ones recorded in the `known-folders` baseline), redirectable per user, creatable on demand
- [ ] Virtual folders: the namespace root (Desktop), This PC, Network, Recycle Bin, Control Panel, Home and Gallery as providers that hold no directory of their own
- [ ] Libraries: a library is a named set of folders shown as one, with a default save location, stored as a small definition file
- [ ] Parsing and display: `shell_parse(name)` accepts a path, a known folder name or a namespace name; `shell_display_name(id, form)` returns the short, full or editing form
- [ ] Change notification: `shell_notify(event, id)` reaches every open view of the affected folder
- [ ] Commit: `"shell: namespace engine with known and virtual folders"`

**Test checkpoint:** `test_shell_namespace` passes: enumerating the root lists This PC, Network and Recycle Bin; parsing the Documents known folder name yields the user's path; creating a file in a watched folder delivers one notification. Test on: QEMU TCG and KVM.

## 18. Property and Column System

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

Every fact shown about an item (name, size, date, dimensions, artist) is a property with one definition.

- [ ] Property description table in `src/shell/props/*.pdef`: key, canonical name, label resource, type, and display rules
  - Types: string, integer, size, date, duration, rating, enum, multi-value
  - Display rules: format, default column width, alignment, sort and group behaviour, and whether it is editable
- [ ] Property store per item: get and set by key, backed by the file system for basic properties and by property handlers for file contents
- [ ] Property handler interface: a handler registered for a file type reads properties from the file (image dimensions, media duration and tags, document author)
- [ ] Formatting: one function turns a value into display text by its description and the user's regional settings (sizes, relative dates, durations)
- [ ] Column sets: the default columns per folder kind and the full list a user may add, declared as property key lists
- [ ] Sort, group and filter by property: stable sort with the natural name order (digits compare as numbers), grouping buckets per type (dates by period, sizes by range, names by first letter), and value filters
- [ ] Per-folder view state: view mode, sort, group, columns and widths stored by folder kind and overridden per folder
- [ ] Commit: `"shell: property system, column sets, sort and group"`

**Test checkpoint:** `test_shell_props` passes: natural sort orders `file2` before `file10`, a 1,536 byte file formats as `1.50 KB`, grouping twelve dated items yields the expected period buckets, and an unknown property key returns not found. Test on: QEMU TCG and KVM.

## 19. Shell Extension Points

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)

The places where an app adds to the shell, each a registration and an interface, each isolated from the shell process.

- [ ] Verbs: a file type, folder, drive or background registers commands (id, label, icon, the selection it applies to, the program to run); the context menu is the command group of §9 built from them, in one fixed order
- [ ] One menu, not two: every registered verb appears in the single context menu, grouped (open, edit, share, clipboard, organise, properties), with an overflow submenu only past a fixed count
- [ ] Thumbnail providers and preview handlers: registered per file type, run in a helper process with a time limit, returning a bitmap or hosting a preview pane; the thumbnail cache of `TODO-01 §5` stores results
- [ ] Icon overlays and status columns: a provider adds a small badge and a status property to items in folders it owns
- [ ] Drop targets and send-to targets: an app registers as a destination for dragged or sent items
- [ ] New-item templates: a file type registers an entry for the New menu with a template or an empty file
- [ ] Property sheet pages: a file type may add a page to the Properties dialog through a declared page definition
- [ ] Isolation: every extension runs out of process; a crash or timeout logs the extension, disables it for the session and never takes the shell down
- [ ] Commit: `"shell: extension points with out-of-process isolation"`

**Test checkpoint:** `test_shell_ext` passes: a registered verb appears once in the menu for a matching file and not for another type; a thumbnail provider that hangs is killed at its time limit, the item shows its type icon, and the log shows `shell: extension <id> disabled (timeout)`. Test on: QEMU TCG and KVM.

## 20. App Model and Activation

**Design:** n/a -- an engine, it draws nothing

What "an app" is to the shell: its identity, how it is listed, and every way it is started.

- [ ] App registration record: app id, display name resource, icon, executable, declared file types, URI schemes, capabilities, and whether it is single instance; stored under one Registry key per app
- [ ] App list source: Start's all-apps list, search, default-app pages and the taskbar read registrations, not a directory scan
- [ ] Window to app mapping: every top-level window carries its app id, so taskbar grouping, jump lists and notifications attribute to the right app
- [ ] Activation kinds, each delivered as one activation message: launch, file open, URI, toast action, jump list task, share target, startup task
- [ ] Single instance redirect: a second launch of a single-instance app forwards its activation to the running instance and exits
- [ ] Default app resolution: file type and URI scheme to app, with per-user choice and the chooser dialog when none is set, building on `D09 T02 §1,§3`
- [ ] Install and uninstall hooks: registering an app adds its list entry, types and schemes atomically, and unregistering removes them and its pins
- [ ] Restart registration: an app may ask to be restarted with its state after an update or a crash
- [ ] Commit: `"shell: app model, registration and activation"`

**Test checkpoint:** `test_app_model` passes: registering an app makes it appear in the app list query, a second launch of a single-instance app delivers one activation to the first and exits, and unregistering removes its type associations. Serial log shows `appmodel: N apps registered`. Test on: QEMU TCG and KVM.

## 21. Resource and Localization Engine

**Design:** n/a -- an engine, it draws nothing

No user-visible string, image or layout constant lives in code.

- [ ] Resource files: `resources/strings/<lang>/<app>.strings` (id, text, a translator note), compiled by `tools/resc/resc.py` into a per-app resource table
- [ ] Lookup by qualifiers: language (with fallback chain), scale, theme and contrast; `res_string(id)` and `res_image(id)` pick the best candidate
- [ ] Message formatting: named placeholders, plural forms per language, number, date and list formatting through the locale services of `D02 T13`
- [ ] Layout direction: a right-to-left language sets flow direction on every window, and images marked directional are mirrored
- [ ] Pseudo-localization build: every string lengthened and accented, so truncation and hard-coded text show up in a test run
- [ ] Build check: `resc.py --check` fails on a string id used in markup or code that no resource file defines, and on a defined id nothing uses
- [ ] Live language change: a change of display language re-resolves strings at next window creation, and shell surfaces rebuild without sign-out
- [ ] Commit: `"ui: resource and localization engine"`

**Test checkpoint:** `test_ui_resources_loc` passes: a string resolves in the fallback language when the preferred one lacks it, the plural form for 1 and 5 differs in English, and the pseudo-localized build renders every label of the test page longer. `python3 tools/resc/resc.py --check` exits 0.

## 22. Shell Process Model and Crash Recovery

**Design:** n/a -- a process arrangement, it draws nothing

Each shell surface runs where its failure is contained, and comes back by itself.

> [!TIP]
> When the Windows shell process fails, the taskbar, desktop and open folder windows vanish together. Here a failed surface restarts alone and the others never notice.

- [ ] Split the shell into hosts, each a process with its own element trees: desktop and taskbar, Start and search, flyouts and notifications, and one per File Explorer window group
- [ ] Shell supervisor in the session manager: starts each host, watches it, and restarts a failed one with backoff (immediately, 1 s, 5 s, then a visible fault notice)
- [ ] State survives a restart: pins, open flyout state and folder windows are kept in the shell stores of §23, so a restarted host rebuilds what it showed
- [ ] The compositor keeps a failed host's last frame on screen until the new process commits its first, so nothing flashes
- [ ] Host contracts: hosts talk through named ports with versioned messages; a host never maps another's memory
- [ ] Safe mode shell: after three failures of one host in a minute, it starts with extensions off and a notice names the last extension loaded
- [ ] Record each restart in the event log with the host, the fault and the restart count
- [ ] Commit: `"shell: isolated surface hosts with supervised restart"`

**Test checkpoint:** killing the Start host in the test harness leaves the taskbar responsive, the serial log shows `shell: host start restarted (1)` within 2 seconds, and the next Win key press opens Start. Test on: QEMU TCG and KVM at 2 CPUs.

## 23. Shell Data Stores

**Design:** n/a -- a data layer, it draws nothing

The lists the shell shows, each with one owner and one format.

- [ ] Pin store: Start pins and folders, taskbar pins, quick access pins; ordered, per user, with change notifications
- [ ] Recent and frequent store: recent items per app and system-wide, frequent folders, with a per-item remove and a clear all, and a setting that stops recording
- [ ] Jump list store: per app categories (pinned, recent, frequent, custom) and tasks, written by the app through one call
- [ ] Recommended store: the Start recommended list, computed locally from recent and newly installed items; nothing leaves the machine
- [ ] Notification store: delivered notifications per app for the notification center, with expiry
- [ ] Layout store: Start layout choice, taskbar alignment and item visibility, tray icon visibility, desktop icon positions per display arrangement
- [ ] One storage engine for all six: a small versioned record file per store under the user profile, written atomically (write new, flush, rename), with a migration function per version
- [ ] Commit: `"shell: pin, recent, jump list, notification and layout stores"`

**Test checkpoint:** `test_shell_stores` passes: a pin added in one process is seen by a subscriber in another, a store file truncated mid-record loads its last good version, and clearing recent items empties every view. Test on: QEMU TCG and KVM.

## 24. Automation Peers and Inventory Ids

**Design:** [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

Every element describes itself, so assistive tools work everywhere and the built OS can be measured.

- [ ] Each element type supplies an automation peer: role, name, value, state flags, supported actions (invoke, toggle, expand, select, scroll, set value) and its bounding rectangle
- [ ] Peers are created on demand and feed the semantic node tree of `TODO-07 §1`; the tree follows element tree changes through the same invalidation as layout
- [ ] Names come from the label, the labelled-by relation, or an explicit name property; an interactive element with no name fails `uic.py` at build time
- [ ] The `inventory` attribute of §11 is stored on the element and exposed on its peer as `inventoryId`
- [ ] Events: focus changed, property changed, structure changed, invoked, selection changed, raised once per change and coalesced per frame
- [ ] Keyboard completeness check in test builds: every element with an invoke, toggle or expand action must be reachable by Tab or arrow navigation
- [ ] Dump: `ui_automation_dump(window)` writes the peer tree as JSON through the transport of `TODO-07 §6`, the input of `TODO-17 §16`
- [ ] Commit: `"ui: automation peers and inventory ids on every element"`

**Test checkpoint:** `test_ui_automation` passes: a button's peer reports role button, its label as name and the invoke action; an unnamed button in `tests/noname.iui` fails `uic.py`; the dump of the test page lists every element with an `inventoryId`. Test on: QEMU TCG and KVM.

---

## OS Comparison

| ⭐  | Feature                              | 🪟 Win11                   | 🐧 Linux                | 🚀 Impossible OS    |
| --- | ------------------------------------ | -------------------------- | ----------------------- | ------------------- |
| 💎  | Element tree and layout              | ✅ WinUI, XAML             | ✅ GTK, Qt              | ⬜ Planned -- §1-§4 |
| 💎  | Styles and visual states             | ✅ Styles, state manager   | ✅ CSS in GTK, QSS      | ⬜ Planned -- §5    |
| 💎  | Retained composition and materials   | ✅ Composition, Mica       | ⚠️ Compositor dependent | ⬜ Planned -- §12   |
| ⭐  | Markup compiled, no runtime parser   | ⚠️ Binary markup, loaded   | ⚠️ Parsed at run time   | ⬜ Planned -- §11   |
| ⭐  | One definition per command           | ❌ Restated per surface    | ⚠️ Actions, optional    | ⬜ Planned -- §9    |
| ⭐  | One definition per setting           | ❌ Settings and Panel      | ⚠️ Schemas, per desktop | ⬜ Planned -- §16   |
| 💎  | Shell namespace and properties       | ✅ Shell items, properties | ⚠️ GVfs, per toolkit    | ⬜ Planned -- §17   |
| ⭐  | Extensions isolated from the shell   | ⚠️ Many load in process    | ⚠️ Varies               | ⬜ Planned -- §19   |
| ⭐  | Shell surfaces restart independently | ❌ One shell process       | ⚠️ Varies by desktop    | ⬜ Planned -- §22   |

> **After §1-§8:** a page can be laid out, styled and operated by keyboard, pointer and touch. **After §9-§15:** pages are declared in markup with shared commands, materials and navigation. **After §16-§24:** the shell's data has one model each, and every element can be checked against the Windows 11 inventory.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ui_framework()` in `src/kernel/test/test_runner.c`, category `TEST_CAT_DESKTOP`. Tests build element trees in memory and inject input; they never call live desktop init.

- [ ] Create `src/kernel/test/test_ui_framework.c` with:
  - `ui_prop_effective()` returns the local value over a style setter and the inherited value over the default
  - `ui_measure()` of a 100 by 20 child with margin 8 returns a desired size of 116 by 36
  - A grid with columns `auto, *, 2*` in 400 units arranges to 100, 100, 200
  - A repeater over 100,000 items holds fewer than 80 live elements
  - A routed click reaches handlers in tunnel then bubble order and stops when handled
  - A command bound to two sources disables both when `can_execute` turns false
  - Inserting into an observable list creates exactly one item element
- [ ] Create `src/kernel/test/test_shell_engines.c` with the namespace, property, extension, app model, settings and store cases named in the test checkpoints of §16 to §23
- [ ] Register both in `test_runner_init()`: `test_register_ui_framework()` and `test_register_shell_engines()`
- [ ] Author `scripts/debug/desktop/run-ui-framework-tests.bat`
- [ ] Host tests for `tools/uic`, `tools/sdefc` and `tools/resc` in each tool's `tests/` folder, wired into `scripts/test-tooling.sh`
- [ ] Commit: `"test: UI framework and shell engine suites"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`
- [ ] `bash scripts/test.sh SUITE=desktop` passes with the two new suites listed
- [ ] `bash scripts/test-smoke-matrix.sh` passes on TCG and KVM at 1 and 2 CPUs; the serial log contains `ui: element types registered`, `settings: N definitions loaded` and `appmodel: N apps registered`
- [ ] The desktop boots with the taskbar and Start built from markup and no call to the old hand-painted paths (`git grep -n 'desktop_draw_taskbar_legacy'` prints nothing)
- [ ] QEMU WHPX at 2 CPUs: theme switch, a host restart and a 100,000 item list scroll each complete without a dropped frame warning in the frame statistics
- [ ] Bare metal: materials hold the frame budget on integrated graphics hardware, and touch gestures match the recorded thresholds on a touch display (VM behaviour differs for both)
