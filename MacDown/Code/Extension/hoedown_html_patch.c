//
//  hoedown_html_patch.c
//  MacDown 3000
//
//  Created by Tzu-ping Chung  on 14/06/2014.
//  Copyright (c) 2014 Tzu-ping Chung . All rights reserved.
//

#include <ctype.h>
#include <string.h>
#include <hoedown/escape.h>
#include <hoedown/document.h>
#include <hoedown/html.h>
#include "hoedown_html_patch.h"
#include "MPBidi.h"

#define USE_XHTML(opt) (opt->flags & HOEDOWN_HTML_USE_XHTML)
#define USE_BLOCKCODE_INFORMATION(opt) \
    (opt->flags & HOEDOWN_HTML_BLOCKCODE_INFORMATION)
#define USE_TASK_LIST(opt) (opt->flags & HOEDOWN_HTML_USE_TASK_LIST)

// Global checkbox index counter for interactive checkbox support.
// NOTE: This counter is NOT thread-safe. Markdown rendering must be serialized
// on a single thread (which MacDown does via the main thread).
// Related to GitHub issue #269.
static int g_checkbox_index = 0;

void hoedown_patch_reset_checkbox_index(void)
{
    g_checkbox_index = 0;
}

// hoedown_buffer_new() stores its argument as the buffer's growth "unit", and
// hoedown_buffer_grow() asserts that unit is non-zero. Passing a size hint of 0
// (e.g. when a heading or code-fence info string is empty) therefore produces a
// buffer that aborts the process the first time anything is written to it. Clamp
// the hint so a derived buffer is always growable. Related to GitHub issue #479.
static hoedown_buffer *new_growable_buffer(size_t size_hint)
{
    return hoedown_buffer_new(size_hint ? size_hint : 16);
}

int hoedown_patch_get_checkbox_index(void)
{
    return g_checkbox_index;
}

// Writes ` dir="ltr"` or ` dir="rtl"` for the base writing direction of a
// block's rendered content. Every text-bearing block declares its direction
// explicitly rather than inheriting it, so a Hebrew paragraph beside an English
// one lays out correctly and neither reaches into code or maths. The renderer
// scans hoedown's already-produced HTML, so it passes MPBidiScanHTML and no
// reference table — reference labels are settled by the time output exists.
static void put_bidi_dir(hoedown_buffer *ob, const uint8_t *data, size_t size)
{
    MPBidiDirection dir = MPBidiResolveDirection(data, size, MPBidiScanHTML,
                                                 MPBidiDefaultFallback, NULL);
    hoedown_buffer_puts(ob, dir == MPBidiDirectionRTL ? " dir=\"rtl\""
                                                      : " dir=\"ltr\"");
}

// rndr_linebreak from html.c, which is static there. Reproduced so the
// paragraph callback's HOEDOWN_HTML_HARD_WRAP branch honours USE_XHTML.
static void put_linebreak(hoedown_buffer *ob,
                          const hoedown_html_renderer_state *state)
{
    hoedown_buffer_puts(ob, USE_XHTML(state) ? "<br/>\n" : "<br>\n");
}

// rndr_blockcode from HEAD. The "language-" prefix in class in needed to make
// the HTML compatible with Prism.
void hoedown_patch_render_blockcode(
    hoedown_buffer *ob, const hoedown_buffer *text, const hoedown_buffer *lang,
    const hoedown_renderer_data *data)
{
	if (ob->size) hoedown_buffer_putc(ob, '\n');

    hoedown_html_renderer_state *state = data->opaque;
    hoedown_html_renderer_state_extra *extra = state->opaque;

    hoedown_buffer *front = NULL;
    hoedown_buffer *back = NULL;
    if (lang && USE_BLOCKCODE_INFORMATION(state))
    {
        front = new_growable_buffer(lang->size);
        back = new_growable_buffer(lang->size);

        hoedown_buffer *current = front;
        for (size_t i = 0; i < lang->size; i++)
        {
            uint8_t c = lang->data[i];
            if (current == front && c == ':')
                current = back;
            else
                hoedown_buffer_putc(current, c);
        }
        lang = front;
    }

    hoedown_buffer *mapped = NULL;
    if (lang && extra->language_addition)
    {
        mapped = extra->language_addition(lang, extra->owner);
        if (mapped)
            lang = mapped;
    }

    // Code never resolves RTL: bidi reordering scrambles brackets and
    // indentation. The dir attribute pairs with bidi.css's LTR island so code
    // stays left-to-right even inside an RTL document, and even where the
    // stylesheet is absent (e.g. a styles-off export).
    HOEDOWN_BUFPUTSL(ob, "<div><pre dir=\"ltr\"");
    if (state->flags & HOEDOWN_HTML_BLOCKCODE_LINE_NUMBERS)
        HOEDOWN_BUFPUTSL(ob, " class=\"line-numbers\"");
    if (back && back->size)
    {
        HOEDOWN_BUFPUTSL(ob, " data-information=\"");
        hoedown_buffer_put(ob, back->data, back->size);
        HOEDOWN_BUFPUTSL(ob, "\"");
    }
    HOEDOWN_BUFPUTSL(ob, "><code class=\"language-");
    if (lang && lang->size)
        hoedown_escape_html(ob, lang->data, lang->size, 0);
    else
        HOEDOWN_BUFPUTSL(ob, "none");
    HOEDOWN_BUFPUTSL(ob, "\">");

	if (text)
    {
        // Remove last newline to prevent prism from adding a blank line at the
        // end of code blocks.
        size_t size = text->size;
        if (size > 0 && text->data[size - 1] == '\n')
            size--;
        hoedown_escape_html(ob, text->data, size, 0);
    }

	HOEDOWN_BUFPUTSL(ob, "</code></pre></div>\n");

    hoedown_buffer_free(mapped);
    hoedown_buffer_free(front);
    hoedown_buffer_free(back);
}

// Supports task list syntax if HOEDOWN_HTML_USE_TASK_LIST is on.
// Implementation based on hoextdown, with interactive checkbox support.
// Related to GitHub issue #269.
void hoedown_patch_render_listitem(
    hoedown_buffer *ob, const hoedown_buffer *text, hoedown_list_flags flags,
    const hoedown_renderer_data *data)
{
	if (text)
    {
        hoedown_html_renderer_state *state = data->opaque;
        size_t offset = 0;
        if (flags & HOEDOWN_LI_BLOCK)
            offset = 3;

        // The item declares its own direction, resolved over its content. A
        // leading "[ ]" checkbox marker is neutral, so the direction still
        // falls on the item's text.
        // Do task list checkbox ([x], [X], or [ ]).
        if (USE_TASK_LIST(state) && text->size >= 3)
        {
            if (strncmp((char *)(text->data + offset), "[ ]", 3) == 0)
            {
                HOEDOWN_BUFPUTSL(ob, "<li");
                put_bidi_dir(ob, text->data, text->size);
                HOEDOWN_BUFPUTSL(ob, " class=\"task-list-item\">");
                hoedown_buffer_put(ob, text->data, offset);
                // Include data-checkbox-index for interactive checkbox support
                hoedown_buffer_printf(ob,
                    "<input type=\"checkbox\" data-checkbox-index=\"%d\">",
                    g_checkbox_index++);
				offset += 3;
            }
            else if (strncmp((char *)(text->data + offset), "[x]", 3) == 0 ||
                     strncmp((char *)(text->data + offset), "[X]", 3) == 0)
            {
                HOEDOWN_BUFPUTSL(ob, "<li");
                put_bidi_dir(ob, text->data, text->size);
                HOEDOWN_BUFPUTSL(ob, " class=\"task-list-item\">");
                hoedown_buffer_put(ob, text->data, offset);
                // Include data-checkbox-index for interactive checkbox support
                hoedown_buffer_printf(ob,
                    "<input type=\"checkbox\" checked data-checkbox-index=\"%d\">",
                    g_checkbox_index++);
				offset += 3;
            }
            else
            {
                HOEDOWN_BUFPUTSL(ob, "<li");
                put_bidi_dir(ob, text->data, text->size);
                HOEDOWN_BUFPUTSL(ob, ">");
                offset = 0;
            }
        }
        else
        {
            HOEDOWN_BUFPUTSL(ob, "<li");
            put_bidi_dir(ob, text->data, text->size);
            HOEDOWN_BUFPUTSL(ob, ">");
            offset = 0;
        }
		size_t size = text->size;
		while (size && text->data[size - offset - 1] == '\n')
			size--;

		hoedown_buffer_put(ob, text->data + offset, size - offset);
	}
	HOEDOWN_BUFPUTSL(ob, "</li>\n");
}

// Decode the UTF-8 codepoint starting at data[i] (i < size). Returns the
// number of bytes consumed (1-4) and stores the codepoint in *codepoint.
// Returns 0 when the lead byte is not a valid UTF-8 start byte, when the
// sequence is truncated (fewer bytes remain than the lead byte promises), or
// when a continuation byte is malformed; the caller then passes the raw byte
// through unchanged. This is a lenient decoder: it recovers the codepoint but
// does NOT reject overlong encodings or surrogate-range (U+D800-U+DFFF)
// values, so it does not fully validate the sequence.
static size_t decode_utf8_codepoint(const uint8_t *data, size_t size, size_t i,
                                     uint32_t *codepoint)
{
    uint8_t c = data[i];
    size_t len;
    uint32_t cp;

    if ((c & 0x80) == 0x00)      { cp = c;        len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else return 0;

    if (i + len > size)
        return 0;

    for (size_t k = 1; k < len; k++)
    {
        uint8_t cc = data[i + k];
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }

    *codepoint = cp;
    return len;
}

// Encode a codepoint <= 0x7FF (used only for lowercased Latin-1 letters) as
// two UTF-8 bytes.
static void encode_utf8_2byte(uint32_t cp, uint8_t out[2])
{
    out[0] = (uint8_t)(0xC0 | (cp >> 6));
    out[1] = (uint8_t)(0x80 | (cp & 0x3F));
}

// Build a stable text-derived slug from a heading's HTML content, matching
// GitHub's heading-anchor slug algorithm (github-slugger semantics) so
// [text](#slug) links copied from GitHub-rendered Markdown keep working.
// Strips HTML tags and skips HTML entities (&amp; / &lt; / &#39; ...), then
// trims leading/trailing whitespace from the heading text. ASCII letters are
// lowercased; digits, '_' and literal '-' are kept as-is; each remaining
// ASCII space/tab maps to exactly one hyphen (runs are NOT collapsed, e.g.
// "Foo --- Bar" -> "foo-----bar"); other ASCII punctuation is dropped.
// Non-ASCII text is decoded as UTF-8: Latin-1 uppercase letters
// (U+00C0-U+00DE, excluding the multiplication sign U+00D7) are lowercased
// and re-encoded; the Latin-1 punctuation/symbol block (U+00A1-U+00BF), the
// multiplication/division signs (U+00D7, U+00F7), and the General
// Punctuation block (U+2000-U+206F, which covers em/en dashes, curly quotes,
// ellipsis, ...) are dropped entirely; every other codepoint passes through
// unchanged as raw UTF-8 bytes (e.g. "Introducción" -> "introducción").
// Malformed UTF-8 bytes are passed through unchanged rather than dropped.
// Anchors keep their raw UTF-8 bytes (no percent-encoding), matching the id
// convention GitHub emits so fragment links copied from GitHub-rendered
// Markdown keep working.
static void slugify(hoedown_buffer *out, const hoedown_buffer *content)
{
    if (!content || !content->size)
        return;

    size_t start = 0, end = content->size;
    while (start < end && (content->data[start] == ' ' ||
                            content->data[start] == '\t' ||
                            content->data[start] == '\n' ||
                            content->data[start] == '\r'))
        start++;
    while (end > start && (content->data[end - 1] == ' ' ||
                            content->data[end - 1] == '\t' ||
                            content->data[end - 1] == '\n' ||
                            content->data[end - 1] == '\r'))
        end--;

    int in_tag = 0;

    for (size_t i = start; i < end; i++)
    {
        uint8_t c = content->data[i];

        if (in_tag)
        {
            if (c == '>') in_tag = 0;
            continue;
        }
        if (c == '<')
        {
            in_tag = 1;
            continue;
        }
        if (c == '&')
        {            // skip an HTML entity like &amp; / &lt; / &#39;
            while (i + 1 < end && content->data[i + 1] != ';')
                i++;
            i++;                    // consume the ';'
            continue;
        }

        if (c >= 0x80)
        {
            uint32_t cp;
            size_t len = decode_utf8_codepoint(content->data, end, i, &cp);

            if (len == 0)
            {
                // Malformed / truncated sequence: pass the raw byte through.
                hoedown_buffer_putc(out, c);
                continue;
            }

            if ((cp >= 0x00A1 && cp <= 0x00BF) || cp == 0x00D7 ||
                cp == 0x00F7 || (cp >= 0x2000 && cp <= 0x206F))
            {
                // Dropped punctuation/symbol codepoint: emit nothing.
            }
            else if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7)
            {
                uint8_t enc[2];
                encode_utf8_2byte(cp + 0x20, enc);
                hoedown_buffer_put(out, enc, 2);
            }
            else
            {
                hoedown_buffer_put(out, content->data + i, len);
            }

            i += len - 1;
            continue;
        }

        if (c >= 'A' && c <= 'Z')
            c += 32;

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')
        {
            hoedown_buffer_putc(out, c);
        }
        else if (c == ' ' || c == '\t')
        {
            hoedown_buffer_putc(out, '-');
        }
    }
}

// rndr_header replacement that always emits a text-derived id, independent
// of the TOC nesting level. Enables [link](#section-name) navigation.
void hoedown_patch_render_header(
    hoedown_buffer *ob, const hoedown_buffer *content, int level,
    const hoedown_renderer_data *data)
{
    (void)data;
    if (ob->size) hoedown_buffer_putc(ob, '\n');

    hoedown_buffer *slug = new_growable_buffer(content ? content->size : 16);
    slugify(slug, content);
    if (slug->size == 0)
        HOEDOWN_BUFPUTSL(slug, "section");

    hoedown_buffer_printf(ob, "<h%d", level);
    put_bidi_dir(ob, content ? content->data : NULL, content ? content->size : 0);
    HOEDOWN_BUFPUTSL(ob, " id=\"");
    hoedown_buffer_put(ob, slug->data, slug->size);
    HOEDOWN_BUFPUTSL(ob, "\">");
    if (content) hoedown_buffer_put(ob, content->data, content->size);
    hoedown_buffer_printf(ob, "</h%d>\n", level);

    hoedown_buffer_free(slug);
}

// The block callbacks below reproduce hoedown's html.c defaults and add an
// explicit dir attribute resolved from the block's own content. Direction is
// never inherited (see MPBidi.h and plans/per-paragraph-rtl.md): declaring it
// on every block is what lets a mixed-direction document lay each block out
// correctly without a global rule reaching code, maths or the theme gutters.

// rndr_paragraph from html.c, plus dir. The leading-isspace skip, the two early
// returns and the HARD_WRAP branch (via put_linebreak) are reproduced exactly.
void hoedown_patch_render_paragraph(
    hoedown_buffer *ob, const hoedown_buffer *content,
    const hoedown_renderer_data *data)
{
    hoedown_html_renderer_state *state = data->opaque;
    size_t i = 0;

    if (ob->size) hoedown_buffer_putc(ob, '\n');

    if (!content || !content->size)
        return;

    while (i < content->size && isspace(content->data[i]))
        i++;

    if (i == content->size)
        return;

    HOEDOWN_BUFPUTSL(ob, "<p");
    put_bidi_dir(ob, content->data, content->size);
    HOEDOWN_BUFPUTSL(ob, ">");

    if (state->flags & HOEDOWN_HTML_HARD_WRAP)
    {
        size_t org;
        while (i < content->size)
        {
            org = i;
            while (i < content->size && content->data[i] != '\n')
                i++;

            if (i > org)
                hoedown_buffer_put(ob, content->data + org, i - org);

            // Do not insert a line break if this newline is the last
            // character on the paragraph.
            if (i >= content->size - 1)
                break;

            put_linebreak(ob, state);
            i++;
        }
    }
    else
    {
        hoedown_buffer_put(ob, content->data + i, content->size - i);
    }
    HOEDOWN_BUFPUTSL(ob, "</p>\n");
}

// rndr_blockquote from html.c, plus dir resolved over its own content.
void hoedown_patch_render_blockquote(
    hoedown_buffer *ob, const hoedown_buffer *content,
    const hoedown_renderer_data *data)
{
    (void)data;
    if (ob->size) hoedown_buffer_putc(ob, '\n');
    HOEDOWN_BUFPUTSL(ob, "<blockquote");
    put_bidi_dir(ob, content ? content->data : NULL,
                 content ? content->size : 0);
    HOEDOWN_BUFPUTSL(ob, ">\n");
    if (content) hoedown_buffer_put(ob, content->data, content->size);
    HOEDOWN_BUFPUTSL(ob, "</blockquote>\n");
}

// rndr_list from html.c, plus dir resolved over the whole list so the markers
// sit on the correct side.
void hoedown_patch_render_list(
    hoedown_buffer *ob, const hoedown_buffer *content,
    hoedown_list_flags flags, const hoedown_renderer_data *data)
{
    (void)data;
    int ordered = (flags & HOEDOWN_LIST_ORDERED) != 0;
    if (ob->size) hoedown_buffer_putc(ob, '\n');
    hoedown_buffer_puts(ob, ordered ? "<ol" : "<ul");
    put_bidi_dir(ob, content ? content->data : NULL,
                 content ? content->size : 0);
    HOEDOWN_BUFPUTSL(ob, ">\n");
    if (content) hoedown_buffer_put(ob, content->data, content->size);
    hoedown_buffer_puts(ob, ordered ? "</ol>\n" : "</ul>\n");
}

// rndr_table from html.c, plus dir resolved over the whole table so a Hebrew
// table gets RTL column order.
void hoedown_patch_render_table(
    hoedown_buffer *ob, const hoedown_buffer *content,
    const hoedown_renderer_data *data)
{
    (void)data;
    if (ob->size) hoedown_buffer_putc(ob, '\n');
    HOEDOWN_BUFPUTSL(ob, "<table");
    put_bidi_dir(ob, content ? content->data : NULL,
                 content ? content->size : 0);
    HOEDOWN_BUFPUTSL(ob, ">\n");
    if (content) hoedown_buffer_put(ob, content->data, content->size);
    HOEDOWN_BUFPUTSL(ob, "</table>\n");
}

// rndr_tablecell from html.c, plus dir. This one callback emits both <th> and
// <td>; hoedown's existing text-align style is preserved.
void hoedown_patch_render_table_cell(
    hoedown_buffer *ob, const hoedown_buffer *content,
    hoedown_table_flags flags, const hoedown_renderer_data *data)
{
    (void)data;
    if (flags & HOEDOWN_TABLE_HEADER)
        HOEDOWN_BUFPUTSL(ob, "<th");
    else
        HOEDOWN_BUFPUTSL(ob, "<td");

    put_bidi_dir(ob, content ? content->data : NULL,
                 content ? content->size : 0);

    switch (flags & HOEDOWN_TABLE_ALIGNMASK)
    {
    case HOEDOWN_TABLE_ALIGN_CENTER:
        HOEDOWN_BUFPUTSL(ob, " style=\"text-align: center\">");
        break;
    case HOEDOWN_TABLE_ALIGN_LEFT:
        HOEDOWN_BUFPUTSL(ob, " style=\"text-align: left\">");
        break;
    case HOEDOWN_TABLE_ALIGN_RIGHT:
        HOEDOWN_BUFPUTSL(ob, " style=\"text-align: right\">");
        break;
    default:
        HOEDOWN_BUFPUTSL(ob, ">");
    }

    if (content)
        hoedown_buffer_put(ob, content->data, content->size);

    if (flags & HOEDOWN_TABLE_HEADER)
        HOEDOWN_BUFPUTSL(ob, "</th>\n");
    else
        HOEDOWN_BUFPUTSL(ob, "</td>\n");
}

// rndr_footnote_def from html.c, plus dir on the <li>. The back-reference
// anchor is still inserted at the end of the first paragraph block.
void hoedown_patch_render_footnote_def(
    hoedown_buffer *ob, const hoedown_buffer *content, unsigned int num,
    const hoedown_renderer_data *data)
{
    (void)data;
    size_t i = 0;
    int pfound = 0;

    // Insert anchor at the end of first paragraph block.
    if (content)
    {
        while ((i + 3) < content->size)
        {
            if (content->data[i++] != '<') continue;
            if (content->data[i++] != '/') continue;
            if (content->data[i++] != 'p' && content->data[i] != 'P') continue;
            if (content->data[i] != '>') continue;
            i -= 3;
            pfound = 1;
            break;
        }
    }

    HOEDOWN_BUFPUTSL(ob, "\n<li");
    put_bidi_dir(ob, content ? content->data : NULL,
                 content ? content->size : 0);
    hoedown_buffer_printf(ob, " id=\"fn%d\">\n", num);
    if (pfound)
    {
        hoedown_buffer_put(ob, content->data, i);
        hoedown_buffer_printf(ob,
            "&nbsp;<a href=\"#fnref%d\" rev=\"footnote\">&#8617;</a>", num);
        hoedown_buffer_put(ob, content->data + i, content->size - i);
    }
    else if (content)
    {
        hoedown_buffer_put(ob, content->data, content->size);
    }
    HOEDOWN_BUFPUTSL(ob, "</li>\n");
}

// Returns 1 if the tag starting at content->data[i] (the '<') is a replaced
// element that always renders visible content of its own (e.g. <img>),
// independent of any text nodes around it.
static int is_replaced_element_tag(const hoedown_buffer *content, size_t i)
{
    static const char *replaced_tags[] = {
        "img", "svg", "video", "audio", "iframe", "embed", "object", "canvas"
    };

    size_t j = i + 1;
    if (j < content->size && content->data[j] == '/')
        return 0; // closing tags never introduce new content

    size_t start = j;
    while (j < content->size && content->data[j] != '>' &&
           content->data[j] != ' ' && content->data[j] != '\t' &&
           content->data[j] != '\n' && content->data[j] != '\r' &&
           content->data[j] != '/')
        j++;

    size_t len = j - start;
    if (!len)
        return 0;

    for (size_t t = 0; t < sizeof(replaced_tags) / sizeof(replaced_tags[0]); t++)
    {
        size_t tag_len = strlen(replaced_tags[t]);
        if (tag_len != len)
            continue;

        int match = 1;
        for (size_t k = 0; k < len; k++)
        {
            uint8_t c = content->data[start + k];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != (uint8_t)replaced_tags[t][k]) { match = 0; break; }
        }
        if (match)
            return 1;
    }
    return 0;
}

// Returns 1 if the entity spelled out between content->data[amp] ('&') and
// the following ';' is whitespace-only (e.g. &nbsp;) rather than a visible
// glyph (e.g. &copy;).
static int is_whitespace_entity(const hoedown_buffer *content, size_t amp,
                                 size_t semi)
{
    size_t start = amp + 1;
    size_t len = semi - start;
    if (!len)
        return 0;

    if (content->data[start] == '#')
    {
        // Numeric reference: &#160; / &#xA0; (NBSP), &#8194; (ENSP), etc.
        // Anything else counts as visible content. Parsed manually (not via
        // strtol) since `content` is not NUL-terminated.
        size_t k = start + 1;
        int is_hex = (k < semi && (content->data[k] == 'x' ||
                                    content->data[k] == 'X'));
        if (is_hex) k++;
        if (k >= semi)
            return 0;

        uint32_t value = 0;
        for (; k < semi; k++)
        {
            uint8_t c = content->data[k];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (is_hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (is_hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return 0;
            value = value * (is_hex ? 16 : 10) + (uint32_t)digit;
        }
        return value == 0x00A0 || value == 0x2002 || value == 0x2003 ||
               value == 0x2009;
    }

    static const char *whitespace_entities[] = { "nbsp", "ensp", "emsp", "thinsp" };
    for (size_t t = 0; t < sizeof(whitespace_entities) / sizeof(whitespace_entities[0]); t++)
    {
        size_t name_len = strlen(whitespace_entities[t]);
        if (name_len != len)
            continue;

        int match = 1;
        for (size_t k = 0; k < len; k++)
        {
            uint8_t c = content->data[start + k];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != (uint8_t)whitespace_entities[t][k]) { match = 0; break; }
        }
        if (match)
            return 1;
    }
    return 0;
}

// Returns 1 if the rendered row content contains no visible text once HTML
// tags and entities are stripped out (i.e. every cell in the row is empty).
// Tags that always render their own content (e.g. <img>) and entities that
// decode to a visible glyph (e.g. &copy;) count as non-blank.
static int is_row_content_blank(const hoedown_buffer *content)
{
    if (!content || !content->size)
        return 1;

    int in_tag = 0;
    for (size_t i = 0; i < content->size; i++)
    {
        uint8_t c = content->data[i];

        if (in_tag)
        {
            if (c == '>') in_tag = 0;
            continue;
        }
        if (c == '<')
        {
            if (is_replaced_element_tag(content, i))
                return 0;
            in_tag = 1;
            continue;
        }
        if (c == '&')
        {
            size_t semi = i + 1;
            while (semi < content->size && content->data[semi] != ';')
                semi++;
            if (semi < content->size)
            {
                if (!is_whitespace_entity(content, i, semi))
                    return 0;
                i = semi;
            }
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return 0;
    }
    return 1;
}

// rndr_table_header replacement that omits the <thead> element entirely when
// every header cell is empty, so a header row like `| | |` doesn't render as
// a blank header line. Related to a MacDown 3000 table-rendering fix.
void hoedown_patch_render_table_header(
    hoedown_buffer *ob, const hoedown_buffer *content,
    const hoedown_renderer_data *data)
{
    (void)data;
    if (is_row_content_blank(content))
        return;

    if (ob->size) hoedown_buffer_putc(ob, '\n');
    HOEDOWN_BUFPUTSL(ob, "<thead>\n");
    hoedown_buffer_put(ob, content->data, content->size);
    HOEDOWN_BUFPUTSL(ob, "</thead>\n");
}

// Adds a "toc" class to the outmost UL element to support TOC styling.
void hoedown_patch_render_toc_header(
    hoedown_buffer *ob, const hoedown_buffer *content, int level,
    const hoedown_renderer_data *data)
{
    hoedown_html_renderer_state *state = data->opaque;

    if (level <= state->toc_data.nesting_level) {
        /* set the level offset if this is the first header
         * we're parsing for the document */
        if (state->toc_data.current_level == 0)
            state->toc_data.level_offset = level - 1;

        level -= state->toc_data.level_offset;

        if (level > state->toc_data.current_level) {
            while (level > state->toc_data.current_level) {
                if (state->toc_data.current_level == 0)
                    HOEDOWN_BUFPUTSL(ob, "<ul class=\"toc\">\n<li>\n");
                else
                    HOEDOWN_BUFPUTSL(ob, "<ul>\n<li>\n");
                state->toc_data.current_level++;
            }
        } else if (level < state->toc_data.current_level) {
            HOEDOWN_BUFPUTSL(ob, "</li>\n");
            while (level < state->toc_data.current_level) {
                HOEDOWN_BUFPUTSL(ob, "</ul>\n</li>\n");
                state->toc_data.current_level--;
            }
            HOEDOWN_BUFPUTSL(ob,"<li>\n");
        } else {
            HOEDOWN_BUFPUTSL(ob,"</li>\n<li>\n");
        }

        hoedown_buffer *slug = new_growable_buffer(content ? content->size : 16);
        slugify(slug, content);
        if (slug->size == 0)
            HOEDOWN_BUFPUTSL(slug, "section");
        HOEDOWN_BUFPUTSL(ob, "<a");
        put_bidi_dir(ob, content ? content->data : NULL,
                     content ? content->size : 0);
        HOEDOWN_BUFPUTSL(ob, " href=\"#");
        hoedown_buffer_put(ob, slug->data, slug->size);
        HOEDOWN_BUFPUTSL(ob, "\">");
        hoedown_buffer_free(slug);
        state->toc_data.header_count++;
        if (content) hoedown_buffer_put(ob, content->data, content->size);
        HOEDOWN_BUFPUTSL(ob, "</a>\n");
    }
}
