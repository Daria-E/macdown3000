# Implementation Plan: Per-Paragraph RTL Support

Automatic per-block direction in the editor and preview. The manual per-paragraph override
is a separate piece of work with its own plan and its own design gate:
[`rtl-manual-override.md`](rtl-manual-override.md). It rewrites the user's document, this
does not, and the two need reviewing on different terms.

## Problem Summary

MacDown 3000 has no bidirectional text support. The reference fork `macdown-with-rtl`
added a whole-document RTL toggle with three defects this plan exists to avoid:

1. **Direction is document-wide.** One `NSMutableParagraphStyle` over the whole editor and
   one `body { direction: rtl }` rule in the preview. Neither can say "this paragraph is
   Hebrew, the next is English."
2. **The preview flickers.** Inherited from old MacDown, whose `renderer:didProduceHTMLOutput:`
   has its DOM-replacement path `#if 0`'d out and always calls `loadHTMLString:`. MacDown
   3000 already fixed this (`MPDocument.m:2133`) with a body-`innerHTML` swap. That fast
   path **preserves `<head>` and discards `<body>` attributes**, so any direction mechanism
   living there would be inert or would force full reloads and bring the flicker back.
3. **Maths and code break.** `direction` and `text-align` are inherited, so a `body` rule
   reaches `<pre>` (where the bidi algorithm reorders brackets and indentation), Prism's
   gutter CSS (physical `left`/`padding-left`), and MathJax's HTML-CSS output (inline-block
   runs with left-based offsets that require an LTR container).

## Design Principle

**Every text-bearing block declares its own direction explicitly.** Direction is resolved
from content by one shared algorithm and written into the markup as `dir="ltr"` or
`dir="rtl"` — never inherited, never `dir="auto"`, never global. Nothing in `<head>` or on
`<body>` changes when direction changes.

Declaring on every block rather than only on RTL ones is the whole simplification: because
`direction` is a CSS inherited property, a block that declares nothing takes its parent's.
An English list item inside a Hebrew list would render right-aligned with its full stop on
the left. Emitting only the difference from the parent would avoid that, but hoedown
renders children before parents, so a callback cannot know its parent's direction without a
post-pass over the rendered HTML or string surgery inside container callbacks. Declaring
unconditionally needs neither, and is correct by construction.

The cost is paid once, in test fixtures (§1.3), not in behaviour or output size.

## Platform Findings

Measured on this machine's AppKit and the legacy WebKit1 `WebView` the preview uses.
Findings 3 and 4 are why direction is computed rather than delegated to `dir="auto"`.

| # | Finding |
|---|---|
| 1 | `baseWritingDirection = Natural` already resolves the bidi base level per paragraph (UAX#9 P2/P3), but `NSTextAlignmentNatural` resolves against the app's UI language, so RTL paragraphs are reordered correctly yet sit flush left. Natural alignment *does* follow an explicitly set `baseWritingDirection`. **The editor gap is alignment, not reordering.** |
| 2 | Per-paragraph `NSParagraphStyle` takes effect in a `richText="NO"` `NSTextView`, which is what `MPDocument.xib` configures. Phase 2's core mechanism works. |
| 3 | **`dir="auto"` on a container is defeated by `dir` on its children.** HTML auto-directionality skips descendants carrying their own `dir`, so `<ul dir="auto">` containing `<li dir="auto">` resolves **LTR**. |
| 4 | **`:dir()` requires WebKit 16.4+.** `MACOSX_DEPLOYMENT_TARGET` is 11.0, where WebKit predates it. `:dir()` is unusable; `[dir="rtl"]` attribute selectors are not. |
| 5 | Logical properties (`padding-inline-start`, `border-inline-start`) and `unicode-bidi: isolate` are supported. |
| 6 | `direction: ltr` on a descendant overrides an RTL ancestor. |
| 7 | `unicode-bidi: plaintext` is parsed but does not resolve direction. |

## Direction Resolution

One function, `MPBidiResolveDirection(text, fallback)`, in C so the hoedown patch,
Objective-C and Quick Look can all call it. **Every caller passes the same `fallback`
constant**, or a punctuation-only block resolves differently per surface.

```
for each character:
    U+200F RLM, U+061C ALM  -> RTL     (explicit override; see rtl-manual-override.md)
    U+200E LRM              -> LTR     (explicit override)
    strong RTL (Hebrew, Arabic, Syriac, Thaana, N'Ko, Samaritan,
                Mandaic, Adlam, Arabic Presentation Forms)
                            -> RTL
    strong LTR              -> LTR
    otherwise               -> continue
-> fallback
```

Deliberate departures from a browser's `dir="auto"`:

- **Code and maths are skipped** — inline spans, fenced and indented blocks, and maths
  delimiters. A Hebrew paragraph opening with `` `NSString` `` or `\(x\)` resolves RTL,
  where a browser would say LTR. Without it, a Hebrew list item containing a code sample
  with English identifiers flips the whole item LTR.
- **Markup is skipped.** The preview scans already-rendered HTML, so tags, attribute values
  and entities must not contribute; `<em>שלום</em>` resolves RTL on the `ש`.
- **Link and image targets are skipped.** In `![](logo.png) שלום עולם` the editor would
  otherwise scan `logo.png`, hit a strong `l` and resolve LTR, while the preview sees the
  path as an attribute value, skips it, and resolves RTL on `ש` — the same paragraph
  laid out oppositely in the two panes. The editor's scanner must skip `](…)` targets,
  `][…]` reference labels and image `alt` text, mirroring what the renderer never sees.

### Keeping the two surfaces in agreement

A shared leaf function is not sufficient, because the two callers see different text. Three
rules close the gap:

1. **One block splitter, used by both.** Its job is to reproduce **hoedown's** block
   boundaries — not "consecutive non-blank lines", and not merely the delta introduced by
   preprocessing.
2. **The splitter is specified against `parse_block`, recursively** (`document.c:2449-2505`)
   — the document's block *sequence*, not one paragraph's continuation rules. Containers
   split their children with the container prefix stripped: `parse_list` yields one block
   per item, `parse_blockquote` re-parses its stripped content into blocks, and
   `parse_footnote_list` does the same. `parse_paragraph`'s continuation rules apply
   *inside* that walk, ending an open paragraph at whichever comes first
   (`document.c:1671-1688`):

   | Break | Detector |
   | --- | --- |
   | blank line | `is_empty` |
   | setext underline | `is_headerline` — note the emit branch (`document.c:1694-1736`) renders lines 1..n-1 as a `<p>` **and** line n as the header, so `טקסט` / `Hello Heading` / `====` is *two* blocks, not one |
   | ATX heading line | `is_atxheader` (MacDown never sets `HOEDOWN_EXT_SPACE_HEADERS`, so `#hashtag` counts) |
   | horizontal rule | `is_hrule` |
   | blockquote line | `prefix_quote` |

   Specifying only the continuation rules is not enough, and the difference is visible in
   the cases this feature exists for. `- פריט ראשון` / `- Second item` renders two
   `dir`-bearing `<li>`s, but no continuation rule fires between them — a
   continuation-only splitter yields one block and the editor lays the English item out
   RTL. Likewise `> שלום עולם` / `>` / `> Hello world` is two `<p>`s inside one
   blockquote: a bare `>` line is not `is_empty`, so only the recursive container walk
   separates them.

   Also note what is **not** a paragraph break: hoedown does not interrupt an open
   paragraph at a list marker or a fence. Those two breaks exist only because
   `MPPreprocessMarkdown` inserts a blank line first, and each applies under narrower
   conditions than "always":

   - **#254, list** (`MPRenderer.m:120`): the marker must be at **column 0**, and the
     preceding line must not itself be a list line. `טקסט עברי` followed by `  * Item`
     (indented) does **not** get a blank line, so the preview renders one RTL paragraph
     containing both lines — and a splitter that breaks on any list marker would disagree.
   - **#36, fence** (`MPRenderer.m:131`): the preceding line must begin with a non-space,
     and only opening fences match.

3. **The splitter never mutates text.** `MPPreprocessMarkdown` *inserts characters*, so
   running it in the editor and splitting its output would produce ranges in preprocessed
   coordinates while `paragraphStyleProvider` applies them to `NSTextStorage`; every
   insertion would shift subsequent blocks and the stamp would land on the wrong paragraph.
   The splitter encodes the boundary conditions above and returns ranges in original
   coordinates. The remaining rewrites cannot move a boundary: `[text] [ref]` → `[text][]
   [ref]` (#25) and the `​` inside fences (#37) are within-block, and CRLF normalisation
   (#382) changes offsets only, since `\r\n` is a single paragraph separator to
   `-[NSString paragraphRangeForRange:]` either way. A `\r` reaching the resolver is
   neutral.
4. **Definitions are extracted before parsing.** `hoedown_document_render`
   (`document.c:2874-2896`) removes every reference definition (`is_ref`) and footnote
   definition (`is_footnote`) line from the text *before* `parse_block` runs, then
   re-emits footnote definitions at the end of the document via `parse_footnote_list`
   (`:2915`). Two consequences the splitter and the agreement test must both encode:

   - A reference definition is a block to the editor and produces **no** HTML leaf.
     Removing the line also leaves its newline behind, so `טקסט עברי` / `[ref]: …` /
     `Hello world` renders as **two** paragraphs where a splitter unaware of the
     extraction sees one. `MacDownTests/Fixtures/links.md` and `regression-issue25.md`
     already contain runs of these.
   - Footnote definitions are hoisted to the document end, so a mid-document `[^1]: …`
     appears in a different position among the leaves than among the blocks. An
     *unreferenced* definition produces no leaf at all: `parse_footnote_list` walks
     `footnotes_used` (`:2915`).

5. **The splitter takes the document's extension flags.** `[preferences extensionFlags]`
   drives both renderers (`MPDocument.m:207-229`), so footnotes, tables and fenced code can
   each be off. With footnotes off, `[^1]: …` stays in the document flow for the renderer
   while rule 4's extraction would remove it for the splitter. Pass the same flags the
   document passes to `hoedown_document_new`.

6. **Entity handling must match.** For `1 < 2 ולכן זה נכון` the editor sees a literal `<`
   and the renderer sees `&lt;`. An unterminated `<` is literal text, not a tag opener,
   or the editor consumes to end of block and falls back to LTR while the renderer resolves
   RTL.

A corpus test asserting the two surfaces agree on every fixture is the guard for all three
(§Testing).

## Phase 1 — Preview

### 1.1 Declare `dir` on every text-bearing block

`MacDown/Code/Extension/hoedown_html_patch.c` already replaces five callbacks
(`blockcode`, `listitem`, `header`, `table_header`, and `toc_header` via
`MPCreateHTMLTOCRenderer`); this extends that pattern.

| Callback | Change |
| --- | --- |
| `paragraph` | new — `<p dir="…">`. Must reproduce `rndr_paragraph` exactly: leading-`isspace` skip, both empty/whitespace-only early returns, and the `HOEDOWN_HTML_HARD_WRAP` branch, whose `rndr_linebreak` is `static` in `html.c` and must be reimplemented honouring `USE_XHTML` |
| `blockquote` | new — resolved over its own content |
| `list` | new — `<ul dir="…">` / `<ol dir="…">`, resolved over the whole list so markers sit on the correct side |
| `table_cell` | new — emits **both `<td>` and `<th>`**, which share this callback; preserve hoedown's existing `style="text-align: …"` |
| `table` | new — resolved over the whole table, so a Hebrew table gets RTL column order |
| `footnote_def` | new — `<li dir="…">` |
| `header` | extend — `dir` alongside the slug `id` |
| `listitem` | extend — `dir` on `<li>`, task-list branch intact |
| `blockcode` | extend — always `dir="ltr"` on `<pre>`; code never resolves RTL |
| `toc_header` | extend — so TOC entries align with their headings |

Two known gaps, both acceptable and both to be stated in the code rather than discovered:

- **Raw HTML blocks** (`rndr_raw_block`) carry user text and get no `dir`, so a `<div>` of
  English prose inside a Hebrew blockquote inherits RTL. The user can write
  `<div dir="ltr">`; we do not rewrite their HTML.
- **Container resolution is first-strong over concatenated children.** An otherwise-English
  table whose first cell is Hebrew resolves RTL and reverses its column order. Correct by
  the stated rule, surprising in that instance.

### 1.2 Add `MacDown/Resources/Extensions/bidi.css`

Static and direction-independent, so it can sit permanently in `<head>` without ever
invalidating the DOM fast path. `MacDown/Resources/Extensions` is a **folder reference**
(`project.pbxproj:543`), so dropping the file in requires no project edit. The preview CSP
(`style-src 'self' 'unsafe-inline' file:`) already permits it.

```css
/* Code is always LTR: bidi reordering scrambles brackets and indentation. */
pre, pre code, code, kbd, samp, tt {
    direction: ltr;
    text-align: left;
    unicode-bidi: isolate;
}

/* Prism's line-numbers and show-language plugins position the gutter and the
   language label with physical offsets. */
pre.line-numbers, div.prism-show-language { direction: ltr; }

/* MathJax's HTML-CSS output is a run of inline-block spans with left-based
   offsets and requires an LTR container. */
.MathJax, .MathJax_Display, .MathJax_Preview, .MathJax_MathML {
    direction: ltr !important;
}
.MathJax_Display { text-align: center; }

/* Diagram output is generated LTR. */
.mermaid, .mermaid svg, .graphviz, .graphviz svg { direction: ltr; }

/* Theme stylesheets indent and rule with physical properties. Attribute
   selectors, not :dir(), because the deployment target predates :dir(). */
blockquote[dir="rtl"] {
    border-left: 0;
    border-right: 4px solid #ddd;
    padding-left: 0;
    padding-right: 15px;
}
ul[dir="rtl"], ol[dir="rtl"] { padding-left: 0; padding-right: 30px; }
```

Values need checking against each of the 11 bundled themes; those above are `GitHub2.css`'s.

Register **last** in `-[MPRenderer stylesheets]`, after `export.css`, so it beats the
themes. In `HTMLForExportWithStyles:highlighting:` register it outside the `if (withStyles)`
gate **and force `stylesOption` to `MPAssetEmbedded`** — otherwise `stylesOption` stays
`MPAssetNone` (`MPRenderer.m:884`) and `-[MPAsset htmlForOption:]` (`MPAsset.m:69-95`)
drops the sheet, shipping an export with `dir` attributes and no LTR island. Print and PDF
need no separate handling: `printOperationWithSettings:` (`MPDocument.m:1486`) prints the
live preview frame, which already carries the sheet.

### 1.3 Test fixtures

Declaring on every block changes rendered output for every document, so this is real churn,
planned for rather than hoped away:

- **Regenerate all 28 goldens** with the existing `REGENERATE_GOLDEN_FILES` flag, in a
  **separate commit** from the behaviour change, so the reviewer can read the real diff.
- **Update — do not relax — the roughly 44 hand-written tag assertions** across
  `MPMarkdownRenderingTests.m`, `MPRendererEdgeCaseTests.m`, `MPHTMLExportTests.m`,
  `MPSyntaxHighlightingTests.m`, `MPDocumentLifecycleTests.m`, `MPHTMLTabularizeTests.m`
  and `MPQuickLookRendererTests.m`. `containsString:@"<li>Item 1</li>"` becomes
  `containsString:@"<li dir=\"ltr\">Item 1</li>"`, **not** `containsString:@"Item 1"` —
  the latter would leave a test that cannot fail if list rendering broke.

**After Phase 1, issue 3 is fixed and issue 1 is fixed in the preview.** Issue 2 cannot
regress: `<head>` never changes, so every render still takes the fast path.

## Phase 2 — Editor

### 2.1 Resolve per Markdown block

A hard-wrapped Markdown paragraph is several `NSString` paragraphs; resolving per line
splits one logical paragraph down the middle and disagrees with the preview. The unit is
the Markdown block, from the shared splitter, in original text-storage coordinates.

`clearHighlightingForRange:` is called with the *visible* range, which starts mid-paragraph,
so the range must be expanded with `-[NSString paragraphRangeForRange:]` before resolving.
**Expand only for the paragraph-style stamp** — that method also clears font traits,
background, link and foreground colour, and widening the whole clear would strip syntax
colour above the visible range that `applyHighlighting:withRange:` will not repaint.

### 2.2 Stop the highlighter flattening paragraph styles

`clearHighlightingForRange:` (`HGMarkdownHighlighter.m:216`) stamps one document-wide
paragraph style — from `targetTextView.defaultParagraphStyle` — over every re-highlighted
range on every parse and every scroll. Any per-paragraph style is erased on the next
keystroke. `peg-markdown-highlight` is vendored in-tree (only `Dependency/prism` is a
submodule) and already has a test file, so it is project code.

Add an opt-in hook:

```objc
@property (copy) NSParagraphStyle *(^paragraphStyleProvider)(NSRange paragraphRange);
```

When set, stamp the provider's result per block; when `nil`, behaviour is byte-for-byte
what it is today.

**Two call sites, not one.** `applyVisibleRangeHighlighting` also does
`setTypingAttributes:self.defaultTypingAttributes` unconditionally (`:381`) after every
parse *and every scroll*, which fights per-paragraph typing attributes.

### 2.3 Supply the provider from `MPDocument`

Two cached immutable `NSParagraphStyle`s built from `applyEditorFontAndParagraphStyle`'s
base style, differing only in `baseWritingDirection` (explicit LTR vs RTL), with `alignment`
left `Natural` so it follows. The base style carries 100 tab stops at `NSTextAlignmentLeft`
with fixed locations (`MPDocument.m:3556-3568`); the RTL variant inherits them unchanged,
which is acceptable but should be eyeballed.

The cache-staleness path to guard is **zoom**: `applyCurrentZoom` →
`applyEditorFontAndParagraphStyle` (`MPDocument.m:3590-3593`) rebuilds the base style and
never re-reads the highlighter's `defaultTypingAttributes`. The theme path already calls
`readClearTextStylesFromTextView` immediately afterwards (`MPDocument.m:3355`).

Typing attributes must stay matched to the caret's block, so typing the first Hebrew
character of a new paragraph doesn't leave the caret in the LTR style.

### 2.4 Keep code blocks LTR in the editor

Fenced and indented code inside an RTL document stays LTR, as in the preview. The
highlighter knows these ranges — pmh's `Code` rule covers ``` and `~~~` fences and
`Verbatim` covers indented code — and the provider forces LTR for blocks **contained in**
one. Containment, not intersection: `pmh_CODE` also matches inline spans and `$…$`
(`pmh_grammar.leg:596-605`), so an intersection test would flip
`שלום ` + `` `NSString` `` + ` עולם` to LTR in the editor while the preview keeps
`<p dir="rtl">`, contradicting §Direction Resolution.

**After Phase 2, issue 1 is fixed in the editor.**

## Phase 3 — Quick Look

`MacDownCore/MPQuickLookRenderer.m` is a **separate** hoedown wiring with its own callbacks
(`:513`) and its own `<head>` (`:546`). Left alone, Quick Look of a Hebrew file shows every
paragraph LTR while the app preview shows it RTL — a visible inconsistency on the same
document, so this is in scope rather than a non-goal.

Share the resolver and the patched callbacks, and add the `bidi.css` rules to
`embeddedStyles`. Quick Look disables MathJax, so only the code and theme rules apply.

It also carries **its own copy of `MPPreprocessMarkdown`** (`:156-177`), and that copy has
**already drifted**: it implements only the #36 fence rule — no #254 list rule, no #25, no
#37, no CRLF normalisation. So `שלום עולם` followed by `- פריט` is a paragraph plus a list
in the preview and a single paragraph in Quick Look, today, before any RTL work.

Bringing that copy to parity is therefore part of this phase, not an assumption of it.
Until it is done, "Quick Look matches both" cannot hold and the corpus agreement test
cannot be extended to the third surface. Sharing one implementation is preferable to
re-synchronising two.

## Testing

New:

- `MPBidiTests` — the resolver: Hebrew, Arabic, Latin, digits-only, punctuation-only,
  empty, emoji-leading, leading inline code, leading maths, leading block code, leading
  image, a link whose target is Latin and whose text is Hebrew, explicit RLM and LRM,
  markup-only content, an unterminated `<`, and Markdown markers (`> `, `- `, `#`) leading
  the line.
- `MPRTLRenderingTests` — `dir` present on every block type in the §1.1 table with the
  right value for RTL and LTR content; `<th>` and `<td>` both covered; an **LTR block
  nested in an RTL container** keeps `dir="ltr"`; table cells keep their `text-align`;
  task-list items keep their checkbox markup.
- **`MPBidiAgreementTests` — the two surfaces agree.** For every fixture in
  `MacDownTests/Fixtures` plus a mixed-direction corpus, walk the editor's block splitter
  over the source and compare against the rendered HTML's `dir` attributes. This is the
  guard for every rule in §"Keeping the two surfaces in agreement".

  The comparison needs an explicit projection, because §1.1 deliberately puts `dir` on
  containers as well as leaves and the two granularities differ. **Compare the sequence of
  `dir` values on text-bearing *leaf* blocks — `p`, `h1`–`h6`, `li` — in document order,
  against the splitter's block sequence**, after three exclusions:

  - **Container elements** (`ul`, `ol`, `blockquote`, `table`) have no editor counterpart.
  - **Tables entirely.** A 3×4 table is 13 `dir`-bearing elements against at most 4 editor
    blocks, since a table row is a single editor line. That mismatch is inherent — per-cell
    direction cannot show in the editor — and is stated as a limitation rather than papered
    over. Per-cell correctness is covered by `MPRTLRenderingTests` instead.
  - **Reference and footnote definitions**, per rule 4 above: the first produces no leaf,
    and the second is hoisted out of source order. Both are compared separately, by
    identity rather than by position.

  (`dd` is not in the leaf set: hoedown emits no definition lists.)

  Extend to Quick Look's renderer in Phase 3, once its preprocessing is at parity.
- Extend `HGMarkdownHighlighterTests` — the provider is consulted per block, and a `nil`
  provider preserves current behaviour. Assert by **pointer identity** against
  `defaultTypingAttributes[NSParagraphStyleAttributeName]` or by recording call ranges;
  asserting the stamped style merely *equals* `defaultParagraphStyle` passes whether or not
  the provider branch exists.
- **A block edited from Hebrew to English loses its RTL paragraph style** — the removal
  half of §2.2, which nothing else covers.
- **A Hebrew paragraph still carries `NSWritingDirectionRightToLeft` after a zoom change**,
  covering the §2.3 staleness path.
- `MPHTMLExportTests` — `bidi.css` embedded in exports **with styles off as well as on**.

Regression (must still pass unchanged): `MPMathJaxScrollTests`, `MPRenderDeferralTests`,
`MPScrollSyncTests`.

Manual, on a document mixing Hebrew and English paragraphs, a hard-wrapped Hebrew
paragraph, a paragraph opening with inline code, a paragraph opening with an image, a
Hebrew list containing an English item, a fenced code block, inline and display maths, a
table, and nested lists:

1. Typing in an RTL paragraph does not flicker or re-align the preview.
2. Code renders LTR with the Prism gutter attached.
3. Display maths is centred and reads left-to-right inside an RTL paragraph.
4. Right-arrow moves logically backward inside an RTL paragraph — correct, but a visible
   behaviour change worth confirming deliberately.
5. Scroll sync still tracks across mixed-direction content.
6. Export to HTML and PDF match the preview; Quick Look matches both.

## Non-Goals

- No `body { direction: rtl }` and no `text-align` on `body`.
- No mutation of `<head>` when direction changes — that is what would reintroduce flicker.
- No global editor RTL mode as the primary mechanism.
- No new Markdown syntax.
- No rewriting of user-authored raw HTML blocks.
- The manual override is out of scope here; see `rtl-manual-override.md`.

## Risks

- **Theme CSS tuning.** 11 bundled themes use physical properties; the `[dir="rtl"]`
  overrides need checking against each. Most likely source of visual polish bugs, least
  likely to be caught by tests.
- **Fixture churn is large and mechanical.** Kept to its own commit, with assertions
  updated rather than relaxed, so it cannot hide a real regression.
- **We own a bidi heuristic.** `dir="auto"` is unavailable (finding 3), so the algorithm is
  ours. Mitigated by it being a pure, heavily-tested function used by every surface.
- **Modifying vendored `HGMarkdownHighlighter`.** Two methods, both gated behind an opt-in
  property; the `nil` path must stay identical.
- **Hoedown callback fidelity.** `rndr_paragraph` in particular has non-obvious behaviour
  that must be reproduced exactly.

## Sequencing

Per `CLAUDE.md`'s Rule of Two, this plan is the artifact for the **design-review gate**;
the **implementation-review gate** then covers the full diff from `main`.

Phases land separately. Phase 1 alone is shippable: it fixes issue 3 outright, fixes issue
1 in the preview, and cannot regress issue 2.

## Build Environment

Xcode, CocoaPods and the `Dependency/prism` submodule are installed. Local runs need
`CI=true`: `Tools/update_build_number.sh` runs PlistBuddy against an `Info.plist` a clean
build has not yet placed in the bundle, under `set -o errexit`, and CI never hits it
because the script's first act is a `CI=true` early exit.

Baseline on this machine is **2 failures**, not 0 —
`MPQuickLookRendererTests.testRenderIncludesCSSStyles` and `.testUsesConfiguredStyle` fail
on unmodified `main`. `MPZoomTests.testZoomChangeInDocAPropagatesToDocB` is order-dependent
and flakes in full-suite runs.

The MathJax font work (`availableFonts: []`, `preferredFont: null`, MathJax origin in
`font-src`) is on a separate branch and not yet in `main`. It is independent of RTL: it
fixes which faces MathJax uses, not how its output behaves inside an RTL container, so the
LTR island above is still required.
