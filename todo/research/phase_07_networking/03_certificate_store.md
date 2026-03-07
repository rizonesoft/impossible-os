# 63 — Certificate Store

## Overview

Root CA certificates for HTTPS/TLS trust. Without this, HTTPS connections cannot verify server identity.

---

## Certificate bundle
Use **Mozilla's CA bundle** (MPL 2.0) — same certs trusted by Firefox and curl.
- ~130 root CA certificates
- ~200 KB file size
- Stored at `C:\Impossible\System\Certs\ca-bundle.crt` (PEM format)

## API
```c
/* Passed to TLS library (BearSSL / Mbed TLS) during handshake */
int tls_load_ca_bundle(const char *path);
int tls_verify_cert(const uint8_t *cert_der, uint32_t len);
```

## Update: new CA bundle shipped with OS updates
## Depends on: `22_internet_wireless.md` (TLS phase)

## Files: `resources/certs/ca-bundle.crt`, `src/kernel/net/tls_certs.c` (~50 lines)
## Implementation: 1 day (just loading the bundle into the TLS library)
