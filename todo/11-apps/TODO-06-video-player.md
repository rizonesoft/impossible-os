---
schema_version: 1
id: video-player
domain: 11-apps
status: active
title: "TODO-06 -- Video Player"
---

# TODO-06 -- Video Player

> **Goal:** Build `player.exe` -- a video player for Impossible OS backed by **pl_mpeg** (public
> domain, single-header MPEG-1/MPEG audio decoder). Decodes MPEG-1 video frames, converts YCbCr
> to RGB via an SSE2-optimised path, synchronises A/V streams against a wall-clock baseline,
> and presents a clean windowed UI with seek, zoom, fullscreen, playlist, and SRT subtitles.

> [!IMPORTANT]
> Audio subsystem (`audio_mixer_stream_add`, `audio_mixer_stream_stop`) from
> `10-platform-services/TODO-01-audio-system.md §2` **must** be complete before §3 A/V sync --
> `→ XREF: 10-platform-services/TODO-01 §2`.
> All video frame buffers and PCM decode buffers are large (frame = width × height × 4 bytes) --
> **always use `pmm_alloc_contiguous()` for these**; `kmalloc` heap is only 2 MiB.
> SSE2 is available (`-msse2` builds only separate SIMD objects such as `src/kernel/gfx/gfx_simd.c`) but the kernel uses
> `-mno-sse` / `-mno-sse2` by default; the player app compiles separately as a user-mode EIF
> with SSE2 enabled.

---

## Inputs

- `10-platform-services/TODO-01-audio-system.md §2` -- `audio_mixer_stream_add(pcm, samples, vol)`, `audio_mixer_stream_stop(handle)` from `include/audio_mixer.h`
- `include/kernel/timer.h` -- `system_get_ticks()` (monotonic ms counter), `sleep_ms(ms)`
- `include/gfx.h` -- `gfx_surface_create()`, `gfx_fill_rect()`, `gfx_blit()`, `gfx_scale_blit()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_SLIDER` (seek and volume sliders per `docs/design/controls.md#slider`), `CTRL_TEXTBOX`
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_fullscreen()`, `wm_mark_dirty()`
- `include/desktop/file_assoc.h` -- `file_assoc_set(ext, prog_id, path)` (→ XREF `09-desktop-shell/TODO-02 §1`)
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_enum_keys`
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- file I/O for pl_mpeg + SRT

---

## Outcome

`player.exe file.mpg` plays back MPEG-1 video with synchronised audio. Full playback controls (seek bar, play/pause, volume), fullscreen mode with auto-hiding controls, playlist sidebar, and SRT subtitle overlay. `.mpg` / `.mpeg` / `.avi` file associations open in the player. Foundation is extensible to H.264 (Phase 2) via codec swap.

---

## Implementation Order

| Step | Section                    | 💎/⭐ | Dependency                                                                             | Status |
| ---- | -------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | pl_mpeg Integration        | 💎    | §9                                                                                     |  [ ]   |
| 2    | YCbCr → RGB Conversion     | 💎    | §9, §1                                                                                 |  [ ]   |
| 3    | A/V Sync Engine            | 💎    | §9, §1, `10-platform-services/TODO-01` §2                                              |  [ ]   |
| 4    | Player UI                  | 💎    | §9, §2, §3                                                                             |  [ ]   |
| 5    | File Operations            | 💎    | §9, §4                                                                                 |  [ ]   |
| 6    | Playlist                   | ⭐    | §9, §4, §5                                                                             |  [ ]   |
| 7    | Subtitle Support (Stretch) | ⭐    | §9, §4                                                                                 |  [ ]   |
| 8    | Format Support Roadmap     | ⭐    | §9, §1                                                                                 |  [ ]   |
| 9    | Import Media Player        | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §4 |  [ ]   |

---

## 1. pl_mpeg Integration

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings pl_mpeg Integration"`

---

## 2. YCbCr → RGB Conversion

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings YCbCr → RGB Conversion"`

---

## 3. A/V Sync Engine

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **Decode loop** (runs in dedicated kernel thread `task_create("player_decode", ...)`)
  - [ ] While `VP_PLAYING`:
    - [ ] Compute `video_time = (system_get_ticks() - start_ticks) / 1000.0` (seconds)
    - [ ] `frame = plm_decode_video(vp->plm, video_time)` → `NULL` if nothing due yet
    - [ ] If `frame != NULL`: `yuv420_to_rgb(frame, &ring[ring_head])`; record `frame_pts[ring_head] = frame->time`; advance `ring_head = (ring_head + 1) % 3`
    - [ ] Check for display: if `frame_pts[ring_tail]` ≤ `video_time`: advance `ring_tail`; signal compositor to blit new frame
    - [ ] **Drift correction**: `audio_ahead = audio_pts - video_pts`
      - If `audio_ahead > 0.200 s` (audio ahead): skip display of current video frame (fast-forward video)
      - If `audio_ahead < −0.200 s` (video ahead): repeat last frame (`ring_tail` unchanged); extend sleep
    - [ ] Sleep: `frame_remaining_ms = frame_pts[ring_tail+1] - video_time) × 1000`; `sleep_ms(max(1, frame_remaining_ms - 2))`
- [ ] Sound a program decodes reaches the mixer (`audio_mixer_stream_add`) in step with its pictures
- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings A/V Sync Engine"`

---

## 4. Player UI

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings Player UI"`

---

## 5. File Operations

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **File associations** (registered at app init):
  - [ ] `file_assoc_set(".mpg",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")`
  - [ ] `file_assoc_set(".mpeg", "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")`
  - [ ] `file_assoc_set(".avi",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")` (stub; plays if container has MPEG-1 video track)
  - [ ] `file_assoc_set(".mp4",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")` (stub; "Format not yet supported" message until H.264 phase)
- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings File Operations"`

---

## 6. Playlist

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings Playlist"`

---

## 7. Subtitle Support (Stretch)

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings Subtitle Support (Stretch)"`

---

## 8. Format Support Roadmap

**Moved:** media-player

Built in MediaPlayer, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: media-player brings Format Support Roadmap"`

## 9. Import Media Player

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** media-player

Media Player is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull media-player` into `src/apps/media-player/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Media Player on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose media-player`
- [ ] Commit: `"apps: Media Player imported"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                    | 🐧 Linux              | 🚀 Impossible OS                                     |
| --- | ---------------------------------------- | --------------------------- | --------------------- | ---------------------------------------------------- |
| 💎  | MPEG-1 video decode                      | ✅ Windows Media Player     | ✅ VLC / mpv          | ⬜ §1 -- pl_mpeg single-header port                  |
| 💎  | YCbCr → RGB                              | ✅ WMP (GPU)                | ✅ VLC (libyuv)       | ⬜ §2 -- SSE2 4-pixel-at-a-time                      |
| 💎  | A/V synchronisation + drift correction   | ✅ WMP                      | ✅ mpv (audio-driven) | ⬜ §3 -- 3-frame ring buffer, ±200 ms                |
| 💎  | Seek bar + time display + volume         | ✅ WMP                      | ✅ VLC                | ⬜ §4 -- slider seek, OSD pill                       |
| ⭐  | Fullscreen with 3 s auto-hiding controls | ✅ WMP / films app          | ✅ VLC / mpv          | ⬜ §4 -- `wm_set_fullscreen`, hide_timer_ms          |
| 💎  | Playlist with loop + shuffle             | ✅ WMP                      | ✅ VLC                | ⬜ §6 -- CTRL_LISTVIEW sidebar, Fisher-Yates shuffle |
| ⭐  | SRT subtitle overlay with TTF text       | ✅ WMP (limited) / films ✅ | ✅ VLC (built-in)     | ⬜ §7 -- (Stretch) -- ; `ttf_draw_string` +          |
| 💎  | File associations                        | ✅ WMP default              | ✅ `xdg-open`         | ⬜ §5 -- `file_assoc_set`                            |
| 💎  | H.264 / MP4 support                      | ✅ WMP / HEVC codec         | ✅ VLC / mpv          | ⬜ §8 -- (Phase 2) -- ; h264bsd                      |

Impossible OS ships a fully native, zero-dependency video player backed by a public-domain single-header codec -- no COM, no DirectShow, no GStreamer pipeline. The SSE2 YCbCr converter and 3-frame ring buffer with drift correction give smooth playback on commodity hardware from day one.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **pl_mpeg port:** `vp_create(fd)` on `test.mpg`; serial log shows width/height/fps/duration; no allocation panics
- [ ] **YCbCr scalar:** call `yuv420_to_rgb` on first frame; dump pixel `[0,0]` -- must be within ±3 of reference value; no visible colour-channel swap
- [ ] **YCbCr SSE2:** pixel values identical to scalar path for same frame; serial log shows `yuv_convert: {w}×{h} in < {frame_period_ms} ms`
- [ ] **A/V sync:** play 30 s of `test.mpg` with audio; no visible audio/video drift; serial log shows drift < 50 ms throughout
- [ ] **Seek:** drag seek bar to 50%; playback resumes from correct position; audio restarts in sync
- [ ] **Pause/Resume:** `[⏯]` pauses; `[⏯]` again resumes; `pause_offset_ms` accounts for paused duration (no jump on resume)
- [ ] **Controls UI:** seek bar advances in real-time; time display updates each second; volume slider adjusts audio level
- [ ] **Fullscreen:** F11 → title bar hidden; canvas fills display; controls hide after 3 s; mouse move → controls reappear
- [ ] **Playlist:** add 3 files; play first; auto-advances to second at end; loop-playlist wraps back to first
- [ ] **File assoc:** `.mpg` double-click in File Manager → `player.exe` opens and begins playback
- [ ] **Subtitles (stretch):** place `test.srt` next to `test.mpg`; open video → subtitles auto-load; correct text overlaid at correct timestamps; `S` toggles off/on
- [ ] Commit: `"apps: video player -- pl_mpeg, YCbCr/SSE2, A/V sync, UI, playlist, subtitles"`
