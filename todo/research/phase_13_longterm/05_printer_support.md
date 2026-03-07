# 48 — Printer Support (Long-term)

## Overview

Printing requires a printer driver model, page rendering, and spooler service.

---

## Approach

### Option A: PDF printing (simplest)
- Render page to PDF file → user transfers to another machine to print
- Needs a basic PDF writer (~500 lines)

### Option B: Network printing (IPP/AirPrint)
- Internet Printing Protocol over HTTP (port 631)
- Most modern printers support IPP
- Requires: TCP + HTTP (from `22_internet_wireless.md`)
- ~1,000 lines for basic IPP client

### Option C: USB printing
- USB Printer Class driver
- Raw data over USB bulk transfers
- Needs: PostScript or PCL page description
- Most complex option

## Print API
```c
/* print.h */
int  print_document(const char *printer_name, const uint8_t *data, 
                    uint32_t size, const char *format);
int  print_enum_printers(char names[][64], int max);
```

## Recommendation: Start with PDF export, add IPP network printing later.
## Priority: 🔴 Long-term
## Implementation: 2-4 weeks (for IPP)
