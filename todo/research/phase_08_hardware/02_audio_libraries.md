# Audio Libraries — Research

## Overview

Audio support requires: a sound card driver, an audio mixing/playback API, and codec libraries for decoding compressed audio formats (MP3, OGG, WAV, etc.).

---

## Sound Card Drivers

| Device | Type | Notes |
|--------|------|-------|
| **AC97** (Intel ICH) | QEMU default | Simple, well-documented, common in hobby OS |
| **Intel HDA** | Modern standard | More complex, used in real hardware |
| **Sound Blaster 16** | Legacy ISA | Very simple, great for learning |
| **virtio-sound** | Paravirtual | QEMU virtio, efficient |

> [!TIP]
> **AC97 is the best starting point** for QEMU. It's a PCI device with a simple register interface. Many hobby OS tutorials cover it.

---

## Audio Playback Libraries

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **miniaudio** | MIT / Public Domain | ✅ Yes | Single header, ~75K lines | Full audio engine, playback + capture + mixing |
| **dr_libs** (dr_wav, dr_mp3, dr_flac) | MIT / Public Domain | ✅ Yes | Single headers, ~3K each | Decoders only, no playback |
| **stb_vorbis** | MIT / Public Domain | ✅ Yes | Single header, ~5K lines | OGG Vorbis decoder |
| **SDL_mixer** | Zlib | ✅ Yes | Needs SDL | Full mixer with format support |

---

## Audio Codec Libraries

### WAV (uncompressed)

| Library | License | Size | Notes |
|---------|---------|------|-------|
| **dr_wav** | Public Domain | ~1500 lines | Single header, decode + encode |
| **Manual parser** | N/A | ~50 lines | WAV is trivial (RIFF header + PCM data) |

### MP3

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **dr_mp3** | Public Domain | ✅ Yes | ~3000 lines | Single header, based on minimp3 |
| **minimp3** | CC0 (Public Domain) | ✅ Yes | ~1500 lines | Reference minimal decoder |
| **libmpg123** | LGPL 2.1 | ✅ Yes | ~30K lines | Full-featured, SIMD-optimized |

### OGG Vorbis (open alternative to MP3)

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **stb_vorbis** | Public Domain | ✅ Yes | ~5000 lines | Single header, decode only |
| **libvorbis** | BSD-3 | ✅ Yes | ~30K lines | Reference implementation |

### FLAC (lossless)

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **dr_flac** | Public Domain | ✅ Yes | ~4000 lines | Single header |
| **libFLAC** | BSD-3 | ✅ Yes | ~30K lines | Reference implementation |

### MIDI

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **TinySoundFont** | MIT | ✅ Yes | Single header | SoundFont-based MIDI synthesis |
| **fluidsynth** | LGPL 2.1 | ✅ Yes | Large | Full MIDI synthesizer |

---

## Recommended Stack for Impossible OS

| Layer | What | Library | Priority |
|-------|------|---------|----------|
| **Driver** | AC97 sound card | Custom (kernel) | P1 |
| **Mixer** | Audio mixing + output | miniaudio or custom | P2 |
| **WAV** | Uncompressed audio | dr_wav or manual | P1 |
| **MP3** | Compressed audio | dr_mp3 (public domain) | P2 |
| **OGG** | Open compressed audio | stb_vorbis | P2 |
| **FLAC** | Lossless audio | dr_flac | P3 |
| **MIDI** | Music synthesis | TinySoundFont | P3 |

> [!NOTE]
> The `dr_libs` family (dr_wav, dr_mp3, dr_flac) are all **public domain**, single-header, zero-dependency C files. They're the ideal choice for Impossible OS — drop-in, no licensing concerns.
