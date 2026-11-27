---
schema_version: 1
id: isotone
domain: 11-apps
status: active
title: "TODO-17 -- Isotone"
---

# TODO-17 -- Isotone

> **Goal:** Bundle Isotone (`github.com/rizonesoft/Isotone`, public) as a permanent app of Impossible OS. Isotone runs as an Impossible SDK program: it is built from imported source with the pinned SDK toolchain, calls the operating system only through the functions listed in the SDK's `abi/tiers.json`, and has no backend of its own on this OS. Its canvas draws on the SDK's GPU tier with a CPU path, and its interface is `isotone::ui` drawing on the SDK. Impossible OS imports its source at a pinned commit, builds it, and compares each executable it built with the hash the Isotone release recorded for it.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-27, against Isotone at `ff1a42e`. Nothing in this file is built. Isotone has a C++23 foundation (`isotone::core`, `isotone::ui`) built with MSVC and clang-cl with the static runtime, and no DirectX code yet; its stack design plans Win32, Direct2D, DirectWrite and Direct3D. Under the approved design (`impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md`), Isotone moves `isotone::core` and `isotone::ui` onto Impossible SDK and puts its canvas on the `gpu` tier, so new drawing starts on the SDK. Impossible OS user mode has a C library and no C++ runtime (the SDK's `cxx` part arrives through `11-apps/TODO-15-imported-programs.md` §22) and no Direct3D 11 (`10-platform-services/TODO-13-program-needs.md` §7 and §8). So §1 and §2 are ours now, §3 waits for Isotone's move onto the SDK, and §4 and §5 follow.

> [!NOTE]
> **Owner direction, 2026-11-22:** Isotone is a permanent part of Impossible OS, like Recto, Inkeys and Office. Each is made to work on Windows first, then Impossible OS is made compatible with it. **Owner decisions, 2026-11-27:** one shared seam, built into Impossible SDK; apps that need the GPU get a Direct3D 11 tier in the SDK; no double work, so Impossible OS builds no per-app backend and no drawing library, text engine, control set or C++ runtime for Isotone. **Approach decision (this file):** Isotone runs as an Impossible SDK program built from imported source with the pinned SDK toolchain; it gets no Impossible OS backend, and its build is reproducible, so what this repository builds is the executable the release shipped. Why: every call Isotone makes is a listed SDK call, each listed call is a roadmap section here, and the GPU work is shared with Inkeys' Direct3D 11 needs. Reversal cost: medium; a backend would add a section between §3 and §4. **Vendor verdict:** Isotone's licence is read from its `LICENSE` at the pinned commit by §1 and recorded before anything is copied.

---

## Inputs

- `github.com/rizonesoft/Isotone` -- the source, its `sdk.json`, `app.json`, stack design and release record -- imported by §1
- `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md` -- the approved design: the boundary rules, the `cxx` part, the GPU tier and who builds what
- -> XREF: `11-apps/TODO-15-imported-programs.md §1,§2,§22,§23` -- the SDK import, its parts, the `cxx` part here and the reproducible build check this file's §3 uses; one copy of each, never repeated in this file
- -> XREF: `10-platform-services/TODO-13-program-needs.md §7,§8,§9` -- the GPU tier: the Direct3D 11 and DXGI functions, resolved names and methods the canvas calls (§4)
- -> XREF: `11-apps/TODO-14-inkeys.md §3,§5` -- Inkeys' Direct3D 11 canvas needs the same device, swap chain, buffers, textures, shaders and draw calls; the GPU sections are shared, never filed twice
- -> XREF: `10-platform-services/TODO-13-program-needs.md §1,§2,§3` -- the Windows functions of tiers 0 to 2
- -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §16` -- per-thread vector state, which must be in place before Isotone runs threaded here (§4)
- `src/libs/PROVENANCE.md`, `CREDITS.md` -- vendoring records (§1)

---

## Outcome

`C:\Program Files\Isotone\Isotone.exe` starts from the Start menu and opens its canvas, built here with the SDK toolchain from Isotone's own source. The canvas draws on the GPU tier when Impossible OS provides it and on the CPU when it does not. The SDK's check refuses a build that calls anything not on the list, and the hash of each executable equals the one the Isotone release recorded. A newer Isotone arrives by re-importing a pinned commit and passing §5.

---

## Implementation Order

| Step | Section                              | 💎/⭐ | Dependency                        |
| ---- | ------------------------------------ | ----- | --------------------------------- |
| 1    | Isotone Source Import and Provenance | ⭐    | --                                |
| 2    | Isotone Boundary Check on Import     | ⭐    | §1, D11 T15 §1                    |
| 3    | Isotone Build with the SDK Toolchain | 💎    | §2, D11 T15 §2,§22,§23            |
| 4    | Isotone App on Impossible OS         | 💎    | §3, D10 T13 §1,§2,§3, D03 T06 §16 |
| 5    | Isotone Canvas on the GPU Tier       | 💎    | §4, D10 T13 §7,§8                 |
| 6    | Isotone Conformance and Performance  | ⭐    | §4, §5                            |

---

## 1. Isotone Source Import and Provenance

> **Spawned-by:** root

**Design:** n/a -- source import and licence records, it draws nothing

**Import:** isotone

Isotone is developed in its own repository and proved on Windows first. It is imported as source, never forked: the tree under `src/apps/isotone/` always equals a pinned upstream commit, and every change Isotone needs is made upstream and re-imported.

- [ ] Read Isotone's `LICENSE` and every third-party notice at the commit to pin, and record the verdict per component in `src/libs/PROVENANCE.md` before anything is copied
  - Exclude what cannot be redistributed under this project's GPL-3.0-only, each exclusion named with its reason
- [ ] Confirm the `from` list of the `isotone` source in `todo/imports.json` against Isotone's top level at that commit (the ledger entry has `commit` null until the first pull, so it may still be corrected)
- [ ] Copy the pinned program with `python3 scripts/imports.py --pull isotone` into `src/apps/isotone/`
- [ ] Review the imported tree before it lands: a Claude read of the imported code plus the Astra passes of `.conclave/reviewers.toml`, and every re-import is reviewed the same way
  - Operator direction 2026-11-23: all imported code is reviewed, not only the integration seam
- [ ] Add the Isotone row to `src/libs/PROVENANCE.md` and its notice set to `CREDITS.md` in the same commit
- [ ] Write the update procedure in `docs/apps/isotone.md`: bump the commit with `--pull isotone --to <commit>`, rebuild, run §6
- [ ] Commit: `"apps: Isotone imported"`

**Test checkpoint:** `python3 scripts/imports.py --check` exits 0 with `isotone` pinned, and `python3 scripts/imports.py --list` shows its commit; editing one imported file without proposing it makes the check exit 1 naming the file.

## 2. Isotone Boundary Check on Import

> **Spawned-by:** root

**Design:** n/a -- a check over source and a built file, it draws nothing

An Isotone program may call Windows only through Impossible SDK. The SDK checks that in Isotone's own CI; this section runs the same checks again when Isotone is imported, with the SDK's own tools, and writes no second checker.

- [ ] Run the SDK's source check (no Windows, COM or DirectX header in Isotone or its dependencies) over `src/apps/isotone/` on import, using the tool imported by `11-apps/TODO-15-imported-programs.md` §1
- [ ] Measure the tier of Isotone's programs with the SDK's effective-tier calculation (imports, plus the tiers its linked parts declare in `parts.json`, the `gpu` part declaring the GPU tier) and list every Windows function it can reach
- [ ] File each listed function that no roadmap section answers (`python3 scripts/imports.py --needs` names it) in `10-platform-services/TODO-13-program-needs.md` or the file that owns the subject, with a reciprocal XREF, so every call has an owner
- [ ] Fail the import on a call outside the list, never on a missing OS feature: a missing OS feature is a section, not a refusal
- [ ] Commit: `"apps: Isotone boundary check on import"`

**Test checkpoint:** `bash scripts/test-isotone.sh --boundary` prints `isotone: boundary OK (tier N)` and exits 0; a planted source file that includes `<windows.h>` makes it exit 1 naming that file.

## 3. Isotone Build with the SDK Toolchain

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- a build, it draws nothing

Isotone is built from the imported tree with the pinned SDK toolchain (`toolchain.json` of the imported SDK), the same one Isotone's release used, with the C++ runtime of the SDK's `cxx` part. The Need is upstream: Isotone moves `isotone::core` and `isotone::ui` onto the SDK, and its shaders are compiled at build time into the bytecode format the tier fixes, in the Isotone repository; both arrive through §1.

- [ ] `scripts/build.sh` builds `src/apps/isotone/` with the SDK toolchain into `build/apps/isotone/`, linking no prebuilt object or archive and no system import library
- [ ] Compare each executable built here with the hash the Isotone release recorded for it, through the shared check of `11-apps/TODO-15-imported-programs.md` §23; a mismatch fails the build
- [ ] Pass the SDK's import check on every executable against its effective tier, as the last word
- [ ] The imported tree builds with zero local patches (`python3 scripts/imports.py --check` still passes after the build)
- [ ] Commit: `"apps: Isotone built with the SDK toolchain"`

**Test checkpoint:** `bash scripts/build.sh` ends with `=== BUILD OK ===`; the log prints `isotone: <exe> hash matches the release` for each executable; changing one byte of an imported source file makes the hash comparison fail.

## 4. Isotone App on Impossible OS

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- Isotone draws its own interface with `isotone::ui` on the SDK; Impossible OS supplies the window, input and files it runs on

This section runs Isotone on its CPU path, which every GPU feature has; §5 adds the GPU surface.

- [ ] Ship Isotone in the image at `C:\Program Files\Isotone\` with a Start menu entry
- [ ] The window opens, draws its interface and canvas on the CPU path, and takes mouse and keyboard input
- [ ] Open, edit, save and reopen a document, with the save atomic (a simulated power cut leaves the original intact)
- [ ] Threaded work runs only after per-thread vector state is in place: `03-memory-concurrency/TODO-06-scheduler-enhancement.md` §16 is done and its SSE2 isolation test passes
- [ ] A fault inside Isotone ends Isotone only and is logged with its module and offset
- [ ] Commit: `"apps: Isotone runs on Impossible OS"`

**Test checkpoint:** the serial log contains `isotone: started` and, after a scripted stroke, `isotone: stroke committed`; the desktop stays responsive throughout. Test on: QEMU TCG and KVM at 1 and 2 CPUs, QEMU WHPX.

## 5. Isotone Canvas on the GPU Tier

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- the canvas is Isotone's own; Impossible OS supplies the Direct3D 11 subset it presents through

- [ ] The `gpu` part finds the Direct3D 11 and DXGI calls of the tier on this OS and the canvas draws on them (`10-platform-services/TODO-13-program-needs.md` §7 and §8 supply them)
- [ ] With the GPU absent or failing at start-up, the same executable reaches the same canvas on the CPU path, reported by `im_gpu_open` and never as a crash
- [ ] A lost device while a canvas is open (presenting, resizing the swap chain, creating a resource) returns the lost status, and the canvas recreates the device or carries on with its CPU path
- [ ] Commit: `"apps: Isotone canvas on the GPU tier"`

**Test checkpoint:** the serial log contains `isotone: canvas gpu` with the GPU path and `isotone: canvas cpu` when the device is refused; both paths produce the same canvas picture within Isotone's tolerance. Test on: QEMU TCG and KVM; the GPU leg on the software rasterizer first.

## 6. Isotone Conformance and Performance

> **Spawned-by:** root

**Design:** n/a -- a test suite, it draws nothing

- [ ] `scripts/test-isotone.sh`: a scripted session under the headless compositor (start, draw 100 strokes, undo, save, close) with a frame-time record, comparing pictures and timings separately, before and after, on the same machine
- [ ] Isotone's canvas target, measured through presentation: panning a 100-megapixel document at 37 degrees with a median frame under 16 ms
  - The target is set in `docs/parity/gesso-section-design.md` in Isotone; work the CPU cannot do within it goes to the `gpu` tier
- [ ] A move or an import lands only when its pictures match within Isotone's own tolerance and its timings are no worse than Isotone's own targets
- [ ] Wire the suite into the smoke matrix as one leg once §4 ships
- [ ] Commit: `"test: Isotone conformance suite"`

**Test checkpoint:** `bash scripts/test-isotone.sh` ends with `isotone: conformance OK` and exits 0, recording median and 99th-percentile frame times; breaking the file read of the platform layer makes it fail at the first document open.

---

## OS Comparison

| ⭐  | Feature                                | 🪟 Win11            | 🐧 Linux            | 🚀 Impossible OS |
| --- | -------------------------------------- | ------------------- | ------------------- | ---------------- |
| 💎  | Layered canvas app in the image        | ⚠️ Paint, no layers | ❌ Not inbox        | ⬜ Planned -- §4 |
| ⭐  | Same source and hash on Windows and OS | ❌ Separate builds  | ❌ Per distro build | ⬜ Planned -- §3 |
| ⭐  | GPU canvas with a CPU path             | ⚠️ Per app          | ⚠️ Per toolkit      | ⬜ Planned -- §5 |
| ⭐  | Bundled app measured by CI             | ❌ No               | ❌ No               | ⬜ Planned -- §6 |

> **After §1-§2:** Isotone's source is in the tree and every call it makes is checked against the list. **After §3-§4:** Impossible OS ships Isotone built from the same source and toolchain as the Windows release, on the CPU path. **After §5-§6:** the canvas runs on the GPU tier, and every Isotone update and every OS change is measured against the same session.

---

## Unit Tests

> Isotone's own tests at the pinned commit are the unit tests: they run on Windows first and inside the OS. No kernel test category is needed.

- [ ] Isotone's automated test suite at the pinned commit builds and passes on Impossible OS under the user-mode test launcher
- [ ] Commit: `"test: Isotone suites on Impossible OS"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`, with Isotone built and every hash matching
- [ ] `python3 scripts/imports.py --check` exits 0
- [ ] `bash scripts/test-isotone.sh` ends with `isotone: conformance OK`
- [ ] `bash scripts/test-smoke-matrix.sh` passes with the Isotone leg; the serial log contains `isotone: started`
- [ ] QEMU WHPX at 2 CPUs and VirtualBox: a 5-minute drawing session shows no fault in the serial log
