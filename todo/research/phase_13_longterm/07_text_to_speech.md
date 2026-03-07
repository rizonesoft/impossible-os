# 92 — Text-to-Speech

## Overview
Convert text to spoken audio. Used by screen reader (accessibility), notifications, and assistant features.

## Library options
| Library | License | Size | Quality |
|---------|---------|------|---------|
| **espeak-ng** | GPL-3 | ~200K lines | Robotic but small |
| **Piper** | MIT | Large (neural) | Natural, needs ONNX runtime |
| **SAM (Software Automatic Mouth)** | Public domain | ~2K lines | Very retro, very small |
| **Custom formant** | — | ~1K lines | Basic but tiny |

## Recommendation: Start with **SAM** (public domain, fits in freestanding kernel) for basic speech, upgrade to **espeak-ng** later.

## API
```c
int tts_speak(const char *text);           /* blocking: generate and play */
int tts_speak_async(const char *text);     /* non-blocking */
void tts_set_rate(uint8_t rate);           /* 0=slow, 100=fast */
void tts_set_voice(const char *voice);
void tts_stop(void);
```

## Depends on: `24_audio_system.md` (audio playback)
## Files: `src/kernel/tts.c` (~300 lines wrapper) + SAM port | 2-4 weeks
## Priority: 🔴 Long-term
