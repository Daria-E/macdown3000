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
        // Exactly, not at least. char_codespan stops at the opening run's
        // length and leaves the surplus as text, and a lone '$' must not pair
        // with the first half of a later '$$'.
        if (close == open)
            return i;
    }
    return SIZE_MAX;
}

/**
 * Skips one HTML tag, and for <code> and <pre> the element's contents too.
 * hoedown passes author-written HTML through untouched (neither
 * HOEDOWN_HTML_ESCAPE nor HOEDOWN_HTML_SKIP_HTML is set), so raw tags reach
 * both surfaces and must be skipped in both.
 *
 * Two things shaped like a tag are not one, and both carry direction:
 *
 *   - An angle autolink. hoedown renders <https://example.com> as a link
 *     whose visible *text* is the URL, so the preview reads those letters
 *     and the editor must too.
 *   - Any other bracketed prose. "1 < 2 שלום > 3" has no tag in it; hoedown
 *     escapes both angles, and skipping between them would strand the editor
 *     on the fallback while the preview resolved on the Hebrew.
 */
static size_t mp_skip_tag(const uint8_t *data, size_t size, size_t from,
                          MPBidiScanMode mode)
{
    size_t after = mp_find(data, size, from, ">");
    if (after == SIZE_MAX)
        return SIZE_MAX;

    size_t name = from + 1;
    if (name >= size)
        return SIZE_MAX;

    // In rendered HTML every '<' opens a tag: hoedown has already escaped any
    // literal angle to &lt;, and an autolink has become a real <a> element
    // whose href legitimately contains "://".
    if (mode == MPBidiScanHTML)
        goto element;

    // Mirror hoedown's tag_length (document.c:426-463): "<", an optional
    // "/", then an alphanumeric — or an HTML comment. Anything else it
    // escapes to &lt;...&gt;, leaving the text visible to the reader, so
    // skipping it here would strand the editor while the preview resolved on
    // the real words. "<!DOCTYPE html>" and "<?php ?>" are prose; "<2 x>" is
    // a tag, however little it looks like one.
    if (name + 3 < size && memcmp(data + name, "!--", 3) == 0)
        goto element;

    size_t start = name;
    if (data[start] == '/')
        start++;
    if (start >= size)
        return SIZE_MAX;
    uint8_t lead = data[start];
    int alnum = (lead >= 'a' && lead <= 'z') || (lead >= 'A' && lead <= 'Z')
                || (lead >= '0' && lead <= '9');
    if (!alnum)
        return SIZE_MAX;

    // Mirror tag_length's autolink detection (document.c:426-463): a scheme
    // of two or more [alnum.+-] then ':', or an address containing '@', with
    // no space, quote or newline reaching the closing angle. hoedown renders
    // those as a link whose visible text is the URL itself, so both surfaces
    // must read it — while "<a href=\"http://x\">" is a plain tag, and
    // "<http://x.com and 5 >" is one too, its autolink aborted by the space.
    size_t stop = after - 1;          /* index of '>' */
    int clean = 1;
    for (size_t j = name; j < stop; j++) {
        uint8_t b = data[j];
        if (b == ' ' || b == '\t' || b == '\n' || b == '"' || b == '\'') {
            clean = 0;
            break;
        }
    }
    if (clean && stop > name) {
        for (size_t j = name; j < stop; j++) {
            if (data[j] == '@')
                return SIZE_MAX;
        }
        size_t j = name, scheme = 0;
        while (j < stop) {
            uint8_t b = data[j];
            int ok = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
                     || (b >= '0' && b <= '9')
                     || b == '.' || b == '+' || b == '-';
            if (!ok)
                break;
            j++; scheme++;
        }
        if (scheme >= 2 && j < stop && data[j] == ':' && j + 1 < stop)
            return SIZE_MAX;
    }

element:
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

/// hoedown's escape_chars (document.c:60-77): the characters a backslash
/// turns into literal text. All are ASCII punctuation, hence neutral.
static int mp_is_escapable(uint8_t c)
{
    return strchr("\\`*_{}[]()#+-.!:|&<>/^~$=\"\'", (char)c) != NULL && c != 0;
}

/// True when `[label]` is defined elsewhere in the document.
static int mp_ref_defined(const uint8_t *label, size_t len,
                          const MPBidiRefs *refs)
{
    return refs && refs->isDefined && len > 0
        && refs->isDefined(label, len, refs->context);
}

/**
 * Handles "[text]", "![alt]" and their reference and inline forms, returning
 * the index to resume at, or SIZE_MAX to scan the construct as plain text.
 *
 * hoedown builds an <img> only when the label resolves, hiding the alt text
 * in an attribute; otherwise it emits the brackets literally and the reader
 * sees every character. Link *text* stays visible either way, so only an
 * image's alt and a resolved reference label are ever skipped.
 */
static size_t mp_skip_bracket(const uint8_t *data, size_t size, size_t from,
                              const MPBidiRefs *refs)
{
    int image = (data[from] == '!');
    size_t open = from + (image ? 2 : 1);
    if (open > size)
        return SIZE_MAX;

    size_t close = open;
    while (close < size && data[close] != ']')
        close++;
    if (close >= size)
        return SIZE_MAX;

    size_t after = close + 1;
    const uint8_t *label = data + open;
    size_t labelLen = close - open;

    // Inline form: the target is hidden, and so is an image's alt.
    if (after < size && data[after] == '(') {
        size_t target = mp_find(data, size, after + 1, ")");
        if (target == SIZE_MAX)
            return SIZE_MAX;
        return image ? target : open;   // link text stays visible
    }

    // Reference form: an empty label collapses onto the bracket text.
    if (after < size && data[after] == '[') {
        size_t refEnd = after + 1;
        while (refEnd < size && data[refEnd] != ']')
            refEnd++;
        if (refEnd >= size)
            return SIZE_MAX;
        const uint8_t *name = data + after + 1;
        size_t nameLen = refEnd - after - 1;
        if (nameLen == 0) { name = label; nameLen = labelLen; }
        if (!mp_ref_defined(name, nameLen, refs))
            return SIZE_MAX;            // rendered literally; read it
        return image ? refEnd + 1 : open;
    }

    // Shortcut form.
    if (!mp_ref_defined(label, labelLen, refs))
        return SIZE_MAX;
    return image ? after : open;
}

/**
 * Skips whatever contributes no direction at `*i`, and reports whether it
 * moved. Both modes skip markup, entities and maths: hoedown emits maths
 * delimiters verbatim for MathJax to process in the browser, so `\(x\)`
 * survives into the HTML exactly as it was written. Only code spans and link
 * targets differ, being Markdown syntax that the renderer has consumed.
 */
static int mp_skip_neutral(const uint8_t *data, size_t size, size_t *i,
                           MPBidiScanMode mode, const MPBidiRefs *refs)
{
    uint8_t c = data[*i];
    size_t next = SIZE_MAX;

    if (c == '<')
        next = mp_skip_tag(data, size, *i, mode);
    else if (c == '&')
        next = mp_skip_entity(data, size, *i);
    else if (c == '$')
        next = mp_close_fence(data, size, *i, '$');
    else if (mode == MPBidiScanMarkdown && c == '\\' && *i + 1 < size
             && !(data[*i + 1] == '\\' && *i + 2 < size
                  && (data[*i + 2] == '(' || data[*i + 2] == '['))
             && mp_is_escapable(data[*i + 1])) {
        // A backslash escape. hoedown emits the character literally, so the
        // syntax it would otherwise have started never exists: "\`not code\`"
        // is prose about backticks. Every escapable character is ASCII
        // punctuation and therefore neutral, so skipping both bytes is right.
        next = *i + 2;
    }
    else if (c == '\\') {
        // Maths delimiters, whose backslash count differs by side. hoedown's
        // char_escape lists ( ) [ ] among the escapable characters and its
        // maths branch requires \\(, so in source a single \( is an escaped
        // paren rendering as "(" — while rndr_math emits the delimiter with
        // one backslash into the HTML.
        size_t slashes = (mode == MPBidiScanMarkdown) ? 2 : 1;
        size_t open = *i + slashes;
        if (open < size && (data[open] == '(' || data[open] == '[')
            && (slashes == 1 || data[*i + 1] == '\\'))
        {
            const char *close = (data[open] == '(')
                ? (slashes == 2 ? "\\\\)" : "\\)")
                : (slashes == 2 ? "\\\\]" : "\\]");
            next = mp_find(data, size, open + 1, close);
        }
    }
    else if (mode == MPBidiScanMarkdown) {
        if (c == '`')
            next = mp_close_fence(data, size, *i, '`');
        // Links and images. Which parts a reader sees depends on the form:
        // an inline target is always hidden, a reference label only when it
        // resolves, and an image's alt only when the image is really built.
        else if (c == '[' || (c == '!' && *i + 1 < size
                              && data[*i + 1] == '['))
            next = mp_skip_bracket(data, size, *i, refs);
    }

    if (next == SIZE_MAX || next <= *i)
        return 0;
    *i = next;
    return 1;
}

MPBidiDirection MPBidiResolveDirection(const uint8_t *data, size_t size,
                                       MPBidiScanMode mode,
                                       MPBidiDirection fallback,
                                       const MPBidiRefs *refs)
{
    if (!data || size == 0)
        return fallback;

    size_t i = 0;
    while (i < size) {
        if (mp_skip_neutral(data, size, &i, mode, refs))
            continue;

        uint32_t cp = mp_next(data, size, &i);
        if (mp_is_rtl(cp))
            return MPBidiDirectionRTL;
        if (mp_is_ltr(cp))
            return MPBidiDirectionLTR;
    }

    return fallback;
}
