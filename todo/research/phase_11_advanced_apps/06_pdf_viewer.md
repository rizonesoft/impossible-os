# 91 — PDF Viewer

## Overview
Open and display PDF documents. Render pages as images, navigate, zoom, search text.

## Library options
| Library | License | Size | Notes |
|---------|---------|------|-------|
| **mupdf** | AGPL-3 | ~200K lines | Full-featured, too complex |
| **Custom minimal** | — | ~2K lines | Render basic text/image PDFs only |

## Minimal approach
- Parse PDF cross-reference table
- Extract page objects
- Decompress streams (zlib/deflate via miniz)
- Render text (via font system) and images (via image decoder)
- Skip: forms, JavaScript, annotations, encryption (initially)

## Features: page navigation, zoom, search text on page, print to file
## Files: `src/apps/pdfview/pdfview.c` (~800 lines) | 2-4 weeks
## Priority: 🟡 Medium-term
