# P0801 — USB & Hardware (Hub)

> **Goal:** USB host controller stack, device class drivers,
> and advanced memory management for kernel scalability.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Audio System

> **Moved to [TODO-P0006-Audio.md](TODO-P0006-Audio.md)** — AC97 driver, audio
> abstraction layer, mixer, codec libraries (WAV/MP3/OGG/FLAC/MIDI), and
> unified audio loader.

---

## 2. USB Support

> **Moved to [TODO-P1601-Drivers.md](TODO-P1601-Drivers.md) §6** — USB core,
> xHCI host controller, HID (keyboard/mouse), Mass Storage, hub support,
> EHCI/UHCI fallback.

---

## 3. Media Player & Sound Settings

> **Moved to [TODO-P0006-Audio.md](TODO-P0006-Audio.md) §6–8** — Media Player app,
> volume popup, sound settings applet.

---

## 4. USB Hot-Plug & Input Fallback

> **Moved to [TODO-P1601-Drivers.md](TODO-P1601-Drivers.md) §6.5–6.7** — Hot-plug event
> system, PS/2↔USB fallback, safe removal.

---

## 5. Advanced Memory Management

> **Moved to [TODO-P0012-Memory-Advanced.md](TODO-P0012-Memory-Advanced.md)** — SLAB allocator,
> vmalloc, growable heap.
