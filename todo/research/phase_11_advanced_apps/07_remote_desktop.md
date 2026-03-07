# 84 — Remote Desktop

## Overview
View and control another Impossible OS machine's desktop remotely. Like Windows Remote Desktop (RDP).

## Approach
- **VNC protocol** (simpler) — sends framebuffer rectangles over TCP
- Or custom protocol: compress framebuffer diffs + forward input events

## Server side
- Capture compositor backbuffer as JPEG/PNG tiles
- Send changed tiles to client
- Receive keyboard/mouse events from client, inject into WM

## Client side
- Connect to remote IP:port
- Display received framebuffer
- Forward local keyboard/mouse → remote

## Compression: JPEG for lossy speed or zlib for lossless
## Depends on: TCP (`22`), image encoding (`06`)

## Files: `src/apps/rdp/rdp_server.c` (~500 lines), `src/apps/rdp/rdp_client.c` (~400 lines) | 2-4 weeks
## Priority: 🔴 Long-term
