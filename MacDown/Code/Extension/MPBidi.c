//
//  MPBidi.c
//  MacDown 3000
//

#include <string.h>
#include "MPBidi.h"

const MPBidiDirection MPBidiDefaultFallback = MPBidiDirectionLTR;

// Explicit marks. U+061C ARABIC LETTER MARK is included because it is a
// strong-RTL formatting character; note it is outside the range slugify()
// strips, so nothing may emit it into a heading.
#define MP_LRM 0x200E
#define MP_RLM 0x200F
#define MP_ALM 0x061C

/**
 * Decodes one UTF-8 sequence. Returns the code point and advances *i.
 * Malformed bytes decode as themselves, which keeps a truncated buffer from
 * looping and cannot turn neutral bytes into strong ones.
 */
static uint32_t mp_next(const uint8_t *data, size_t size, size_t *i)
{
    uint8_t c = data[*i];
    size_t remaining = size - *i;

    if (c < 0x80) { *i += 1; return c; }
    if ((c & 0xE0) == 0xC0 && remaining >= 2) {
        *i += 2;
        return (uint32_t)((c & 0x1F) << 6 | (data[*i - 1] & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && remaining >= 3) {
        *i += 3;
        return (uint32_t)((c & 0x0F) << 12 | (data[*i - 2] & 0x3F) << 6
                          | (data[*i - 1] & 0x3F));
    }
    if ((c & 0xF8) == 0xF0 && remaining >= 4) {
        *i += 4;
        return (uint32_t)((c & 0x07) << 18 | (data[*i - 3] & 0x3F) << 12
                          | (data[*i - 2] & 0x3F) << 6 | (data[*i - 1] & 0x3F));
    }
    *i += 1;
    return c;
}

/**
 * Strong right-to-left, approximating UAX#9's R and AL classes by script
 * block. Digits and punctuation inside those blocks are deliberately excluded:
 * Arabic-Indic digits are class AN and Arabic punctuation is neutral, so
 * neither may decide a paragraph.
 */
static int mp_is_rtl(uint32_t cp)
{
    if (cp == MP_RLM || cp == MP_ALM)
        return 1;

    // Arabic number signs, punctuation and digits — not strong.
    if (cp >= 0x0600 && cp <= 0x0605) return 0;
    if (cp == 0x060C || cp == 0x061B || cp == 0x061F) return 0;
    if (cp >= 0x0660 && cp <= 0x066D) return 0;
    if (cp == 0x06DD) return 0;
    if (cp >= 0x06F0 && cp <= 0x06F9) return 0;

    return (cp >= 0x0590 && cp <= 0x05FF)      // Hebrew
        || (cp >= 0x0600 && cp <= 0x07BF)      // Arabic, Syriac, Thaana
        || (cp >= 0x07C0 && cp <= 0x085F)      // N'Ko, Samaritan, Mandaic
        || (cp >= 0x0860 && cp <= 0x08FF)      // Syriac Sup., Arabic Ext.
        || (cp >= 0xFB1D && cp <= 0xFDFF)      // Hebrew and Arabic pres. forms
        || (cp >= 0xFE70 && cp <= 0xFEFF)      // Arabic pres. forms-B
        || (cp >= 0x10800 && cp <= 0x10FFF)    // Cypriot, Phoenician, Kharoshthi
        || (cp >= 0x1E800 && cp <= 0x1EFFF);   // Mende Kikakui, Adlam, Arabic Math
}

/**
 * Strong left-to-right, approximating UAX#9's L class. Digits are excluded —
 * they are class EN and cannot decide a paragraph.
 */
static int mp_is_ltr(uint32_t cp)
{
    if (cp == MP_LRM)
        return 1;
    if (cp < 0x41)
        return 0;

    return (cp >= 0x41 && cp <= 0x5A)          // A-Z
        || (cp >= 0x61 && cp <= 0x7A)          // a-z
        || (cp >= 0x00C0 && cp <= 0x02FF)      // Latin supplements, IPA
        || (cp >= 0x0370 && cp <= 0x058F)      // Greek, Cyrillic, Armenian
        || (cp >= 0x0900 && cp <= 0x1FFF)      // Indic, SE Asian, Greek ext.
        || (cp >= 0x2C00 && cp <= 0xD7FF)      // Georgian, CJK, Hangul
        || (cp >= 0xF900 && cp <= 0xFB17)      // CJK compatibility
        || (cp >= 0x10000 && cp <= 0x107FF)    // Linear B, Aegean
        || (cp >= 0x11000 && cp <= 0x1E7FF)    // Brahmic, musical, CJK ext.
        || (cp >= 0x20000);                    // CJK extensions B and later
    // Emoji and symbols (U+1F300-U+1FAFF) are class ON and fall through as
    // neutral, so a paragraph opening with one resolves on the text after it.
}

/// Index just past `needle` at or after `from`, or SIZE_MAX if absent.
static size_t mp_find(const uint8_t *data, size_t size, size_t from,
                      const char *needle)
{
    size_t len = strlen(needle);
    if (len == 0 || size < len)
        return SIZE_MAX;
    for (size_t i = from; i + len <= size; i++) {
        if (memcmp(data + i, needle, len) == 0)
            return i + len;
    }
    return SIZE_MAX;
}

/**
 * Index just past the run of `delim` closing the run that starts at `from`,
 * or SIZE_MAX if it is never closed.
 *
 * An unclosed delimiter must not swallow the block: `$5 לחודש` is prose about
 * money, not maths, and consuming to the end of it would strand the resolver
 * on the fallback while the renderer resolved on the Hebrew.
 */
static size_t mp_close_fence(const uint8_t *data, size_t size, size_t from,
                             uint8_t delim)
{
    size_t open = 0;
    size_t i = from;
    while (i < size && data[i] == delim) { i++; open++; }
    while (i < size) {
        if (data[i] != delim) { i++; continue; }
        size_t close = 0;
        while (i < size && data[i] == delim) { i++; close++; }
        if (close >= open)
            return i;
    }
    return SIZE_MAX;
}

/**
 * Skips one HTML tag, and for <code> and <pre> the element's contents too.
 * hoedown passes author-written HTML through untouched (neither
 * HOEDOWN_HTML_ESCAPE nor HOEDOWN_HTML_SKIP_HTML is set), so raw tags reach
 * both surfaces and must be skipped in both.
 */
static size_t mp_skip_tag(const uint8_t *data, size_t size, size_t from)
{
    size_t after = mp_find(data, size, from, ">");
    if (after == SIZE_MAX)
        return SIZE_MAX;

    size_t name = from + 1;
    if (name < size && data[name] == '/')
        return after;
    if (name + 4 <= size && memcmp(data + name, "code", 4) == 0) {
        size_t end = mp_find(data, size, after, "</code>");
        return end == SIZE_MAX ? after : end;
    }
    if (name + 3 <= size && memcmp(data + name, "pre", 3) == 0) {
        size_t end = mp_find(data, size, after, "</pre>");
        return end == SIZE_MAX ? after : end;
    }
    return after;
}

/// Skips a well-formed entity reference. `&` alone is literal text.
static size_t mp_skip_entity(const uint8_t *data, size_t size, size_t from)
{
    size_t i = from + 1;
    if (i < size && data[i] == '#')
        i++;
    size_t start = i;
    while (i < size && i - start < 8
           && ((data[i] >= '0' && data[i] <= '9')
               || (data[i] >= 'a' && data[i] <= 'z')
               || (data[i] >= 'A' && data[i] <= 'Z')))
        i++;
    if (i > start && i < size && data[i] == ';')
        return i + 1;
    return SIZE_MAX;
}

/**
 * Skips whatever contributes no direction at `*i`, and reports whether it
 * moved. Both modes skip markup, entities and maths: hoedown emits maths
 * delimiters verbatim for MathJax to process in the browser, so `\(x\)`
 * survives into the HTML exactly as it was written. Only code spans and link
 * targets differ, being Markdown syntax that the renderer has consumed.
 */
static int mp_skip_neutral(const uint8_t *data, size_t size, size_t *i,
                           MPBidiScanMode mode)
{
    uint8_t c = data[*i];
    size_t next = SIZE_MAX;

    if (c == '<')
        next = mp_skip_tag(data, size, *i);
    else if (c == '&')
        next = mp_skip_entity(data, size, *i);
    else if (c == '$')
        next = mp_close_fence(data, size, *i, '$');
    else if (c == '\\' && *i + 1 < size
             && (data[*i + 1] == '(' || data[*i + 1] == '['))
        next = mp_find(data, size, *i + 2,
                       data[*i + 1] == '(' ? "\\)" : "\\]");
    else if (mode == MPBidiScanMarkdown) {
        if (c == '`')
            next = mp_close_fence(data, size, *i, '`');
        else if (c == '~' && *i + 2 < size
                 && data[*i + 1] == '~' && data[*i + 2] == '~')
            next = mp_close_fence(data, size, *i, '~');
        else if (c == ']' && *i + 1 < size
                 && (data[*i + 1] == '(' || data[*i + 1] == '['))
            next = mp_find(data, size, *i + 2,
                           data[*i + 1] == '(' ? ")" : "]");
        else if (c == '!' && *i + 1 < size && data[*i + 1] == '[') {
            // Stop before the bracket so the target is skipped next pass.
            size_t j = *i + 2;
            while (j < size && data[j] != ']')
                j++;
            next = (j < size) ? j : SIZE_MAX;
        }
    }

    if (next == SIZE_MAX || next <= *i)
        return 0;
    *i = next;
    return 1;
}

MPBidiDirection MPBidiResolveDirection(const uint8_t *data, size_t size,
                                       MPBidiScanMode mode,
                                       MPBidiDirection fallback)
{
    if (!data || size == 0)
        return fallback;

    size_t i = 0;
    while (i < size) {
        if (mp_skip_neutral(data, size, &i, mode))
            continue;

        uint32_t cp = mp_next(data, size, &i);
        if (mp_is_rtl(cp))
            return MPBidiDirectionRTL;
        if (mp_is_ltr(cp))
            return MPBidiDirectionLTR;
    }

    return fallback;
}
