---
schema_version: 1
id: inkeys
domain: 11-apps
status: active
title: "TODO-14 -- Inkeys Screen Annotation and Whiteboard"
---

# TODO-14 -- Inkeys Screen Annotation and Whiteboard

> **Goal:** Bundle Inkeys as the permanent screen annotation and whiteboard app of Impossible OS. Inkeys (`github.com/Alan-CRL/Inkeys`, by AlanCRL, GPL-3.0) is a mature Win32 and COM application: a Direct2D 1.1 toolbar, a Direct3D 11 and HLSL canvas, a Dear ImGui and ImFluent settings window, DirectComposition presentation with an `UpdateLayeredWindowIndirect` fallback, and Windows 7 SP1 support. Impossible OS imports Inkeys' source at a pinned commit and makes the OS run it: the compatibility work is done in Impossible OS by implementing the Windows APIs Inkeys calls, not by forking Inkeys. Inkeys stays a Windows app first; anything Inkeys itself has to change is done upstream and arrives through a re-import.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-22. Nothing in this file is built. Inkeys3 is developed on the upstream `dev` branch (head `c06697b1ab2b`, 2026-11-16) and builds with MSVC and vcpkg for x86, x64 and ARM64. Impossible OS user mode has a C library and no C++ runtime (planned in `11-apps/TODO-05-pdf-viewer.md` §11), and no Direct3D 11, Direct2D or DirectComposition (prerequisites of `12-user-platform-sdk/TODO-07-win32-compat-matrix.md` §13, owners not yet filed). So §1 and §2 are ours now; §3 climbs the compatibility ladder as the OS gains each API; §4 to §6 follow.

> [!NOTE]
> **Owner direction, 2026-11-22:** Inkeys is a permanent part of Impossible OS, like Recto, Office and Isotone. The rule for every bundled app: it is developed and made to work on Windows first, then Impossible OS is made compatible with it. Requested in `rizonesoft/impossible-os#7` by its author. Reversal cost: low; removing the app is deleting `src/apps/inkeys/` and its rows. **Approach decision (this file):** Inkeys runs on the Impossible OS Win32, COM and DirectX surface, built from the imported source; it gets no Impossible OS backend of its own (Recto's §12 seam model is not used here). Why: Inkeys is somebody else's project with its own release cadence, and the APIs it needs (D3D11, D2D, DComp, layered windows, pointer input) are ones the Win32 compatibility matrix must deliver anyway, so a private backend would be throwaway work. Reversal cost: medium; a backend would add a section between §4 and §5. **Vendor verdict:** Inkeys' `LICENSE` is GPL-3.0 (read at the upstream `dev` branch 2026-11-22), compatible with this project's GPL-3.0-only; its third-party set is audited per component in §1.

---

## Inputs

- `github.com/Alan-CRL/Inkeys` (`dev` branch) -- the upstream source, its `README.md` third-party table, `.gitmodules` (vcpkg, HiMsg) and `.github/workflows/build-windows.yml` -- imported by §1
- -> XREF: `11-apps/TODO-05-pdf-viewer.md §11` -- the C++23 user-mode runtime Inkeys builds on (§4); one runtime for every bundled C++ app, never a second copy here
- -> XREF: `12-user-platform-sdk/TODO-07-win32-compat-matrix.md §13` -- Tier 9 owns the Direct3D 11, Direct2D, DirectWrite and DirectComposition prerequisites and lists Inkeys as a real binary target (§3, §5)
- -> XREF: `12-user-platform-sdk/TODO-07-win32-compat-matrix.md §1` -- the API coverage tracker the §2 inventory feeds
- -> XREF: `10-platform-services/TODO-12-long-term-features.md §4` -- touch and pen input delivered as `WM_POINTER*` with pressure, the input Inkeys draws with (§5)
- `src/libs/PROVENANCE.md`, `CREDITS.md` -- vendoring records (§1)
- `todo/11-apps/TODO-12-screenshot-archive.md` -- the snipping tool annotates a captured image; Inkeys draws live over the running desktop and on a whiteboard. Complement, no shared code

---

## Outcome

`C:\Program Files\Inkeys\Inkeys.exe` starts from the Start menu, puts its toolbar over the desktop, and draws smooth pen, touch and mouse strokes over any window or on a full-screen whiteboard, built from Inkeys' own source in this tree. A newer Inkeys arrives by re-importing a pinned commit and passing the §6 conformance suite.

---

## Implementation Order

| Step | Section                             | 💎/⭐ | Dependency            |
| ---- | ----------------------------------- | ----- | --------------------- |
| 1    | Inkeys Source Import and Provenance | ⭐    | --                    |
| 2    | Inkeys API Inventory                | ⭐    | §1, D12 T07 §1        |
| 3    | Inkeys Compatibility Probes         | 💎    | §2, D12 T07 §8,§9,§13 |
| 4    | Inkeys Build for Impossible OS      | 💎    | §1, D11 T05 §11       |
| 5    | Inkeys App on Impossible OS         | 💎    | §3, §4, D10 T12 §4    |
| 6    | Inkeys Conformance and Performance  | ⭐    | §5                    |

---

## 1. Inkeys Source Import and Provenance

> **Spawned-by:** root

**Design:** n/a -- source import and licence records, it draws nothing

Inkeys is imported as source, never forked: the tree under `src/apps/inkeys/` always equals a pinned upstream commit, and every change Inkeys needs is made upstream and re-imported.

- [ ] Import Inkeys at a pinned commit under `src/apps/inkeys/` with `scripts/vendor-inkeys.sh <commit>`, including the `HiMsg` submodule at the commit the superproject records
  - Copies no `Vcpkg/` checkout, build output, `.trellis/` tooling or binary over 1 MiB
- [ ] Audit every component in the upstream third-party table at that commit by its LICENSE file, and record the verdict per component in `src/libs/PROVENANCE.md`
- [ ] Exclude what cannot be redistributed under GPL-3.0, each named in the import script with its reason
  - The Microsoft DirectX SDK (June 2010) headers, Zip Utils (no licence file), `DesktopDrawpadBlocker` (no licence file), and any component the audit cannot confirm
- [ ] Map each vcpkg port Inkeys uses to an existing `src/libs/` vendor, a new vendor import with its own rows, or an Inkeys build option that compiles the feature out (decided in §4)
- [ ] Review the imported tree before it lands: a Claude read of the imported code plus the Astra passes of `.conclave/reviewers.toml`, and every re-import is reviewed the same way
  - Operator direction 2026-11-23: all imported code is reviewed, not only the integration seam
- [ ] Add the Inkeys row to `src/libs/PROVENANCE.md` and its notice set to `CREDITS.md` in the same commit
- [ ] `scripts/vendor-inkeys.sh --check` refuses a tree that differs from the pinned commit, wired into `scripts/lint.sh` for staged files under `src/apps/inkeys/`
- [ ] Write the update procedure in `docs/apps/inkeys.md`: bump the commit, re-run the import, rebuild, run §6
- [ ] Commit: `"apps: import Inkeys source at a pinned commit"`

**Test checkpoint:** `bash scripts/vendor-inkeys.sh --check` prints `inkeys: tree matches <commit>` and exits 0; editing one imported file makes it exit 1 naming the file. `git check-ignore` reports none of the imported files as ignored, and no excluded component is present in the tree.

## 2. Inkeys API Inventory

> **Spawned-by:** root

**Design:** n/a -- a generated data file and report, it draws nothing

The list of what Impossible OS must provide, measured from Inkeys itself rather than guessed.

- [ ] `tools/app-api-inventory/`: a host tool that lists a PE's imports (`llvm-objdump -p`) and the COM and DirectX objects its source creates, and writes `docs/apps/inkeys-api.json`
  - Run against Inkeys' official x64 release binary at the pinned commit and against the imported source
- [ ] Classify every entry against the D12 T07 §1 coverage tracker as implemented, stubbed or missing, with its owning section
- [ ] File each missing API with no owning section in the domain-correct TODO, with a reciprocal XREF, so the ladder in §3 has an owner for every rung
- [ ] Mark the Windows-host-only integrations (PowerPoint bridge, toast notifications, updater) as compiled out by §4, not as OS work
- [ ] Commit: `"apps: Inkeys API inventory"`

**Test checkpoint:** `python3 tools/app-api-inventory/inventory.py --check docs/apps/inkeys-api.json` exits 0 and reports a count of implemented, stubbed and missing entries that matches the tracker; an entry with no owner fails the check.

## 3. Inkeys Compatibility Probes

> **Spawned-by:** root

**Design:** n/a -- test programs, their output is compared with Windows 11, not designed

Small programs, each one rung of what Inkeys does, run first on Windows 11 for the expected result and then on Impossible OS. The ladder follows the inventory of §2.

- [ ] `sdk/compat/inkeys/draw_gdi.c`: a window that draws mouse strokes with GDI (`WM_MOUSEMOVE`, `Polyline`)
- [ ] `sdk/compat/inkeys/layered.c`: a topmost per-pixel-alpha layered window over the desktop through `UpdateLayeredWindowIndirect`, click-through on and off (`WS_EX_TRANSPARENT`)
- [ ] `sdk/compat/inkeys/d2d_toolbar.c`: a Direct2D 1.1 device context drawing a rounded, anti-aliased toolbar into a layered window
- [ ] `sdk/compat/inkeys/d3d11_canvas.c`: a Direct3D 11 device and swap chain drawing strokes with a compiled HLSL shader, presented through DirectComposition
- [ ] `sdk/compat/inkeys/pointer.c`: `WM_POINTER*` pen pressure and touch contacts logged in order
- [ ] Each probe prints `inkeys-probe <name>: passed` and exits 0 on Windows 11 and on Impossible OS, and is added to the D12 T07 §12 compat gate as soon as it passes here
- [ ] Commit: `"compat: Inkeys probe ladder"`

**Test checkpoint:** `bash scripts/compat-check.sh` runs the five probes; on the day each rung's APIs land, that probe's line flips from `missing` to `passed`. Test on: QEMU TCG and KVM at 1 and 2 CPUs.

## 4. Inkeys Build for Impossible OS

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- a build, it draws nothing

Inkeys is built from the imported tree with the project toolchain. Decision: clang-19 for the Windows PE32+ x64 target against the Impossible OS Win32 headers and import libraries, with the C++ runtime of D11 T05 §11. Reversal cost: medium; an MSVC-built binary is still run as a compatibility test in §6. The Need is upstream: the build options below are added in the Inkeys repository, coordinated with its author, and arrive through §1.

- [ ] Upstream build option that compiles out the Windows-host-only integrations named in §2 (PowerPoint bridge, toast notifications, updater), with the Windows build unchanged when it is off
- [ ] Upstream build option for each vcpkg dependency §1 maps to an Impossible OS vendor, so the in-tree build links the system copy
- [ ] `scripts/build.sh` builds `src/apps/inkeys/` into `build/apps/inkeys/Inkeys.exe`, with its HLSL shaders compiled at build time by the shader compiler Tier 9 supplies
- [ ] The imported tree builds with zero local patches (`scripts/vendor-inkeys.sh --check` still passes after the build)
- [ ] Commit: `"apps: build Inkeys for Impossible OS"`

**Test checkpoint:** `bash scripts/build.sh` ends with `=== BUILD OK ===` and `build/apps/inkeys/Inkeys.exe` exists; `llvm-objdump -p` on it shows no import outside `docs/apps/inkeys-api.json`.

## 5. Inkeys App on Impossible OS

> **Spawned-by:** root

**Design:** n/a -- Inkeys draws its own interface; Impossible OS supplies the window, input and composition it runs in

- [ ] Ship Inkeys in the image at `C:\Program Files\Inkeys\` with a Start menu entry
- [ ] The toolbar appears over the desktop, stays topmost, and passes clicks through to the windows below when drawing is off
- [ ] Mouse, touch and pen strokes draw over a running window; pen pressure changes stroke width
- [ ] Whiteboard mode fills the screen and returns to the desktop on exit
- [ ] The settings window (Dear ImGui and ImFluent) opens, renders and saves its settings
- [ ] A fault inside Inkeys ends Inkeys only and is logged with its module and offset
- [ ] Commit: `"apps: Inkeys runs on Impossible OS"`

**Test checkpoint:** the serial log contains `inkeys: started` and, after a scripted stroke, `inkeys: stroke committed`; the desktop stays responsive throughout. Test on: QEMU TCG and KVM at 1 and 2 CPUs, QEMU WHPX.

## 6. Inkeys Conformance and Performance

> **Spawned-by:** root

**Design:** n/a -- a test suite, it draws nothing

- [ ] `scripts/test-inkeys.sh`: a scripted session under the headless compositor (start, draw 100 strokes, undo, whiteboard, settings, exit) with a frame-time and input-to-pixel latency record
- [ ] Run Inkeys' official MSVC-built x64 release binary at the pinned commit through the same script, as the unmodified-binary compatibility check
- [ ] Wire the suite into the smoke matrix as one leg once §5 ships
- [ ] Commit: `"test: Inkeys conformance suite"`

**Test checkpoint:** `bash scripts/test-inkeys.sh` ends with `inkeys: conformance OK` and exits 0, recording median and 99th-percentile frame times; removing `UpdateLayeredWindowIndirect` makes it fail at the toolbar step.

---

## OS Comparison

| ⭐  | Feature                                 | 🪟 Win11                      | 🐧 Linux                 | 🚀 Impossible OS |
| --- | --------------------------------------- | ----------------------------- | ------------------------ | ---------------- |
| 💎  | Draw live over the desktop              | ⚠️ Snipping Tool Quick markup | ⚠️ Gromit-MPX, not inbox | ⬜ Planned -- §5 |
| ⭐  | Full-screen whiteboard, inbox           | ❌ Separate Whiteboard app    | ❌ Not inbox             | ⬜ Planned -- §5 |
| 💎  | Pen pressure and touch strokes          | ✅ Windows Ink                | ⚠️ Per toolkit           | ⬜ Planned -- §5 |
| ⭐  | Toolbar for touch boards and desktops   | ❌ Not inbox                  | ❌ Not inbox             | ⬜ Planned -- §5 |
| ⭐  | Bundled third-party app, measured by CI | ❌ No                         | ❌ No                    | ⬜ Planned -- §6 |

> **After §1-§2:** Inkeys' source is in the tree and the OS knows exactly which APIs it needs. **After §3-§5:** Impossible OS ships live screen annotation and a whiteboard inbox, which Windows 11 offers only as a markup step of the Snipping Tool and Linux only as an add-on. **After §6:** every Inkeys update and every OS change is measured against the same session.

---

## Unit Tests

> Inkeys' own tests and the §3 probes are the unit tests: they run on Windows 11 and inside the OS. No kernel test category is needed.

- [ ] Inkeys' automated test suite at the pinned commit builds and passes on Impossible OS under the user-mode test launcher
- [ ] The §3 probes pass on Impossible OS for every rung whose APIs have shipped
- [ ] Commit: `"test: Inkeys suites on Impossible OS"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`, with Inkeys built
- [ ] `bash scripts/vendor-inkeys.sh --check` exits 0
- [ ] `bash scripts/test-inkeys.sh` ends with `inkeys: conformance OK`
- [ ] `bash scripts/test-smoke-matrix.sh` passes with the Inkeys leg; the serial log contains `inkeys: started`
- [ ] QEMU WHPX at 2 CPUs and VirtualBox: a 5-minute drawing session over a running window shows no fault in the serial log
- [ ] Bare metal: pen and touch drawing on a touch display holds the frame budget on the software compositor (VM behaviour differs)
