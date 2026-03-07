# 43 — Media Player

## Overview

Audio file playback with playlist support. Uses AC97/HDA driver + dr_mp3/dr_wav decoders.

---

## Layout

```
┌──────────────────────────────────────┐
│  Media Player                   ─ □ ×│
├──────────────────────────────────────┤
│                                      │
│         🎵 Song Title                │
│         Artist Name                  │
│         Album Name                   │
│                                      │
│    0:42 ━━━━━━━━━━━░░░░░░░░ 3:28    │
│                                      │
│       ⏮   ▶   ⏭    🔊━━━━━         │
│                                      │
├──────────────────────────────────────┤
│  Now Playing:                        │
│  1. Song One              3:28       │
│  2. Song Two ▶            4:15       │
│  3. Song Three            2:52       │
└──────────────────────────────────────┘
```

## Supported formats (via dr_libs — public domain)
| Format | Library | License |
|--------|---------|---------|
| WAV | dr_wav | Public Domain |
| MP3 | dr_mp3 | Public Domain |
| FLAC | dr_flac | Public Domain |
| OGG Vorbis | stb_vorbis | Public Domain |

## Features: play/pause/stop, next/prev, seek bar, volume, repeat/shuffle, playlist
## File associations: `.mp3`, `.wav`, `.flac`, `.ogg`
## Depends on: `24_audio_system.md`

## Files: `src/apps/mediaplayer/mediaplayer.c` (~600 lines)
## Implementation: 1-2 weeks
