<!-- docs: covers=todo/11-apps/TODO-16-office.md reviewed=2026-11-27 order=16 -->
# Rizonesoft Office

## What is it?

Rizonesoft Office is an app by Rizonesoft ([Office on GitHub](https://github.com/rizonesoft/Office)). It is a permanent part of Impossible OS: it will ship in the image and start from the Start menu. Its programs open and edit documents.

## How does it work?

**Today.** Nothing is built yet. Office has no program yet; its tech stack record plans C++23 with its own toolkit on the SDK from its first line.

**Planned design.** Rizonesoft Office runs as an [Impossible SDK](https://github.com/rizonesoft/ImpossibleSDK) program. It is built from source imported at a pinned commit, with the pinned SDK toolchain, and it calls the operating system only through the Windows functions listed in the SDK's `abi/tiers.json`. It has no backend of its own on Impossible OS, and Impossible OS builds no drawing library, text engine, control set or C++ runtime for it. The build is reproducible, so the executable built here is compared with the hash the release recorded, and a difference fails the import.

1. **Import.** The source is copied in at a pinned commit after a licence audit, and a local edit under `src/apps/office/` is refused by the import check.
2. **Boundary check.** The SDK's own source check and import check run again on import; a call outside the list fails it.
3. **Build.** The program is built with the SDK toolchain and the `cxx` part, with no prebuilt object, and its hash is compared with the release.
4. **Run.** It is installed at `C:\Program Files\Rizonesoft Office\` with a Start menu entry, and runs on the CPU path first.
5. **Conformance.** A scripted session measures frame times after every import and every OS change.

## What are its interfaces?

None yet. Rizonesoft Office calls only the functions of the SDK's tiers; the measured list is produced by the boundary check once section 2 ships.

## How do I use it?

Not yet available. Once shipped, start it from the Start menu.

## What is not implemented yet?

Nothing is implemented yet. Every step is planned in [the Rizonesoft Office roadmap file](https://github.com/rizonesoft/impossible-os/blob/main/todo/11-apps/TODO-16-office.md). It builds on the SDK import and the `cxx` part in [the imported programs roadmap](https://github.com/rizonesoft/impossible-os/blob/main/todo/11-apps/TODO-15-imported-programs.md), on per-thread vector state in the scheduler roadmap.

## How does it compare with Windows 11 and Linux?

Windows 11 and most Linux distributions ship neither. Impossible OS will ship Rizonesoft Office in the image, built from the same source and toolchain as its Windows release.

## See also

- [Rizonesoft Office roadmap file](https://github.com/rizonesoft/impossible-os/blob/main/todo/11-apps/TODO-16-office.md)
- [Imported Programs and Parts](imported-programs.md)
- [PDF App (Recto)](pdf-viewer.md) and [Inkeys](inkeys.md), the other bundled apps imported from source
- [Office upstream](https://github.com/rizonesoft/Office)
