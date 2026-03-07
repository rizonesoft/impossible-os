# 44 — Web Browser

## Overview

The biggest app project. Requires HTML/CSS rendering, JavaScript engine, HTTP/HTTPS networking, and DOM.

---

## Reality check

| Component | Lines of code | Notes |
|-----------|--------------|-------|
| HTML parser | ~5,000 | Tag parsing, DOM tree |
| CSS parser + layout | ~20,000 | Box model, cascading, selectors |
| Rendering engine | ~10,000 | Paint boxes, text, images |
| JavaScript engine | ~50,000+ | Interpreter or JIT |
| **Firefox total** | ~25,000,000 | For reference |
| **Chrome total** | ~35,000,000 | For reference |

> [!WARNING]
> A web browser is the most complex app in any OS. Even a minimal one (HTML subset + CSS + no JS) is 30,000+ lines.

## Realistic phased approach

### Phase 1: Text-only browser (Lynx-like)
- HTTP GET requests (we'll have TCP/DNS)
- Strip HTML tags, display text
- Follow links (click → navigate)
- ~500 lines

### Phase 2: Basic HTML renderer
- Headings, paragraphs, lists, bold/italic
- Images (`<img>` via `image_load()`)
- Links with underline + click
- Tables (basic)
- ~5,000 lines

### Phase 3: CSS support
- Box model, margins, padding
- Colors, fonts, backgrounds
- Simple selectors
- ~15,000 lines

### Phase 4: JavaScript (long-term)
- Either port **Duktape** (MIT, ~60K lines, ES5.1) or **QuickJS** (MIT, ~35K lines, ES2020)
- Both are embeddable, standard C, minimal dependencies

## Alternative: Port an existing engine
- **NetSurf** (GPL, ~200K lines) — full browser for embedded/hobby OS, already ported to several OSes
- **Dillo** (GPL, ~30K lines) — minimalist browser
- **Links/ELinks** — text mode browser

## Files: `src/apps/browser/` (~5,000-50,000 lines depending on phase)
## Implementation: months to years
