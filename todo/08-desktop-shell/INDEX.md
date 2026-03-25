# 08 Desktop Shell

This domain covers the user-visible desktop shell: compositor policy, shell surfaces, desktop workflows, and the overall shell experience.

## Belongs Here

- Desktop shell, compositor behavior, window manager rules, and workspace UX.
- Taskbar, start menu, notifications, boot splash experience, and BSOD presentation.
- Shell-level coordination that ties reusable UI pieces into the full desktop product.

## Does Not Belong Here

- Low-level graphics primitives, theme internals, or reusable controls. Put that in [07 Graphics UI](../07-graphics-ui/INDEX.md).
- Standalone built-in applications. Put that in [10 Apps](../10-apps/INDEX.md).

## Likely Source Areas

- [src/desktop](../src/desktop/)
- [src/kernel/main](../src/kernel/main/)
- [src/kernel/boot_splash.c](../src/kernel/boot_splash.c)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-compositor-policies.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
