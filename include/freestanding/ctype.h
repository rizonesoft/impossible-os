/* Freestanding <ctype.h> shim -- ASCII/C locale only.
 *
 * Added for ACPICA, which under ACPI_USE_SYSTEM_CLIBRARY expects the standard
 * ctype set; its AML paths classify HID/UID strings and hex digits with these.
 * Static inline so no translation unit owns the definitions and nothing new
 * reaches the link.
 *
 * "C locale" is the whole contract: the kernel has no locale machinery and
 * ACPI names are ASCII by specification (ACPI 6.5 sections 19.2.2, 20.2.2), so
 * classifying anything above 0x7F as alphabetic would be wrong, not merely
 * incomplete. Inputs are taken as int and reduced to unsigned char, which is
 * what makes a negative char argument defined rather than an array underrun.
 */
#ifndef _FREESTANDING_CTYPE_H
#define _FREESTANDING_CTYPE_H

static inline int isdigit(int c)  { unsigned u = (unsigned)(unsigned char)c; return u >= '0' && u <= '9'; }
static inline int isupper(int c)  { unsigned u = (unsigned)(unsigned char)c; return u >= 'A' && u <= 'Z'; }
static inline int islower(int c)  { unsigned u = (unsigned)(unsigned char)c; return u >= 'a' && u <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) { unsigned u = (unsigned)(unsigned char)c;
                                    return isdigit(c) || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F'); }
static inline int isspace(int c)  { unsigned u = (unsigned)(unsigned char)c;
                                    return u == ' ' || (u >= '\t' && u <= '\r'); }
static inline int isprint(int c)  { unsigned u = (unsigned)(unsigned char)c; return u >= 0x20 && u < 0x7F; }
static inline int isgraph(int c)  { unsigned u = (unsigned)(unsigned char)c; return u > 0x20 && u < 0x7F; }
static inline int iscntrl(int c)  { unsigned u = (unsigned)(unsigned char)c; return u < 0x20 || u == 0x7F; }
static inline int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

static inline int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
static inline int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }

#endif /* _FREESTANDING_CTYPE_H */
