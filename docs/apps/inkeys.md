<!-- docs: covers=todo/11-apps/TODO-14-inkeys.md reviewed=2026-11-22 order=14 -->
# Inkeys Screen Annotation

## What is it?

Inkeys is a free, open-source screen annotation and whiteboard app by AlanCRL, licensed under the GPL-3.0. It puts a toolbar over the desktop so you can draw with a pen, a finger or the mouse over any window, or switch to a full-screen whiteboard. Inkeys is a permanent part of Impossible OS: it will ship in the image as the system's annotation app.

## How does it work?

**Today.** Nothing is built yet. Inkeys is a Windows app written mainly in C++: its toolbar uses Direct2D 1.1, its canvas uses Direct3D 11 with HLSL shaders, its settings window uses Dear ImGui and ImFluent, and it presents through DirectComposition with a layered-window fallback. Impossible OS does not yet have a C++ runtime in user mode or the DirectX interfaces Inkeys calls.

**Planned design.** Inkeys stays a Windows app first. Impossible OS imports its source at a pinned commit, never edits it locally, and makes the operating system run it by implementing the Windows APIs Inkeys uses. Anything Inkeys itself has to change is made in the Inkeys repository and arrives through the next import.

1. **Import.** The source is copied in at a pinned commit, with a licence audit of every third-party component; anything that cannot be redistributed under the GPL-3.0 is left out.
2. **API inventory.** A host tool lists every Windows API and COM object Inkeys uses and marks each as implemented, stubbed or missing, with the section that owns it.
3. **Compatibility probes.** Small programs, each one step of what Inkeys does (a GDI drawing window, a transparent layered window, a Direct2D toolbar, a Direct3D 11 canvas, pen and touch input), pass on Windows 11 first and then on Impossible OS.
4. **Build and ship.** Inkeys is built from the imported tree with the project toolchain and installed at `C:\Program Files\Inkeys\` with a Start menu entry.
5. **Conformance.** A scripted drawing session measures frame times and input latency after every import and every OS change.

## What are its interfaces?

None yet. Inkeys calls the standard Windows interfaces (Win32 windows and input, COM, Direct2D 1.1, Direct3D 11, DirectComposition); the measured list will live in `docs/apps/inkeys-api.json` once section 2 ships.

## How do I use it?

Not yet available. Once shipped, start Inkeys from the Start menu or run `C:\Program Files\Inkeys\Inkeys.exe`.

## What is not implemented yet?

Nothing is implemented yet. Every step is planned in [the Inkeys roadmap file](https://github.com/rizonesoft/impossible-os/blob/main/todo/11-apps/TODO-14-inkeys.md): the import (section 1), the API inventory (section 2), the compatibility probes (section 3), the build (section 4), the app itself (section 5) and the conformance suite (section 6). It builds on the C++ runtime planned for Recto and on Tier 9 of the Win32 compatibility matrix, which owns Direct3D 11, Direct2D and DirectComposition.

To update Inkeys later: bump the pinned commit, re-run the import script, rebuild, and run the conformance suite. The import check refuses any local edit under `src/apps/inkeys/`.

## How does it compare with Windows 11 and Linux?

Windows 11 offers live drawing on screen only as the Quick markup step of the Snipping Tool, and its whiteboard is a separate app. Linux desktops ship no annotation tool by default; Gromit-MPX is a popular add-on. Impossible OS will ship Inkeys in the image, with both live annotation and a whiteboard.

## See also

- [Inkeys roadmap file](https://github.com/rizonesoft/impossible-os/blob/main/todo/11-apps/TODO-14-inkeys.md)
- [PDF App (Recto)](pdf-viewer.md), the other bundled app imported from source
- [Inkeys upstream](https://github.com/Alan-CRL/Inkeys)
