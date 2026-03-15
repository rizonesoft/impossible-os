# TODO-535 — Impossible OS C++ SDK

> **Goal:** Build a fully featured C++ SDK that lets developers write, compile,
> and distribute native Impossible OS applications. The SDK provides the headers,
> libraries, toolchain wrappers, documentation, and developer tooling needed to
> target Impossible OS from both Linux (cross-compile) and natively on the OS.
>
> **Dependency:** The compiler toolchain (TODO-530-Compiler.md) must be completed
> first. TCC provides C support; GCC/G++ provides the full C++ capability the SDK
> requires.

> [!IMPORTANT]
> **Compiler dependency:** Sections §1–§3 (headers, C runtime, base libs) can be
> prepared in parallel with TODO-530. Full C++ SDK (§4–§6) requires GCC/G++ from
> TODO-530 §3.2–§3.3 to be complete.

> [!NOTE]
> **Public repo:** The SDK is published as a public repo
> `rizonesoft/impossible-os-sdk` (see TODO-001-GitHub.md §5). This allows
> third-party developers to target Impossible OS without access to the private
> source repo.

---

## 1. SDK Header Set

### 1.1 Core System Headers (C)

**Prompt:** Create the core C system headers for Impossible OS. These are the foundation of the SDK — every program will include at least one of these. They must be compatible with TCC and GCC. Model on the Win32 API style (since PE is the binary format) with Impossible OS-specific extensions. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sdk: core C system headers"`. Create `docs/sdk/headers.md` documenting each header.

- [ ] Create `sdk/include/impossible.h` — master include (includes all below)
- [ ] Create `sdk/include/types.h` — `uint8_t`…`uint64_t`, `size_t`, `bool`, `NULL`, `HANDLE`
- [ ] Create `sdk/include/errors.h` — error codes (`IOS_OK`, `IOS_ERR_*`), `GetLastError()`
- [ ] Create `sdk/include/memory.h` — `VirtualAlloc`, `VirtualFree`, `HeapAlloc`, `HeapFree`
- [ ] Create `sdk/include/process.h` — `CreateProcess`, `ExitProcess`, `GetCurrentPID`
- [ ] Create `sdk/include/thread.h` — `CreateThread`, `ExitThread`, `Sleep`, `GetThreadID`
- [ ] Create `sdk/include/sync.h` — `CreateMutex`, `WaitForObject`, `ReleaseMutex`, semaphores
- [ ] Create `sdk/include/file.h` — `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`, `GetFileSize`
- [ ] Create `sdk/include/filesystem.h` — `CreateDirectory`, `DeleteFile`, `FindFirstFile`, `FindNextFile`
- [ ] Create `sdk/include/string.h` — `strcpy`, `strlen`, `strcmp`, `sprintf`, `memcpy`, `memset`
- [ ] Create `sdk/include/math.h` — `abs`, `sqrt`, `sin`, `cos`, `floor`, `ceil`, `pow`
- [ ] Create `sdk/include/io.h` — `printf`, `scanf`, `puts`, `gets`, `fopen`, `fclose`, `fread`, `fwrite`
- [ ] Create `sdk/include/time.h` — `GetSystemTime`, `GetTickCount`, `Sleep`
- [ ] Cross-reference: all types consistent with `include/kernel/` headers
- [ ] Commit: `"sdk: core C system headers"`

### 1.2 GUI / Window Manager Headers

**Prompt:** Create the GUI headers that let apps create windows, draw to the screen, and handle input. Model on the Win32 API (`CreateWindowEx`, `DefWindowProc`, `WndProc`, message loop) since it's familiar and our PE execution model is Win32-like. Keep it simple initially — no full Win32 compatibility required. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sdk: GUI/window manager headers"`. Create `docs/sdk/gui.md`.

- [ ] Create `sdk/include/window.h` — `CreateWindow`, `DestroyWindow`, `ShowWindow`, `SetWindowTitle`
- [ ] Create `sdk/include/message.h` — `MSG` struct, `GetMessage`, `TranslateMessage`, `DispatchMessage`
- [ ] Create `sdk/include/paint.h` — `BeginPaint`, `EndPaint`, `InvalidateRect`, `UpdateWindow`
- [ ] Create `sdk/include/gdi.h` — `DrawText`, `DrawRect`, `DrawLine`, `DrawBitmap`, `SetPixel`
- [ ] Create `sdk/include/input.h` — `GetAsyncKeyState`, `GetCursorPos`, keyboard scan codes
- [ ] Create `sdk/include/dialog.h` — `MessageBox`, `OpenFileDialog`, `SaveFileDialog`
- [ ] Create `sdk/include/menu.h` — `CreateMenu`, `AppendMenu`, `SetMenu`, `TrackPopupMenu`
- [ ] Create `sdk/include/controls.h` — button, edit, listbox, combobox, scrollbar control creation
- [ ] Create `sdk/include/font.h` — `CreateFont`, `SetFont`, `MeasureText`, `DrawTextEx`
- [ ] Create `sdk/include/bitmap.h` — `LoadBitmap`, `CreateBitmap`, `BlitBitmap`, `DestroyBitmap`
- [ ] Commit: `"sdk: GUI/window manager headers"`

### 1.3 C++ Standard Library Headers (STL subset)

**Prompt:** Provide a curated subset of the C++ Standard Library headers suitable for OS application development. Rather than porting a full libstdc++ (which has POSIX dependencies), provide clean, minimal implementations or forward to libstdc++ where available (GCC §3.3). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: C++ STL headers"`.

- [ ] Create `sdk/include/cxx/string` — `std::string`, `std::wstring`
- [ ] Create `sdk/include/cxx/vector` — `std::vector<T>`
- [ ] Create `sdk/include/cxx/map` — `std::map<K,V>`, `std::unordered_map<K,V>`
- [ ] Create `sdk/include/cxx/list` — `std::list<T>`
- [ ] Create `sdk/include/cxx/memory` — `std::unique_ptr<T>`, `std::shared_ptr<T>`
- [ ] Create `sdk/include/cxx/functional` — `std::function<T>`, lambdas
- [ ] Create `sdk/include/cxx/algorithm` — `std::sort`, `std::find`, `std::for_each`
- [ ] Create `sdk/include/cxx/iostream` — `std::cout`, `std::cin`, `std::cerr`
- [ ] Create `sdk/include/cxx/fstream` — `std::ifstream`, `std::ofstream`
- [ ] Create `sdk/include/cxx/chrono` — `std::chrono::steady_clock`, `duration_cast`
- [ ] Create `sdk/include/cxx/thread` — `std::thread`, `std::mutex`, `std::lock_guard`
- [ ] Commit: `"sdk: C++ STL headers"`

---

## 2. C Runtime Library

### 2.1 startup.asm — Process Entry Point

**Prompt:** Every Impossible OS PE binary needs a startup stub that sets up the C runtime environment before calling `main()`. This stub: (1) initializes the heap, (2) sets up argc/argv from the process command line, (3) calls static constructors (C++ init), (4) calls `main()`, (5) calls static destructors (C++ atexit), (6) calls `ExitProcess`. One `startup.asm` + `crt0.c` provides this for all SDK programs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: C runtime startup"`.

- [ ] Create `sdk/crt/startup.asm` — PE entry point `_start`
- [ ] Stack alignment to 16-byte boundary before calling C
- [ ] Call `__crt_init()` — heap init, argc/argv setup
- [ ] Call `__cxx_init()` — run C++ static constructors (`.init_array`)
- [ ] Call `main(argc, argv)` — user entry point
- [ ] Call `__cxx_fini()` — run C++ static destructors (`.fini_array`)
- [ ] Call `ExitProcess(return value of main)`
- [ ] Create `sdk/crt/crt0.c` — `__crt_init()`, `__cxx_init()`, `__cxx_fini()`, argc/argv parsing
- [ ] Commit: `"sdk: C runtime startup"`

### 2.2 libimp.a — Import Library

**Prompt:** Create import libraries (`.lib`/`.a` files) that let the linker resolve calls to OS functions (kernel syscalls exposed as Win32-style DLL stubs). `kernel32.lib` covers memory, process, file I/O, threading. `user32.lib` covers window management, messages, input. `gdi32.lib` covers drawing. These are thin wrappers over the kernel syscall interface. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: import libraries"`.

- [ ] Create `sdk/lib/kernel32.lib` — `VirtualAlloc`, `HeapAlloc`, `CreateFile`, `ReadFile`, `WriteFile`, `CreateProcess`, `ExitProcess`, `CreateThread`, `Sleep`, `GetLastError`
- [ ] Create `sdk/lib/user32.lib` — `CreateWindow`, `DestroyWindow`, `GetMessage`, `DispatchMessage`, `MessageBox`, `ShowWindow`, `InvalidateRect`
- [ ] Create `sdk/lib/gdi32.lib` — `DrawText`, `DrawRect`, `SetPixel`, `DrawBitmap`, `CreateFont`
- [ ] Commit: `"sdk: import libraries"`

---

## 3. Build System

### 3.1 CMake Toolchain File

**Prompt:** Provide a CMake toolchain file that makes it easy to cross-compile Impossible OS applications from Linux. Developers set `CMAKE_TOOLCHAIN_FILE=impossible-os.cmake`, set the sysroot to the SDK directory, and CMake will use the correct compiler, linker flags, and libraries. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: CMake toolchain"`. Create `docs/sdk/cmake.md`.

- [ ] Create `sdk/cmake/impossible-os.cmake` toolchain file
- [ ] Set `CMAKE_SYSTEM_NAME ImpossibleOS`
- [ ] Set `CMAKE_C_COMPILER x86_64-w64-mingw32-gcc` (or path to cross-GCC)
- [ ] Set `CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++`
- [ ] Set `CMAKE_EXE_LINKER_FLAGS` for PE output
- [ ] Set sysroot include/lib paths to SDK dirs
- [ ] Create CMake module `FindImpossibleSDK.cmake`
- [ ] Provide example `CMakeLists.txt` for a hello world app
- [ ] Test: `cmake -DCMAKE_TOOLCHAIN_FILE=... && make` produces a PE binary
- [ ] Commit: `"sdk: CMake toolchain"`

### 3.2 Makefile Template

**Prompt:** Provide a simple Makefile template for building Impossible OS apps without CMake. Targets: `all` (build), `clean`, `install` (copies to disk image). Includes the correct cross-compiler flags. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: Makefile template"`.

- [ ] Create `sdk/templates/Makefile.template`
- [ ] Cross-compiler: `x86_64-w64-mingw32-g++` (or `tcc` for C-only)
- [ ] Flags: `-Wall -Wextra -O2 -nostdlib -ffreestanding` for bare metal; relax for userland
- [ ] Include paths: `$(SDK_ROOT)/include`
- [ ] Library paths + libs: `$(SDK_ROOT)/lib/kernel32.lib -limpossibled`
- [ ] Target: link to PE `.exe` using cross-linker
- [ ] Commit: `"sdk: Makefile template"`

---

## 4. C++ Application Framework (IxUI)

### 4.1 IxUI — Native C++ GUI Framework

**Prompt:** IxUI (Impossible UI) is a C++ object-oriented GUI framework built on top of the low-level window headers. It provides a modern C++ API: `Application`, `Window`, `Widget` base class, event handling via `std::function` callbacks, layout managers, and built-in widgets. Think Qt-lite. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sdk: IxUI C++ GUI framework"`. Create `docs/sdk/ixui.md`.

- [ ] Create `sdk/ixui/include/ixui/Application.h` — singleton app class, message loop
- [ ] Create `sdk/ixui/include/ixui/Window.h` — base window wrapper class
- [ ] Create `sdk/ixui/include/ixui/Widget.h` — abstract base widget (draw, hit-test, event)
- [ ] Create `sdk/ixui/include/ixui/Button.h` — push button widget
- [ ] Create `sdk/ixui/include/ixui/Label.h` — static text widget
- [ ] Create `sdk/ixui/include/ixui/TextBox.h` — single/multi-line text input
- [ ] Create `sdk/ixui/include/ixui/ListBox.h` — scrollable list widget
- [ ] Create `sdk/ixui/include/ixui/ComboBox.h` — drop-down selector
- [ ] Create `sdk/ixui/include/ixui/CheckBox.h` — checkbox + radio button
- [ ] Create `sdk/ixui/include/ixui/Slider.h` — numeric slider widget
- [ ] Create `sdk/ixui/include/ixui/Layout.h` — VBoxLayout, HBoxLayout, GridLayout
- [ ] Create `sdk/ixui/include/ixui/Event.h` — `MouseEvent`, `KeyEvent`, `PaintEvent` structs
- [ ] Create `sdk/ixui/src/` — implementations of all above
- [ ] Build `sdk/lib/libixui.a` static library
- [ ] Commit: `"sdk: IxUI C++ GUI framework"`

### 4.2 IxUI Style System

**Prompt:** Add a theming/styling system to IxUI. Widgets read colors, fonts, padding, and border radii from a `StyleSheet` object (similar to CSS for Qt). Supports the built-in Impossible OS theme (dark/light) and custom themes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: IxUI style system"`.

- [ ] Create `sdk/ixui/include/ixui/StyleSheet.h` — color, font, padding, radius per widget type
- [ ] `StyleSheet::fromTheme(DARK | LIGHT)` — load OS default theme colors
- [ ] `StyleSheet::fromFile("app.iss")` — load custom stylesheet from `.iss` file
- [ ] Widget `setStyleSheet(StyleSheet*)` — override default style
- [ ] Commit: `"sdk: IxUI style system"`

### 4.3 IxUI Graphics Context

**Prompt:** Provide a high-level C++ `Painter` class for 2D drawing within widgets. Wraps the low-level GDI calls in a clean API: draw primitives, text, images, gradients, and clipping. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: IxUI Painter graphics context"`.

- [ ] Create `sdk/ixui/include/ixui/Painter.h`
- [ ] `Painter::fillRect(x, y, w, h, color)` — filled rectangle
- [ ] `Painter::drawRect(x, y, w, h, color, thickness)` — outline rectangle
- [ ] `Painter::drawText(x, y, text, font, color)` — anti-aliased text
- [ ] `Painter::drawImage(x, y, Image*)` — blit a loaded image
- [ ] `Painter::drawLine(x1, y1, x2, y2, color, thickness)` — line segment
- [ ] `Painter::fillGradient(x, y, w, h, c1, c2, direction)` — linear gradient
- [ ] `Painter::setClipRect(x, y, w, h)` — restrict drawing to a rect
- [ ] Commit: `"sdk: IxUI Painter graphics context"`

---

## 5. Developer Tools

### 5.1 SDK Installer / Sysroot Package

**Prompt:** Package the SDK as a downloadable archive that developers can extract on Linux to get everything needed to cross-compile Impossible OS apps: headers, import libs, toolchain file, and example projects. Also provide a `setup.sh` script for automated installation. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: sysroot installer package"`. Create `docs/sdk/getting-started.md`.

- [ ] Create `sdk/package.sh` — assembles SDK into `impossible-os-sdk-vX.Y.Z.tar.gz`
- [ ] Package contents: `include/`, `lib/`, `cmake/`, `templates/`, `examples/`, `docs/`
- [ ] Create `sdk/setup.sh` — installs SDK to `~/.impossible-os-sdk/`, configures PATH
- [ ] Create `sdk/uninstall.sh` — removes SDK
- [ ] GitHub Actions: build and attach SDK tarball to each release
- [ ] Commit: `"sdk: sysroot installer package"`

### 5.2 Example Projects

**Prompt:** Provide example projects demonstrating how to use the SDK for common app types. Each example should build with the CMake toolchain and produce a working `.exe`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: example projects"`.

- [ ] Create `sdk/examples/hello-console/` — C++ console app (`printf`, `ExitProcess`)
- [ ] Create `sdk/examples/hello-window/` — basic IxUI window with a button
- [ ] Create `sdk/examples/notepad-clone/` — IxUI TextBox + file open/save dialogs
- [ ] Create `sdk/examples/paint-app/` — IxUI Painter drawing canvas
- [ ] Create `sdk/examples/file-browser/` — IxUI ListView showing directory contents
- [ ] Each example includes `CMakeLists.txt`, `Makefile.template`, and `README.md`
- [ ] Commit: `"sdk: example projects"`

### 5.3 SDK Documentation

**Prompt:** Write comprehensive SDK documentation. Cover the full API reference, getting started guide, porting guide (from Win32), and architecture overview. Published to the public `impossible-os-sdk` GitHub repo. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"docs: full SDK documentation"`.

- [ ] Create `docs/sdk/getting-started.md` — install SDK, hello world, build and run
- [ ] Create `docs/sdk/api/system.md` — `impossible.h` full API reference
- [ ] Create `docs/sdk/api/gui.md` — window + message API reference
- [ ] Create `docs/sdk/api/ixui.md` — IxUI class reference (all widgets, Painter, Application)
- [ ] Create `docs/sdk/api/cpp-stl.md` — supported STL subset reference
- [ ] Create `docs/sdk/porting-guide.md` — how to port a Win32 app to Impossible OS
- [ ] Create `docs/sdk/architecture.md` — SDK layering diagram (headers → libs → IxUI)
- [ ] Update repo `README.md` with quick-start snippet
- [ ] Commit: `"docs: full SDK documentation"`

### 5.4 SDK Version Management

**Prompt:** Define a versioning scheme for SDK releases that tracks OS compatibility. Each SDK release is numbered `vMAJOR.MINOR.PATCH` and declares which OS version it targets. Provide compatibility tables so developers know which SDK for which OS. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"sdk: versioning and compatibility table"`.

- [ ] Create `sdk/VERSION` file — current SDK version string
- [ ] Create `sdk/include/impossible/version.h` — `IOS_SDK_VERSION_MAJOR/MINOR/PATCH`
- [ ] Create `docs/sdk/compatibility.md` — SDK version vs OS version table
- [ ] Tag SDK releases as `sdk-v1.0.0` on the public SDK repo
- [ ] Commit: `"sdk: versioning and compatibility table"`

---

## 6. On-OS Dev Experience

### 6.1 SDK Pre-installed on OS

**Prompt:** When Impossible OS ships, the SDK headers and tools should be pre-installed so developers can start coding immediately. Place headers at `C:\Impossible\Include\`, libs at `C:\Impossible\Lib\`, and examples at `C:\Impossible\SDK\Examples\`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sdk: pre-installed on OS image"`.

- [ ] Add SDK files to the FAT32 disk image build (Makefile/build.sh step)
- [ ] `C:\Impossible\Include\` — all SDK headers
- [ ] `C:\Impossible\Lib\` — `kernel32.lib`, `user32.lib`, `gdi32.lib`, `libixui.a`
- [ ] `C:\Impossible\SDK\Examples\` — sample projects
- [ ] `C:\Impossible\SDK\Docs\` — offline documentation (plain text / HTML)
- [ ] Shell command: `sdk-info` — prints SDK version, compiler version, include path
- [ ] Commit: `"sdk: pre-installed on OS image"`

### 6.2 Native IDE / Code Editor Integration

**Prompt:** Provide syntax highlighting, autocomplete, and build integration for the built-in code editor (Notepad / future IDE). At minimum: keyword highlighting for Impossible OS types/macros, autocomplete for common IxUI classes, a build button that runs TCC/GCC, and an error list pane that parses compiler output. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sdk: native IDE integration"`. Cross-reference TODO-330-Notepad.md.

- [ ] Add syntax highlighter for C/C++ to code editor (TODO-330)
- [ ] Autocomplete database: all SDK type names, macros, IxUI classes
- [ ] Build action: Ctrl+B → runs `tcc` or `g++` with SDK flags
- [ ] Parse compiler errors → jump to line in editor
- [ ] Run action: Ctrl+F5 → builds and launches output `.exe`
- [ ] Commit: `"sdk: native IDE integration"`

---

## Priority Order

| Priority | Section                       | Dependency                                       |
|----------|-------------------------------|--------------------------------------------------|
| 🔴 P0   | §1.1 Core C headers           | None — do this now alongside TODO-530            |
| 🔴 P0   | §1.2 GUI/WM headers           | Window manager (TODO-110)                        |
| 🔴 P0   | §2.1 CRT startup              | None — needed by all SDK programs                |
| 🟠 P1   | §2.2 Import libraries         | Kernel syscall ABI stable (TODO-020)             |
| 🟠 P1   | §3.1 CMake toolchain          | §1.1 + §2.2                                      |
| 🟠 P1   | §3.2 Makefile template        | §1.1 + §2.2                                      |
| 🟡 P2   | §4.1 IxUI framework           | §1.2 GUI headers, TODO-130-Controls              |
| 🟡 P2   | §4.2 IxUI style system        | §4.1                                             |
| 🟡 P2   | §4.3 IxUI Painter             | §4.1                                             |
| 🟡 P2   | §1.3 C++ STL headers          | TODO-530 §3.3 (GCC C++ support)                  |
| 🟢 P3   | §5.1 SDK installer            | §1–§4 complete                                   |
| 🟢 P3   | §5.2 Example projects         | §4 IxUI complete                                 |
| 🟢 P3   | §5.3 Documentation            | §1–§5.2 complete                                 |
| 🟢 P3   | §5.4 Versioning               | First stable SDK release                         |
| 🔵 P4   | §6.1 Pre-installed on OS      | Full SDK built + disk image pipeline             |
| 🔵 P4   | §6.2 Native IDE integration   | TODO-330-Notepad complete                        |

---

## Key Paths

| Path                             | Purpose                                    |
|----------------------------------|--------------------------------------------|
| `sdk/include/`                   | All public SDK headers                     |
| `sdk/include/cxx/`               | C++ STL subset headers                     |
| `sdk/ixui/include/ixui/`         | IxUI C++ GUI framework headers             |
| `sdk/lib/`                       | Import libraries + static libs             |
| `sdk/crt/`                       | C runtime startup (`crt0.c`, `startup.asm`)|
| `sdk/cmake/`                     | CMake toolchain file                       |
| `sdk/templates/`                 | Makefile template                          |
| `sdk/examples/`                  | Example projects                           |
| `sdk/docs/`                      | Developer documentation                    |
| `C:\Impossible\Include\`         | On-OS SDK headers (pre-installed)          |
| `C:\Impossible\Lib\`             | On-OS SDK libraries (pre-installed)        |
| `C:\Impossible\SDK\Examples\`    | On-OS example projects                     |

---

## Cross-References

| TODO                         | Relationship                                              |
|------------------------------|-----------------------------------------------------------|
| TODO-530-Compiler.md         | **Dependency** — TCC (C) + GCC/G++ (C++) required        |
| TODO-110-UI-Framework.md     | **Dependency** — IxUI wraps the kernel's window manager   |
| TODO-130-Controls.md         | **Dependency** — IxUI mirrors the kernel control set      |
| TODO-040-Filesystem.md       | **Dependency** — `file.h` and `filesystem.h` depend on FS |
| TODO-001-GitHub.md           | **Deployment** — SDK published as public GitHub repo       |
| TODO-330-Notepad.md          | **Consumer** — IDE integration uses SDK autocomplete       |
