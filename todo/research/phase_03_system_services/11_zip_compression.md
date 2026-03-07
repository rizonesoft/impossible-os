# 89 — File Compression (ZIP)

## Overview
Create and extract ZIP archives. Required for: app installer (.ipkg = ZIP), downloads, file sharing.

## Library: **miniz** (MIT, single file, ~4000 lines, already researched in `31_general_utilities.md`)
- Deflate/inflate compression
- ZIP archive read/write
- zlib-compatible API

## API
```c
/* zip.h */
int zip_create(const char *zip_path, const char **files, int count);
int zip_extract(const char *zip_path, const char *dest_dir);
int zip_list(const char *zip_path, char names[][256], int max);
int zip_add_file(const char *zip_path, const char *file_path);
```

## Shell command: `zip archive.zip file1 file2`, `unzip archive.zip`
## Integration: right-click → "Compress to ZIP" / "Extract here"
## Files: `src/kernel/zip.c` (~200 lines wrapper around miniz) | 3-5 days
