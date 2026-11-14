---
schema_version: 1
id: pdf-viewer
domain: 11-apps
status: active
title: "TODO-05 -- PDF Viewer"
---

# TODO-05 -- PDF Viewer

> **Goal:** Bundle Recto as the PDF app of Impossible OS. Recto is the operator's own open-source PDF viewer, OCR tool and document converter (C++23, GPL-3.0-or-later), with its own engine (`recto-pdf`) and toolkit (`recto-ui`). Instead of writing a second, smaller PDF parser and viewer here, Impossible OS imports Recto's source at a pinned commit, gives it a C++23 runtime and a platform backend on the Impossible OS UI engines, and registers it as the system handler for `.pdf`. The in-house `pdfview.exe` engine and window this file used to plan (§1 to §8) are superseded by §10 to §20.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-14. Nothing in this file is built. Recto itself is at milestone M0 (repository and build scaffold, no application functionality yet), and it targets Windows 11 directly: Direct2D, DirectWrite, DirectComposition and `Windows.Media.Ocr`. Impossible OS user mode has a C library and no C++ runtime. So the first work here is ours (§10 import, §11 runtime), and the rest follows Recto's own milestones through its platform seam (§12).

> [!NOTE]
> **Owner direction, 2026-11-14:** Recto is bundled in Impossible OS, and it is ported with a backend, not run as an unmodified Windows binary: its toolkit draws through the Impossible OS graphics, text and UI engines, and it is built from source in this tree. Reversal cost: low; the unmodified binary remains a later Win32 compatibility test and needs no change here. Vendor verdict: Recto is GPL-3.0-or-later (same licence family as this project, read at `D:\Recto\LICENSE`); the C++ runtime is LLVM libc++ (Apache-2.0 with the LLVM exception).

---

## Inputs

- `D:\Recto` (operator's host) -- Recto's repository: `src/core`, `src/pdf`, `src/ui`, `src/app`, `src/cli`, `design/SPEC.md`, `docs/plans/2026-10-13-recto-foundation.md` -- imported by §10
- -> XREF: `08-graphics-ui/TODO-18-ui-framework-engines.md §6,§7,§12,§19,§20,§24` -- the engines Recto's toolkit backend and system integration are built on (§14, §18)
- -> XREF: `08-graphics-ui/TODO-19-fluent-component-library.md §17,§22` -- the tab view and title bar Recto's window uses (§15)
- -> XREF: `07-networking/TODO-10-pdf-viewer.md §9` -- PDFs opened from the network are handed to Recto (§18)
- -> XREF: `10-platform-services/TODO-09-compiler-sdk.md §11` -- on-device C++ compilers stay there; the C++ runtime for bundled apps is §11 here
- `02-kernel-core/TODO-03-kernel-libraries.md §4` -- `mz_uncompress(dst, &dst_len, src, src_len)` (miniz FlateDecode)
- `include/kernel/image.h` -- `image_load_mem(img, data, size)` -- §5 image rendering
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_draw_char()`, `ttf_measure_width()` -- §4 text rendering
- `include/gfx.h` -- `gfx_surface_create()`, `gfx_fill_rect()`, `gfx_blit()`, `gfx_drop_shadow()`, `gfx_scale_blit()` (planned; not in `gfx.h` yet) -- §4–§6
- `include/desktop/controls.h` -- `CTRL_SCROLLBAR` (`CTRL_SCROLLBAR_VERT`/`CTRL_SCROLLBAR_HORIZ`), `CTRL_TEXTBOX`, `CTRL_BUTTON` -- §6
- `include/desktop/wm.h` -- `wm_create_window()` -- §6
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, app_path)` -- §9
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- §1 file loading
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free_contiguous()` -- all large buffers

---

## Outcome

`C:\Program Files\Recto\Recto.exe file.pdf` opens a PDF in Recto, built from Recto's own source for Impossible OS: its engine renders the pages, its toolkit draws through the Impossible OS UI engines, and it is the system's handler for `.pdf` (open, thumbnail, preview, properties and search). Recto's later features (OCR, convert, organize) arrive by re-importing a newer pinned commit and passing the conformance suite of §20.

---

## Implementation Order

| Step | Section                                   | 💎/⭐ | Dependency                             |
| ---- | ----------------------------------------- | ----- | -------------------------------------- |
| 1    | PDF Structure Parser                      | 💎    | VFS file load                          |
| 2    | Page Tree Traversal                       | 💎    | §1 object resolver                     |
| 3    | Stream Decompression                      | 💎    | §2 stream refs, miniz `D02T03 §4`      |
| 4    | Content Stream Renderer                   | 💎    | §3 decompressed streams, TTF font_mgr  |
| 5    | Image Rendering                           | 💎    | §3, `image_load_mem`                   |
| 6    | PDF Viewer UI                             | 💎    | §4 + §5 page surface, controls.h, wm.h |
| 7    | Text Search                               | ⭐    | §4 text operator cache                 |
| 8    | Continuous Scroll View (Stretch)          | ⭐    | §6 stable UI                           |
| 9    | File Association                          | 💎    | §6 app exists, TODO-02 §1              |
| 10   | Recto Source Import and Provenance        | ⭐    | --                                     |
| 11   | C++23 User-Mode Runtime                   | 💎    | --                                     |
| 12   | Recto Platform Seam                       | ⭐    | §10, §11                               |
| 13   | Recto PDF Engine on Impossible OS         | 💎    | §12                                    |
| 14   | Recto UI Backend on Impossible OS Engines | ⭐    | §12, D08 T18 §6,§7,§12,§24             |
| 15   | Recto App on Impossible OS                | 💎    | §13, §14, D08 T19 §17,§22              |
| 16   | OCR Provider                              | 💎    | §13                                    |
| 17   | Convert and Organize Features             | 💎    | §15                                    |
| 18   | System Integration                        | 💎    | §15, D08 T18 §19,§20                   |
| 19   | Packaging and Updates                     | 💎    | §15                                    |
| 20   | Recto Conformance on Impossible OS        | ⭐    | §13, §15                               |

---

## 1. PDF Structure Parser

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

---

## 2. Page Tree Traversal

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

---

## 3. Stream Decompression

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

---

## 4. Content Stream Renderer

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

---

## 5. Image Rendering

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

---

## 6. PDF Viewer UI

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §15 (item: "Build the Recto app for Impossible OS from src/app with the engine" at line 260)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §15; do not build this here

---

## 7. Text Search

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §15 (item: "Build the Recto app for Impossible OS from src/app with the engine" at line 260)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §15; do not build this here

---

## 8. Continuous Scroll View (Stretch)

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §15 (item: "Build the Recto app for Impossible OS from src/app with the engine" at line 260)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §15; do not build this here

---

## 9. File Association

> **Changed 2026-11-14:** the registered handler is `C:\Program Files\Recto\Recto.exe` (§15, §18), not `pdfview.exe`. Where an item below names `pdfview.exe`, register Recto.

**Source file:** registered at `pdfview_ui_init()`

- [ ] `file_assoc_set(".pdf", "ImpossibleOS.PDFViewer", "C:\\Impossible\\System32\\pdfview.exe")`
- [ ] Command-line: `pdfview.exe C:\path\to\file.pdf` -- open and display immediately
- [ ] `pdfview.exe` without args → open-file dialog (`dialog_file_open()`) filtered to `*.pdf`
- [ ] Open from File Manager double-click → `file_assoc_open()` routes to `pdfview.exe` with path arg
- [ ] Open from browser download (→ XREF `11-apps/TODO-01 §8`) → same launch path
- [ ] Open from email attachment (→ XREF `11-apps/TODO-04 §4`) → viewer window per attachment click
- [ ] Recent files: `HKCU\Software\Impossible\PDFViewer\RecentFiles` (up to 10 paths, MRU order)

---

## 10. Recto Source Import and Provenance

> **Spawned-by:** root

**Design:** n/a -- source import and licence records, it draws nothing

Recto (`D:\Recto` on the operator's host, the operator's own project, GPL-3.0-or-later) becomes the system PDF app. It is imported as source, never forked: every change Impossible OS needs is made in Recto behind its platform seam (§12) and re-imported.

- [ ] Import Recto at a pinned commit under `src/apps/recto/` with `scripts/vendor-recto.sh <commit>`
  - Copies `src/core`, `src/pdf`, `src/ui`, `src/services`, `src/app`, `src/cli`, `resources` and `LICENSE`
  - Copies nothing from `build/`, `vcpkg_installed/` or `tests/` fixtures over 1 MiB
- [ ] Read Recto's `LICENSE` and `NOTICE.md` at that commit and record the result: add the row to `src/libs/PROVENANCE.md` (upstream path, commit, licence, import command) and to `CREDITS.md` in the same commit
- [ ] Record every third-party notice Recto itself carries (its `NOTICE.md`) in `CREDITS.md`, so the redistributed image carries the full notice set
- [ ] Write the update procedure in `docs/apps/pdf-viewer.md`: bump the commit, re-run the import, rebuild, run §20; a local edit under `src/apps/recto/` is refused by `scripts/vendor-recto.sh --check` (the tree must equal the pinned commit)
- [ ] Add `scripts/vendor-recto.sh --check` to `scripts/lint.sh` for staged files under `src/apps/recto/`
- [ ] Commit: `"apps: import Recto source at a pinned commit"`

**Test checkpoint:** `bash scripts/vendor-recto.sh --check` prints `recto: tree matches <commit>` and exits 0; editing one imported file makes it exit 1 naming the file. `git check-ignore` reports none of the imported files as ignored.

## 11. C++23 User-Mode Runtime

> **Spawned-by:** root

**Design:** n/a -- a language runtime, it draws nothing

Recto is C++23. Impossible OS user mode has a C library only, so the C++ runtime comes first. Vendor verdict: LLVM libc++, libc++abi and libunwind (Apache-2.0 with the LLVM exception, compatible with GPL-3.0; read each `LICENSE.TXT` before importing), built with the existing clang-19 toolchain.

- [ ] Vendor libc++, libc++abi and libunwind at the LLVM 19 release tag under `src/libs/llvm-runtimes/`, with `PROVENANCE.md` and `CREDITS.md` rows in the same commit
- [ ] Build them for the user-mode target as static libraries: exceptions and RTTI on, threads on the native thread API, no locale beyond "C" until the locale services land, no filesystem until §12 supplies it
- [ ] Supply the C library pieces libc++ needs and user libc lacks, each as a named function with a test
  - `aligned_alloc`, the `strtod` family, `wcs*` basics, `clock_gettime`, and `pthread`-shaped mutex, condition variable and thread-local shims over native primitives
- [ ] Wire stack unwinding to the image's unwind tables, building on the user-mode exception dispatch of `02-kernel-core/TODO-23-exception-dispatch-seh.md`
- [ ] Add `scripts/build.sh` support for user-mode C++ translation units and a `user/tests/cxx/` suite: exceptions across frames, `std::thread` with `std::mutex`, `std::string`, `std::vector`, `std::format`, `std::expected`
- [ ] Record the supported subset in `docs/sdk/cxx-runtime.md` (new docs page section) and cross-reference it from `10-platform-services/TODO-09-compiler-sdk.md` §11, which keeps on-device compilers
- [ ] Commit: `"sdk: C++23 user-mode runtime (libc++, libc++abi, libunwind)"`

**Test checkpoint:** `user/tests/cxx/test_cxx.exe` prints `cxx: 6 of 6 passed` under the user-mode test launcher; a thrown exception caught two frames up returns the expected value. Test on: QEMU TCG and KVM at 1 and 2 CPUs.

## 12. Recto Platform Seam

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- an interface between Recto and its host, it draws nothing

Recto's engine and toolkit call Windows directly today. The seam is one interface, defined in Recto, with a Windows implementation and an Impossible OS implementation. The Need is Recto itself: the seam is authored in the Recto repository and arrives through §10.

- [ ] In Recto, define `recto-platform`: files and memory mapping, atomic save, threads and the render pool, clock, clipboard, settings store, file dialogs, printing, and the drawing, text and input interfaces `recto-ui` calls
- [ ] In Recto, move every direct Windows call in `recto-core`, `recto-pdf` and `recto-ui` behind that interface, with the existing Direct2D, DirectWrite and DirectComposition code as the Windows implementation
- [ ] In Impossible OS, write `src/apps/recto-platform-ios/`: the same interface over the user-mode file, thread, clipboard and settings APIs
- [ ] Add a build check that the imported `recto-core`, `recto-pdf` and `recto-ui` include no Windows header outside the Windows implementation folder (`scripts/vendor-recto.sh --seam-check`)
- [ ] Commit: `"apps: Recto platform seam for Impossible OS"`

**Test checkpoint:** `bash scripts/vendor-recto.sh --seam-check` prints `recto seam: 0 Windows includes outside platform/windows` and exits 0. The platform implementation's unit tests pass under the user-mode launcher.

## 13. Recto PDF Engine on Impossible OS

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- a document engine, it draws nothing by itself

`recto-pdf` and `recto-core` built for Impossible OS replace the in-house parser, decompressor and renderer this file used to plan. The Need is Recto's engine milestone (M1): until it exists upstream there is nothing to build.

- [ ] Build recto-core and recto-pdf as static libraries for the user-mode target with clang-19 and the runtime of §11
- [ ] Run Recto's engine unit tests on the host (Linux build of the engine with the seam's host implementation) and inside the OS under the user-mode launcher
- [ ] Run Recto's fuzz corpus (`fuzz/`) against the Impossible OS build on the host with ASan and UBSan through `tools/fuzz/run.sh`, adding one target per Recto fuzz entry point
- [ ] Render the conformance set (Recto's test PDFs) to bitmaps on Impossible OS and on Windows and compare within Recto's own tolerance
- [ ] Bound memory: the engine's caches take their budget from the seam, and an oversized or malicious file fails with an error and never exhausts the process
- [ ] Commit: `"apps: Recto PDF engine built and tested on Impossible OS"`

**Test checkpoint:** the user-mode launcher prints `recto-pdf: N of N tests passed`; the fuzz targets run 10 s each with no finding; a 500-page test file opens and renders page 250 in under Recto's stated target. Test on: QEMU TCG and KVM.

## 14. Recto UI Backend on Impossible OS Engines

> **Spawned-by:** root

**Needs:** third-party

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`shell.md#materials`](../../docs/design/shell.md#materials)

`recto-ui` draws through the platform seam. On Impossible OS that seam is implemented on the UI framework engines, so no Direct2D, DirectWrite or DirectComposition layer is needed.

- [ ] Drawing: map the seam's drawing interface (fills, strokes, paths, images, clips, transforms, layers) onto the graphics foundation of `08-graphics-ui/TODO-01-graphics-asset-foundation.md` §1 to §3
- [ ] Text: map text layout, measurement and hit-testing onto the text engine of `08-graphics-ui/TODO-02-text-font-internationalization.md` §3 and §4
- [ ] Composition: the page surface, scrolling and animated transitions use the composition visual tree of `08-graphics-ui/TODO-18-ui-framework-engines.md` §12
- [ ] Input: pointer, touch, wheel, keyboard and IME reach `recto-ui` through the routed input of `08-graphics-ui/TODO-18-ui-framework-engines.md` §7 and §8
- [ ] Theme: Recto's tokens resolve through the theme resources of `08-graphics-ui/TODO-18-ui-framework-engines.md` §6, so it follows light, dark, contrast and accent like every other app
- [ ] Accessibility: `recto-ui` elements publish automation peers through `08-graphics-ui/TODO-18-ui-framework-engines.md` §24
- [ ] Commit: `"apps: recto-ui backend on the Impossible OS UI engines"`

**Test checkpoint:** Recto's toolkit gallery page renders on Impossible OS and its automation dump lists every control with a name; a theme switch restyles it without a restart. Test on: QEMU TCG and KVM.

## 15. Recto App on Impossible OS

> **Spawned-by:** root

**Needs:** third-party

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout), [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)

The application itself: Recto's window, its three layout presets and its reading features, running as `C:\Program Files\Recto\Recto.exe`. This replaces the `pdfview.exe` window, search and continuous scroll this file used to plan.

- [ ] Build the Recto app for Impossible OS from `src/app` with the engine (§13) and the toolkit backend (§14)
- [ ] Window: Recto's tabs in the title bar use the title bar and tab view controls of `08-graphics-ui/TODO-19-fluent-component-library.md` §22 and §17 through the seam, so its chrome matches every other window
- [ ] Reading: continuous, single-page and two-page views, zoom, outline and thumbnail panels, text selection and `Ctrl+F` search work as Recto specifies in its `design/SPEC.md`
- [ ] Dialogs: open, save and print go through the system dialogs, not Recto-drawn copies
- [ ] Settings: Recto's settings persist through the seam's settings store under the user profile
- [ ] Record in Recto's `docs/scope.md` what differs on Impossible OS (no Windows-only OCR provider, see §16)
- [ ] Commit: `"apps: Recto runs on Impossible OS"`

**Test checkpoint:** launching `Recto.exe test.pdf` shows page 1; `Ctrl+F` finds a known word and highlights it; switching the layout preset rearranges the same controls; the serial log shows `recto: document opened (N pages)`. Test on: QEMU TCG and KVM; WHPX at 2 CPUs.

## 16. OCR Provider

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- a recognition engine, it draws nothing

Recto has two OCR paths: its built-in neural engine and `Windows.Media.Ocr`. Only the first exists on Impossible OS.

- [ ] Build Recto's built-in OCR engine for Impossible OS behind the seam's compute interface (CPU only until a GPU compute path exists)
- [ ] Exclude the `Windows.Media.Ocr` provider from the Impossible OS build at the seam, and have Recto report one provider
- [ ] Ship the recognition models as data files under `C:\Program Files\Recto\models\`, with their licence recorded in `CREDITS.md`
- [ ] Bound the cost: OCR runs on the render pool at below-normal priority and can be cancelled
- [ ] Commit: `"apps: Recto built-in OCR on Impossible OS"`

**Test checkpoint:** a scanned one-page test PDF becomes searchable: `Ctrl+F` finds a word that exists only in the image; cancelling mid-run returns within 1 second.

## 17. Convert and Organize Features

> **Spawned-by:** root

**Needs:** third-party

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

Recto's later milestones (convert, organize) arrive through re-import. This section is the check that each works on Impossible OS as it lands.

- [ ] Convert: PDF to images and text, images to PDF, searchable OCR PDF, and the Office conversions Recto ships, each verified with one fixture per direction
- [ ] Organize: combine, split, reorder, rotate, delete and extract pages, and passwords and permissions, always on a copy and saved atomically through the seam
- [ ] `recto-cli` is built and installed beside the app, so the same conversions run from the command shell
- [ ] Printing goes through the system print path when it exists; until then the Print command is hidden, not broken
- [ ] Commit: `"apps: Recto convert and organize verified on Impossible OS"`

**Test checkpoint:** `recto-cli convert test.pdf --to png` writes one PNG per page; combining two fixtures yields a file whose page count is the sum; a power cut simulated mid-save leaves the original intact.

## 18. System Integration

> **Spawned-by:** root

**Design:** n/a -- registrations and handlers, it draws nothing new

Recto is the system's PDF handler everywhere a PDF appears.

- [ ] Register Recto through the app model of `08-graphics-ui/TODO-18-ui-framework-engines.md` §20: app id, name, icon, the `.pdf` type and single-instance activation; §9 of this file points its association at `Recto.exe`
- [ ] Thumbnail provider and preview handler for `.pdf` through the extension points of `08-graphics-ui/TODO-18-ui-framework-engines.md` §19, running the engine out of process with a time limit
- [ ] Property handler: page count, title, author and creation date for the details pane and columns (`08-graphics-ui/TODO-18-ui-framework-engines.md` §18)
- [ ] Search: PDF text is indexed through a filter that calls the engine (`09-desktop-shell/TODO-05-file-search.md` §1)
- [ ] Open from the network: the browser and mail hand a downloaded PDF to Recto (`07-networking/TODO-10-pdf-viewer.md` §9)
- [ ] Commit: `"apps: Recto registered as the system PDF handler"`

**Test checkpoint:** double-clicking a PDF in the file manager opens Recto; the file's thumbnail shows page 1; the details pane shows its page count; searching for a word inside the PDF finds the file.

## 19. Packaging and Updates

> **Spawned-by:** root

**Design:** n/a -- packaging, it draws nothing

- [ ] Install Recto into the image at `C:\Program Files\Recto\` with its resources and models, a Start entry and its file association, from `scripts/build.sh`
- [ ] Record the bundled Recto commit and version in the image manifest and show it in Recto's About dialog
- [ ] Recto updates with the OS: a new pinned commit ships in an OS update through `10-platform-services/TODO-03-updates-packages.md`
- [ ] The image size cost of Recto and its models is printed by the build and held under a budget recorded in `docs/apps/pdf-viewer.md`
- [ ] Commit: `"apps: Recto packaged in the system image"`

**Test checkpoint:** a fresh image boots with Recto in Start; `dir "C:\Program Files\Recto"` lists `Recto.exe` and `recto-cli.exe`; the build log prints `recto: image cost N MiB (budget M)`.

## 20. Recto Conformance on Impossible OS

> **Spawned-by:** root

**Design:** n/a -- a test suite, it draws nothing

One command proves the bundled Recto still works after an import or an OS change.

- [ ] `scripts/test-recto.sh`: runs the engine tests, the fuzz smoke, the render comparison of §13 and a scripted app session (open, page, zoom, search, close) under the headless compositor
- [ ] Performance: Recto's own targets (first page, scroll frame time, search) measured on the software compositor and recorded per run
- [ ] Wire the suite into the smoke matrix as one leg once §15 ships
- [ ] Commit: `"test: Recto conformance suite"`

**Test checkpoint:** `bash scripts/test-recto.sh` ends with `recto: conformance OK` and exits 0; breaking the seam's file read makes it fail at the first document open.

---

## OS Comparison

| ⭐  | Feature                                         | 🪟 Win11                      | 🐧 Linux            | 🚀 Impossible OS       |
| --- | ----------------------------------------------- | ----------------------------- | ------------------- | ---------------------- |
| 💎  | Built-in PDF reading                            | ✅ Edge PDF viewer            | ✅ Evince, Okular   | ⬜ Planned -- §13, §15 |
| 💎  | Text search and selection                       | ✅ Edge                       | ✅ Evince           | ⬜ Planned -- §15      |
| 💎  | Thumbnails, preview and properties in the shell | ⚠️ Preview via Edge handler   | ✅ Via thumbnailers | ⬜ Planned -- §18      |
| ⭐  | OCR for scanned PDFs, built in                  | ❌ Not in the inbox viewer    | ⚠️ Separate tools   | ⬜ Planned -- §16      |
| ⭐  | Convert and organize, built in                  | ❌ Needs other software       | ⚠️ Separate tools   | ⬜ Planned -- §17      |
| ⭐  | One native app, no browser engine               | ❌ A browser hosts the viewer | ✅ Native viewers   | ⬜ Planned -- §15      |

> **After §10-§13:** Impossible OS has a C++23 runtime and Recto's engine renders PDFs. **After §14-§15:** Recto is the PDF app, matching the inbox viewers of Windows 11 and Linux. **After §16-§20:** OCR, conversion and page organizing are built in, which neither ships in its default viewer.

---

## Unit Tests

> Recto's own tests are the unit tests: they run on the host and inside the OS under the user-mode test launcher (§13, §20). No kernel test category is needed.

- [ ] `user/tests/cxx/test_cxx.exe` covers the C++ runtime of §11 (exceptions, threads, containers, `std::format`, `std::expected`)
- [ ] The `recto-platform-ios` tests cover the seam of §12: atomic save leaves the original intact on a simulated failure; a file mapping reads back what was written
- [ ] Recto's engine tests pass unchanged on Impossible OS (§13)
- [ ] Commit: `"test: C++ runtime and Recto platform suites"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`, with Recto and the C++ runtime built
- [ ] `bash scripts/vendor-recto.sh --check` and `--seam-check` both exit 0
- [ ] `bash scripts/test-recto.sh` ends with `recto: conformance OK`
- [ ] `bash scripts/test-smoke-matrix.sh` passes with the Recto leg; the serial log contains `recto: document opened`
- [ ] QEMU WHPX at 2 CPUs and VirtualBox: a 100-page PDF opens, scrolls and searches with no fault in the serial log
- [ ] Bare metal: scrolling a text-heavy PDF holds the frame budget on the software compositor, and touch pan and pinch work on a touch display (VM behaviour differs)
