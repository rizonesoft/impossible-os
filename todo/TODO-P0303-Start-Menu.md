# P0303 — Start Menu

> **Goal:** Windows 7-layout Start Menu with Windows 11/12 dark mode aesthetics,
> app launching, and search filtering.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 7. Start Menu *(from Phase 04 §2)*

> **Design:** Windows 7 two-column layout rendered with Windows 11/12 dark mode
> aesthetics — Acrylic blur, rounded corners, accent highlights, Selawik font.

### 7.1 Start Menu Layout *(from Phase 04 §2.1)*

**Design:** Windows 7-style two-column popup with Win11/12 dark aesthetics, anchored above the Start button. **Left column** (260px): search bar at top, alphabetically grouped pinned programs below (headings A, B, C…). **Right column** (190px, darker overlay): quick access links — Computer, Documents, Pictures, Music, Downloads, Control Panel, Help. Bottom of right column has MS Fluent icon buttons (Settings, Power popup with Shutdown/Restart). Acrylic blur background matching taskbar (tint `0x202020`, opacity 200, blur 3), drop shadow, rounded corners (10px), 14px Selawik font. Implemented in `src/desktop/desktop.c`.

- [x] Two-column layout anchored above Start button (450px × dynamic height)
  - [x] **Left column** (260px): search bar at top + alphabetical pinned programs
  - [x] Search bar: 42px tall, rounded corners, 🔍 icon + placeholder text
  - [x] Pinned programs: heading letter (A, T…) + icon + label per row (38px rows)
  - [x] Items: About, All Programs ►, Terminal
- [x] **Right column** (190px, darker alpha-blended overlay):
  - [x] Quick access: Computer, Documents, Pictures, Music, Downloads, Control Panel, Help
  - [x] Each item: system icon + label, hover highlight (`0x3A3A3A`)
  - [x] Bottom: Settings ⚙ and Power ⏻ icon buttons (36px)
- [x] Win11/12 dark mode rendering:
  - [x] Acrylic blur background (tint `0xFF202020`, opacity 200, blur radius 3)
  - [x] Right column: per-pixel alpha-blended overlay (`0xB0181818`) — no double noise
  - [x] Drop shadow (12px radius, +4y offset, `rgba(0,0,0,140)`)
  - [x] Rounded corners (10px) via fixed `gfx_draw_rounded_rect` (outline only)
  - [x] 1px column divider + 1px border outline
  - [x] Accent-color hover highlights, 14px Selawik (FONT_UI) for labels
  - [x] Inner padding: 14px
- [x] `gfx_draw_rounded_rect` bug fix: rewrote to trace edges per scanline (was filling entire rect)
- [x] Commits: `"desktop: start menu layout"`, `"desktop: fix acrylic"`, `"gfx: fix gfx_draw_rounded_rect"`

### 7.2 Start Menu Data *(from Phase 04 §2.2)*

**Prompt:** Load pinned apps from the Registry, scan installed apps from filesystem for All Programs list. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu data loading"`. Update `README.md` if it contains stale or incorrect references to the Start menu. Create or update documentation in `docs/` covering pinned app loading, app scanning, and alphabetical grouping.


- [ ] Load pinned apps from Registry `HKU\{name}\Software\Impossible\Shell\PinnedApps`
- [ ] Scan installed apps from `C:\Impossible\Bin\` and `C:\Programs\`
- [ ] Build alphabetical "All Programs" list with folder grouping
- [ ] Right-column quick links → map to filesystem paths (Documents → `C:\Users\{name}\Documents\`, etc.)
- [ ] Display app icons from icon store
- [ ] Commit: `"desktop: start menu data loading"`

### 7.3 Start Menu Interaction *(from Phase 04 §2.3)*

**Prompt:** Toggle open/close on Start click or Win key. Launch apps, navigate All Programs, power submenu, search filtering. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu interaction"`. Update `README.md` if it contains stale or incorrect references to the Start menu. Create or update documentation in `docs/` covering Start menu interaction, All Programs navigation, power actions, and search filtering.


- [ ] Start button / Win key → toggle menu
- [ ] Click pinned app → launch, close menu
- [ ] "All Programs ►" → slide transition replacing left column with app list + "Back" link
- [ ] Click right-column link → open folder in File Manager
- [ ] Power button → fly-out submenu: Shut Down, Restart, Sleep, Lock
- [ ] Search bar: type to filter pinned + all programs + right-column links
- [ ] Slide-up animation (200ms, `GFX_EASE_OUT_CUBIC`)
- [ ] Click outside / Escape → close
- [ ] Commit: `"desktop: start menu interaction"`

