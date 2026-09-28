<!-- docs: covers=todo/08-graphics-ui/TODO-01-graphics-asset-foundation.md sources=resources/icons/src,resources/icons/color,resources/brand/logo.svg,resources/fonts,resources/cursors,scripts/convert-icons.sh reviewed=2026-09-28 -->
# System Icons

The colour icons are original artwork that reads instantly as Windows 11 while every shape and gradient is our own. This page is the canonical spec. Sources live in [`resources/icons/src/`](../../resources/icons/src/); the rendered PNGs in `resources/icons/color/<size>/` are packed into `icons.ires` by the build.

## At a glance

| Rule | Value |
| --- | --- |
| Grid | 48 x 48 viewBox, 2 px outer padding, so live art sits inside 4..44 |
| View | Front-facing with gentle depth, never isometric |
| Light | One key light from straight above |
| Fills | Soft vertical gradients, lighter at the top |
| Highlight | A 1 px lighter edge along top surfaces |
| Edges | No black outlines; a 1 px edge at 25 to 60 percent opacity only where a shape would vanish on a light or dark background |
| Corner radius | 2 px on containers, 3 px on devices and windows, 1 to 1.5 px on small glyphs |
| Sizes shipped | 16, 24, 32, 48, 64, 72, 96, 128 and 256 px |

## Palette

Folders use a warm manila: the front runs `#FFD75E` to `#F5B82E`, the back and tab `#F0B429` to `#D9920F` (the `#E8A317` family), with a strip of white paper between them.

System objects use the logo blues (`#2EB5F0`, `#2185E6`, `#155CDE`); glyphs on folders run `#3AA0F5` to `#1565D8`.

Neutrals come from a cool grey ramp (`#E6EBF1` through `#3D4753`) for device bodies, the gear and page edges.

The Recycle Bin is clear glass, as on Windows 11: the dark interior (`#4E6680` to `#8CA6C2`) shows through an open elliptical rim (`#E8F0F8` to `#7D93AD`), and the glass is tinted and lit from the left (`#E4F0FB` at 92 percent to `#93B0D0` at 90 percent) with one soft reflection band and no ribs; the full bin holds crumpled paper balls (lumpy outlines, many small flat facets catching the light unevenly, and crease lines) that rest on each other from the bottom up to the brim, seen through a lighter glass, the top one sitting in the opening. The terminal is dark slate (`#3A4452` to `#1E252E`) with a cyan prompt (`#40DAF2`).

## Small-size rules

Every icon is checked at 16 and 24 px on `#202020` and `#F3F3F3` before it lands:

- Glyphs on folders are at least 12 units wide so they survive at 16 px as a coloured blob in the right place.
- Strokes that carry meaning are at least 1.6 units wide; anything thinner is decoration and may disappear at small sizes.
- A shape that relies on a light edge for contrast also gets a faint darker edge, so it holds on both light and dark backgrounds.
- One source file per icon: where detail collapses, simplify the source instead of adding a small-size file.

## The set

| Icon | File | Used for |
| --- | --- | --- |
| This PC | `computer.svg` | The This PC desktop icon and Explorer root |
| Recycle Bin (empty) | `recycle_bin_empty.svg` | Recycle Bin with nothing in it |
| Recycle Bin (full) | `recycle_bin_full.svg` | Recycle Bin holding deleted items |
| Folder | `folder_closed.svg` | Any folder |
| Open folder | `folder_open.svg` | Folder being opened, drag targets |
| Documents | `folder_documents.svg` | The Documents known folder |
| Downloads | `folder_downloads.svg` | The Downloads known folder |
| Pictures | `folder_pictures.svg` | The Pictures known folder |
| Music | `folder_music.svg` | The Music known folder |
| Videos | `folder_videos.svg` | The Videos known folder |
| Desktop | `folder_desktop.svg` | The Desktop known folder |
| User folder | `user_folder.svg` | The signed-in user's profile folder |
| File Explorer | `file_explorer.svg` | The File Explorer app, pinned to the taskbar |
| Network | `network.svg` | Network location and the Network desktop icon: This PC's monitor (reused, at 80 percent) with a cable running out from under its stand to three devices |
| System drive | `drive_system.svg` | The `C:` drive, badged with the OS mark |
| Removable drive | `drive_removable.svg` | USB sticks and other removable media |
| Settings | `settings.svg` | The Settings app |
| Control Panel | `control_panel.svg` | The classic Control Panel |
| Terminal | `terminal.svg` | The terminal and `cmd.exe` |
| Generic file | `file_default.svg` | A file with no specific type |
| Text file | `text_file.svg` | `.txt`, `.log`, `.ini` and similar |
| Image file | `file_image.svg` | `.png`, `.jpg`, `.bmp` and similar |
| Archive | `file_archive.svg` | `.zip` and other archives |
| Application | `exe_default.svg` | `.exe` files with no embedded icon |
| Library | `dll_default.svg` | `.dll` and `.sys` files |
| Start | `start.svg` | The Start button on the taskbar |

## The logo and the Start button

The logo in [`resources/brand/logo.svg`](../../resources/brand/logo.svg) keeps the original identity: four 64-unit ribbons on a 368 grid, arranged as a pinwheel whose slanted ends give the octagonal, impossible outline. The redesign keeps the geometry and changes the light: the top ribbon is the lightest cyan, the bottom the deepest blue, and each ribbon darkens where its inner end tucks under the next, which sells the impossible loop. `resources/logo.svg`, `gh-pages/logo.svg` and the boot logo PNGs in `resources/logo/` are copies or renders of this file.

The Start button (`start.svg`) is the same mark inside the standard 2 px padding, and reads at 24 px on both frosted taskbars.

## Which icons go where?

- **Colour icons** (this set, `resources/icons/src/`) represent user-facing objects: files, folders, drives, apps, places and system items on the desktop, in File Explorer, in Start and on the taskbar.
- **Monochrome glyphs** come from Fluent System Icons (MIT, vendored as fonts in `resources/fonts/`): commands, toolbar and command-bar buttons, menu items, tray and status glyphs, and object types in developer tools (for example kernel object types in ObBrowse). They are tinted with `text_primary` or `text_secondary`, never recoloured per item.
- **Elevation shield** (UAC) is the Fluent shield glyph at `size.shield_glyph` (16), tinted `accent`, placed before the label of any button that elevates.
- **Status** glyphs follow the [status colours](controls.md#status-colours) rule.
- No other third-party icon set is used, and no colour icon is copied from Windows.

## What about cursors?

Cursors are a deliberate decision, not a gap: the system keeps the existing Adwaita cursor set (`resources/cursors/`, credited in CREDITS.md) for now. A Windows 11 style cursor redesign is future work; until it exists, no section draws its own cursors.

## How are icons rendered?

Impossible OS is SVG-first (decided 2026-09-28). System icons, cursors and shell glyphs ship as SVG and are drawn at the exact size, scale and theme where they appear, so they are crisp at 100%, 150%, 200% and every size in between. Native apps can carry an SVG icon inside their EIF executable. PNG, ICO, BMP and JPEG stay fully supported for Windows apps and for pictures.

- **Rendering.** A vendored SVG renderer (plutovg and plutosvg) draws each icon once per size, scale, theme and accent and keeps the result in an on-disk icon cache, so an icon is never drawn twice for the same size, and a theme change redraws only what is on screen.
- **The system SVG profile.** System icons use a fixed subset of SVG: paths and basic shapes, fills and strokes, linear and radial gradients, opacity, transforms, `defs`, `use`, `symbol` and `clipPath`. No masks, filters, text or CSS. The build rejects an icon outside the profile.
- **Theming.** Icon colours are token names from [`tokens.json`](tokens.json), so light, dark, accent and high contrast re-tint the same source instead of shipping separate files.
- **The icon engine.** A build-time tool generates icons from shared parts (the monitor used by This PC and Network, the folder, the document), validates every icon, and regenerates only icons whose inputs changed. Every icon is one of three kinds, so polish is never lost: **hand-authored** (the SVG is the source and the engine never rewrites it), **generated** (the definition is the source, and output is byte-for-byte reproducible), or a **per-size override** (a hand-tuned file for one size always wins). A hand edit to generated output fails the check instead of being overwritten.

The work is planned in the [graphics asset foundation roadmap](../../todo/08-graphics-ui/TODO-01-graphics-asset-foundation.md), in its SVG runtime renderer and icon engine sections.

## What happens when an icon is missing?

A missing icon is never shipped as the generic fallback and never parked as a stub. Whoever needs it, the unattended run included, adds a **draft** in the same change:

1. Add its row to [The set](#the-set) (the spec comes first), naming what it is used for.
2. Draw it from the existing parts and metaphors (the This PC monitor, the folder, the document, the drive), following the palette, the small-size rules and the system SVG profile. Until the icon engine exists, that means an SVG in `resources/icons/src/`; afterwards, a generated definition.
3. List it in [Drafts awaiting polish](#drafts-awaiting-polish) with the section that needed it.

**When no part fits.** Most new icons are an existing part plus an overlay (a drive with a badge, a document with a glyph). When the icon needs an object nothing in the library draws, such as a printer or a camera, the draft adds that object as a **new part** first, drawn to the same rules (palette tokens, light from the top left, the small-size rules), and the icon then composes it. The part is listed in the drafts table too, so polishing it improves every icon that uses it, and the next icon that needs a printer reuses it instead of drawing its own. If an object cannot be drawn credibly even as a simple part, the floor is the **tile plus glyph** draft: the standard rounded tile in the icon's category colour with a simple line glyph built from primitives. It is plain but on-palette, legible and clearly named, which the generic fallback is not.

A draft is a real, validated icon, so the feature works and looks consistent on day one. Polish comes later, by hand or in the definition, and it persists: a hand edit makes the icon **hand-authored**, which the engine never rewrites. Polishing an icon removes its row from the drafts table.

### Drafts awaiting polish

| Icon | File | Needed by | Drafted |
| --- | --- | --- | --- |

## Adding or changing an icon

1. Edit or add the SVG in `resources/icons/src/`. Give every gradient an id prefixed with the icon name, so several icons can be inlined into one page without collisions.
2. Run `bash scripts/convert-icons.sh` to re-render all nine sizes, then check the result at 16 and 24 px on a light and a dark background.
3. Commit the SVG and the regenerated PNGs together.
4. A new name reaches the shell only once it is in the `irespack` accepted list, the `system_icon_t` enum and the kernel name table. Ten are wired today (`computer`, `control_panel`, `dll_default`, `exe_default`, `file_default`, `folder_closed`, `folder_open`, `recycle_bin_empty`, `recycle_bin_full`, `text_file`); the rest are rendered and committed for the shell work that needs them.
