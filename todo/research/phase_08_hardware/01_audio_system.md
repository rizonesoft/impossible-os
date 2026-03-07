# 24 — Audio System

## Overview

Sound card driver, audio mixer, and system sounds. Required for media player, system alerts, and games.

---

## Stack

```
Apps (media player, games, system sounds)
        ↓
Audio Mixer (mix multiple streams, volume control)
        ↓
Audio Driver (AC97 / Intel HDA)
        ↓
Hardware (sound card via PCI)
```

## Sound Card Drivers

| Device | QEMU flag | Complexity | Notes |
|--------|-----------|-----------|-------|
| **AC97** | `-device AC97` (default) | ~300 lines | Simple, well-documented |
| **Intel HDA** | `-device intel-hda -device hda-duplex` | ~800 lines | Modern standard |
| **Sound Blaster 16** | `-device sb16` | ~200 lines | Very simple ISA device |

## Audio API
```c
/* audio.h */
int audio_init(void);
int audio_play(const int16_t *pcm_data, uint32_t samples, uint32_t sample_rate);
void audio_set_volume(uint8_t volume);  /* 0-100 */
int audio_is_playing(void);
```

## Codec libraries: dr_wav, dr_mp3 (public domain, see `libraries/29_audio_libraries.md`)

## System sounds (stored in `C:\Impossible\Sounds\`)
- Startup chime, click, error, notification, shutdown

## Files: `src/kernel/drivers/ac97.c`, `src/kernel/audio.c`, `include/audio.h`
## Implementation: 2-3 weeks
