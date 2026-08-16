# Implementation Plan: Manual Per-Paragraph Writing Direction

Depends on [`per-paragraph-rtl.md`](per-paragraph-rtl.md), which resolves direction
automatically from content. This adds the escape hatch for when that resolution is wrong.

It is planned and gated separately because it is the only part of the RTL work that
**rewrites the user's document**.

> **Status: the insertion mechanism is unresolved. Do not implement from this plan yet.**
>
> Three design gates have each found the Unicode-mark mechanism silently destroying a
> different Markdown construct: block detectors at byte 0, the block *inside* a blockquote,
> reference definitions, display maths, task-list checkboxes, and — most recently —
> emphasis. See §Open Problem. Everything else here (the menu, the resolution priority, the
> structural-invariant verification, the known consequences) is sound and survives whatever
> replaces the insertion point.

## Requirement

A user can set any individual paragraph's direction to Left-to-Right or Right-to-Left, or
back to Automatic, and the choice persists in the document. Applying an override must never
change anything about the document except its direction, with the single documented
exception in §Verification (a block opening with a quotation mark loses that quote's
curling).

## Mechanism

**Format ▸ Paragraph ▸ Writing Direction ▸ Automatic / Left-to-Right / Right-to-Left**,
acting on **the block the insertion point is in**, with a checkmark showing that block's
current state. Automatic resolution from content is the default and remains in force for
every block the user has not overridden; this is the escape hatch for the ones it gets
wrong, not the primary mechanism.

Scoping to the caret rather than the selection keeps the command's effect legible — one
block, one mark, one undo step — and sidesteps the mixed-selection checkmark question
entirely. A range selection spanning several blocks is out of scope for now; if it is
wanted later it is a loop over this operation, not a different one.

The override inserts an invisible Unicode direction mark into the block — `U+200F`
RIGHT-TO-LEFT MARK or `U+200E` LEFT-TO-RIGHT MARK — and removes any existing mark.
"Automatic" removes them and lets resolution fall through to content.

The mark is therefore an *input* to `MPBidiResolveDirection`, which already gives explicit
marks priority over content, rather than state stored beside the document. Nothing to
persist, and it round-trips through save, reopen and copy/paste.

## Insertion Point

**The mark goes at the start of the innermost block's content, after every prefix that
hoedown uses to identify the block.** Never at byte 0.

Every one of hoedown's block detectors is anchored at the start of the line:

```c
static int is_atxheader(...) { if (data[0] != '#') return 0; ... }   /* document.c:1484 */
```

`prefix_uli`, `prefix_oli`, `prefix_quote`, `prefix_code`, `is_hrule`, `is_codefence`,
`parse_table` and `parse_htmlblock` are the same. A mark at byte 0 **destroys the block**:
`‏# Xcode כותרת` stops being a heading, renders as a paragraph with a literal `#`, loses
its slug `id` so every `[…](#anchor)` link to it breaks, and drops out of the TOC,
`MPPDFAnchorInjector` and scroll sync — silently.

**The rule is recursive.** `parse_blockquote` (`document.c:1620-1657`) strips the `> `
prefix and re-parses the remainder as blocks, so for `> ## סיכום` the position after `> `
is byte 0 of an inner heading, and inserting there destroys it exactly as before, one level
in. Prefixes are stripped repeatedly until a non-container block is reached, and the mark
goes after all of them.

**A prefix here means anything a parser or renderer matches at a fixed offset — not only
hoedown's block detectors.** MacDown's own `hoedown_patch_render_listitem`
(`hoedown_html_patch.c:132-146`) does `strncmp(text->data + offset, "[ ]", 3)`, so a mark
placed after the bullet of `- [ ] משימה` puts U+200F's three bytes exactly where `[` must
be: the item loses `class="task-list-item"`, loses its `<input type="checkbox">`, renders
the literal text `[ ] משימה`, and stops toggling. Task-list markers are a prefix in every
sense that matters, and are consumed as one.

| Block | Prefix consumed before the mark |
| --- | --- |
| paragraph | none |
| ATX heading | `#`s and the following space, if any |
| setext heading | none (the underline is the next line) |
| list item | the bullet or number and its space, **then any `[ ] ` / `[x] ` task marker** |
| blockquote | `> `, then recurse |
| footnote definition | `[^id]: `, then recurse |
| table cell | none, relative to the cell's content |

Recursion applies to every container, not only blockquotes: a list item may contain a
heading (`- # כותרת`) and a footnote definition may contain a blockquote
(`[^1]: > ציטוט`).

Not overridable, with the menu item disabled when the selection contains only these:

| Block | Why |
| --- | --- |
| fenced code, indented code | no valid position inside the fence line; code is unconditionally LTR anyway |
| hrule, HTML block | no text of ours to direct |
| link reference definition | `[id]: url` renders nothing; a mark makes the definition visible text and strips the href from **every** reference link in the document |

**Plus a content-based exclusion, which is not a block type at all**: any block whose entire
inline content is whitespace followed by `$$…$$`. `document.c:764` computes `displaymode`
with `is_empty_all` over the whole inline buffer of *whatever block is being parsed*, so a
leading mark demotes display maths to inline not only in a paragraph but in
`## $$\int_0^1 f$$`, in `| $$E=mc^2$$ | טקסט |`, and in `[^1]: $$x$$`. Stating this by
block type is what missed those three; it is a property of the content.

Both this and the task-list rule are cases where the *content*, not the block type, decides.
Enumerating types is what let the three previous design gates through, so the implementation
should derive both from the parser's own prefix handling rather than from a list.

When the caret sits in a block that cannot take an override, the menu item is disabled.

## Open Problem: the mark perturbs parsing

Inserting a character into the document text changes how Markdown parses around it. Three
gates have found six instances, and the generalisations offered after each one were too
narrow:

| Broken construct | Why the mark reaches it |
| --- | --- |
| ATX heading, list, blockquote, fence, table at byte 0 | block detectors are line-anchored |
| the block *inside* a blockquote | `parse_blockquote` strips `> ` and re-parses |
| link reference definition | `is_ref` is attempted only at block start |
| display maths | `is_empty_all` over the preceding inline buffer |
| task-list checkbox | `strncmp(text->data + offset, "[ ]", 3)` in MacDown's own renderer patch |
| **emphasis / strong / strike / highlight** | `char_emphasis` (`document.c:780-783`) refuses to open when the preceding byte is not space, `>` or `(`; U+200F ends in `0x8F`. MacDown sets `HOEDOWN_EXT_NO_INTRA_EMPHASIS` by default (`MPPreferences.m:410`, `MPDocument.m:214`) |

Verified: `**important** text` renders `<strong>`; `‏**important** text` renders literal
asterisks. Identical for `*em*`, `~~strike~~` and `==highlight==`.

"After every prefix a parser or renderer matches at a fixed offset" does not describe this
class. Task-lists, smartypants and emphasis all inspect the byte **preceding** them, so the
mark changes the byte before the block's first inline construct as well as its position.
Links, images, code spans, superscript and autolinks were checked and survive.

Two candidate directions, **neither verified**:

1. **Put the mark at the end of the block** and have `MPBidiResolveDirection` treat a mark
   anywhere in the block as the override, rather than relying on first-strong position.
   Nothing follows it, so no leading-byte adjacency applies. Needs checking against closing
   delimiter runs and trailing-context constructs.
2. **Strip marks in preprocessing so they never reach hoedown**, carrying the override to
   the renderer out of band. Removes the entire class at once, but needs a way to associate
   a stripped mark with the block its callback later renders — non-trivial, since hoedown
   callbacks have no block index.

Whichever is chosen must be validated by the structural invariant below against a corpus
that includes every construct in the table above, before this plan returns to its gate.

## Verification

**The core test is a structural invariant, not a list of cases.** Enumerating block types
is what let two gates through: the first missed that a mark at byte 0 breaks headings, the
second missed that "after `> `" breaks the block *inside* a blockquote, and neither
enumeration mentioned reference definitions or display maths.

`MPRTLOverrideTests` — for every fixture in `MacDownTests/Fixtures` plus a corpus covering
each block type **nested inside each container type**, and — because type enumeration is
precisely what failed before — **each content-decided case**: a task-list item, and a
display-maths-only heading, table cell and footnote definition. For every overridable block
in each document:

1. Render the document.
2. Apply the override to that block.
3. Render again.
4. Assert the two HTML trees are identical — same elements, same nesting, same attributes,
   same text — **modulo exactly three normalisations**, and no others.

Compare recursively over the parsed tree, not by string equality, so the assertion is about
structure rather than formatting. Any insertion point that changes an element type, drops
an `id`, demotes display maths to inline, strips an href, or eats a task-list checkbox
fails this automatically, including for cases nobody thought to enumerate.

### The permitted normalisations

Each one is a hole in the only defence against the defect class three gates have found, so
each is enumerated here rather than added later under test pressure:

1. **`dir` attribute values, wherever they appear.** This is the point of the exercise. It
   must extend to *ancestors*: container direction is resolved over concatenated children,
   so overriding one cell can legitimately flip its `<table>`'s `dir` too.
2. **Bidi control characters in text nodes.** hoedown escapes only `& < > " '`, so the mark
   itself reaches the rendered text verbatim — `‏שלום` vs `שלום`. Without this the
   invariant fails on the very first case in every document. Normalise U+200E, U+200F and
   U+061C out of text before comparing.
3. **Quote curling on the overridden block's first quote.** Per §Known Consequences,
   smartypants declines to open a quote after the mark, so `&ldquo;ציטוט&rdquo;` becomes
   `&quot;ציטוט&quot;`. This survives normalisation 2 and would otherwise fail the
   comparison.

Normalisation 3 means the Requirement above ("must never change anything except its
direction") is knowingly not quite met: overriding a block that opens with a quotation mark
also changes that quotation mark. That is the one accepted exception, and it is accepted
rather than hidden.

Supporting tests:

- Applying, then setting back to Automatic, restores the document byte-for-byte.
- Applying twice is idempotent — one mark, not two.
- Switching LTR → RTL replaces the mark rather than accumulating.
- Non-overridable blocks leave the document untouched and disable the menu item.
- The menu item is disabled, and the document untouched, when the caret is in a
  non-overridable block.
- A heading overridden to RTL keeps its slug `id`, so anchor links still resolve. (Covered
  by the invariant, but worth naming since it is the failure the gates found.)

## Known Consequences

- **`smartypants` stops curling the first quote** of an overridden block: `word_boundary`
  (`html_smartypants.c:63`) is false for byte 0x8F, the last byte of U+200F, so
  `smartypants_quotes` declines to open. A block overridden to RTL beginning `"ציטוט"`
  keeps straight quotes.
- **Character counts include the mark.** `MPWordCountTypeCharacter` and `…NoSpaces` count
  it; U+200F is not whitespace. Word counts are unaffected.
- **`editorShowsInvisibleCharacters` will not reveal it.** `NSLayoutManager` substitutes
  glyphs for space, tab and newline only, not bidi controls. Discoverability comes from the
  Format-menu checkmark; more would need custom glyph drawing, which is out of scope.
- **A per-cell table override cannot show in the editor.** The override unit is a table
  cell, but the editor's direction unit is a Markdown block and a table row is one line.
  The preview honours it; the editor shows the row's direction.
- **Portability is limited to MacDown.** A leading strong character determines base
  direction under UAX#9 P2/P3, which is how AppKit's `NSWritingDirectionNatural` behaves —
  but in HTML, base direction comes from CSS `direction` or the `dir` attribute, so
  `<p>‏שלום Hello</p>` renders LTR on GitHub and in browsers. Inside MacDown the computed
  `dir` attribute carries the meaning, and the mark and the attribute never disagree,
  because the mark is only ever an input to the algorithm that writes the attribute.

## Rejected Alternatives

- **Wrapping the block in `<div dir="rtl">`** — intrusive in the source, and hoedown's
  block-HTML handling stops Markdown processing inside it, so the paragraph would stop
  being Markdown.
- **A per-document sidecar of paragraph indices** — breaks on the first edit that inserts
  or removes a block.
- **A new Markdown syntax** — nothing else would understand it, and it would not round-trip.

## Sequencing

Per `CLAUDE.md`'s Rule of Two this plan needs its own **design-review gate** before
implementation, and an **implementation-review gate** covering the full diff from `main`.

It lands after `per-paragraph-rtl.md` Phases 1 and 2, since the override is only meaningful
once automatic resolution and the shared resolver exist.
