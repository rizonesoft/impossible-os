<!-- docs: covers=todo/08-graphics-ui/TODO-19-fluent-component-library.md sources=src/desktop/controls.c,include/desktop/controls.h,docs/design/controls.md reviewed=2026-11-14 order=20 -->
# Fluent Component Library

## What is it?

The component library is the set of controls a Windows 11 style interface is built from that no other roadmap file owns: the button family, toggle switch, text and number entry, pickers, flyouts, the command bar, the dialog frame, navigation view, breadcrumb, tab view, settings cards, grid and flip views, the title bar and media controls. It also sets one contract every control meets. Nothing in it is implemented yet.

## How does it work?

**Today.** [`controls.c`](../../src/desktop/controls.c) implements a button, a label, a text box and a scroll bar, declared in [`controls.h`](../../include/desktop/controls.h). The [controls specification](../design/controls.md) describes more controls than exist.

**Planned design.** Each control is an element type with a markup template on the [UI framework engines](ui-framework-engines.md). A written contract lists what "done" means for any control: visual states from the shared state engine, full keyboard use, pointer, touch and pen, an automation peer, right-to-left, two densities, and the light, dark and contrast themes. A gallery app shows every control in every state, and a conformance suite compares each state with the measurements in the [Windows 11 parity inventory](win11-parity-inventory.md).

Classic Win32 control classes are rendered by thin adapters over the same controls, so an old-style dialog and a new page look alike.

## What are its interfaces?

Shipped today: the four controls in [`controls.h`](../../include/desktop/controls.h). Planned: one element type and template per control under `src/desktop/ui/controls/`, the checker `scripts/ui/check-control.py`, the gallery app under `src/apps/gallery/`, and the suite `scripts/ui/state-matrix.py`.

## How do I use it?

There is nothing new to run yet.

## What is not implemented yet?

Every control and tool in this library is planned and none is built. The [roadmap file](../../todo/08-graphics-ui/TODO-19-fluent-component-library.md) owns each one. Check box, radio button, slider, progress bar, combo box, tab strip and overlay scroll bar stay with [Core Widgets](widget-library.md); list view, tree view, toolbar, menu bar, status bar, tooltip, dialog system and info bar stay with [Complex Controls and Dialogs](complex-controls-dialogs.md).

## How does it compare with Windows 11 and Linux?

Windows 11 ships this control set in WinUI, and Linux has equivalents in GTK 4, libadwaita and Qt. Planned differences here: one dialog frame and one title bar system-wide where Windows 11 has several, classic controls that follow the modern look and dark mode, and a conformance suite that checks every control state against a measured reference.

## See also

- [Fluent component library roadmap](../../todo/08-graphics-ui/TODO-19-fluent-component-library.md)
- [UI Framework Engines](ui-framework-engines.md) and [Windows 11 Parity Inventory](win11-parity-inventory.md)
- [Controls Specification](../design/controls.md)
