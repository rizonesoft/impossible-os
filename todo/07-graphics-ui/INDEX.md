# 07 Graphics UI

This domain tracks the low-level and reusable visual building blocks used by the desktop and apps.

## Belongs Here

- Framebuffer rendering, text, images, icons, effects, and other graphics primitives.
- Reusable controls, theme infrastructure, animation primitives, and UI framework behavior.
- Asset presentation systems that support multiple products or surfaces.

## Does Not Belong Here

- Window manager, taskbar, desktop policies, or other shell behavior. Put that in [08 Desktop Shell](../08-desktop-shell/INDEX.md).
- App-specific UI backlog. Put that in [10 Apps](../10-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/gfx](../src/kernel/gfx/)
- [src/desktop/controls.c](../src/desktop/controls.c)
- [resources](../resources/)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-theme-system.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
