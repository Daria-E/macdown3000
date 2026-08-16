//
//  MPBidi.h
//  MacDown 3000
//
//  Resolves the base writing direction of a block of text. Shared by the
//  editor, which scans Markdown source, and the renderer, which scans the
//  HTML hoedown has already produced, so that the two panes cannot disagree.
//

#ifndef MacDown_MPBidi_h
#define MacDown_MPBidi_h

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MPBidiDirectionLTR = 0,
    MPBidiDirectionRTL = 1,
} MPBidiDirection;

typedef enum {
    /// Raw Markdown source. Additionally skips code spans and fences and the
    /// target halves of links and images — syntax the renderer has consumed
    /// by the time it sees the block.
    MPBidiScanMarkdown = 0,

    /// Rendered HTML. Additionally skips the contents of <code> and <pre>,
    /// which in Markdown source were delimited by backticks instead.
    MPBidiScanHTML = 1,
} MPBidiScanMode;

/**
 * Resolves the base direction of `size` bytes of UTF-8 at `data`.
 *
 * Both modes skip markup, entities and maths. hoedown neither escapes nor
 * strips author-written HTML, and it emits maths delimiters verbatim for
 * MathJax to process in the browser, so raw tags and `\(x\)` reach the
 * rendered output unchanged and must be skipped on both sides. Only code
 * spans and link targets are mode-specific.
 *
 * Follows UAX#9 P2/P3 — the first strong directional character wins — with
 * three deliberate departures, so that a Hebrew paragraph opening with a Latin
 * identifier, an equation or an image path still resolves right-to-left:
 *
 *   - an explicit direction mark (U+200E, U+200F, U+061C) outranks content;
 *   - code, maths, and link and image targets are skipped;
 *   - markup is skipped, per `mode`.
 *
 * Returns `fallback` when the text holds nothing strong. Every caller must
 * pass the same fallback, or a punctuation-only block resolves differently in
 * each pane.
 */
MPBidiDirection MPBidiResolveDirection(const uint8_t *data, size_t size,
                                       MPBidiScanMode mode,
                                       MPBidiDirection fallback);

/**
 * The direction every caller falls back to when a block holds no strong
 * character. Exposed so the two surfaces cannot drift apart.
 */
extern const MPBidiDirection MPBidiDefaultFallback;

#endif
