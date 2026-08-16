//
//  MPBidiTests.m
//  MacDown 3000
//
//  Tests for MPBidiResolveDirection, the direction resolver shared by the
//  editor and the preview.
//

#import <XCTest/XCTest.h>
#import "MPBidi.h"


@interface MPBidiTests : XCTestCase
@end


@implementation MPBidiTests

#pragma mark - Helpers

/// Labels the fixture documents define, for the reference-form tests.
static NSSet *MPTestDefinedLabels = nil;

static int MPTestIsDefined(const uint8_t *label, size_t length, void *context)
{
    (void)context;
    NSString *name = [[NSString alloc] initWithBytes:label length:length
                                            encoding:NSUTF8StringEncoding];
    return name && [MPTestDefinedLabels containsObject:name];
}

- (MPBidiDirection)resolve:(NSString *)text mode:(MPBidiScanMode)mode
{
    const char *utf8 = text.UTF8String;
    MPBidiRefs refs = { MPTestIsDefined, NULL };
    return MPBidiResolveDirection((const uint8_t *)utf8, strlen(utf8), mode,
                                  MPBidiDefaultFallback, &refs);
}

- (void)setUp
{
    [super setUp];
    MPTestDefinedLabels = [NSSet set];
}

/**
 * Asserts that a Markdown block and the HTML hoedown renders from it resolve
 * to the same direction, and that the direction is the expected one.
 *
 * This pairing is the point of the function. Scanning either mode alone can
 * pass while the two panes lay the same paragraph out in opposite directions,
 * so the HTML half of every case here is what hoedown actually emits for the
 * Markdown half — not what it looks like it should emit.
 */
- (void)assertAgree:(NSString *)markdown
               html:(NSString *)html
          resolvesTo:(MPBidiDirection)expected
               line:(NSUInteger)line
{
    MPBidiDirection md = [self resolve:markdown mode:MPBidiScanMarkdown];
    MPBidiDirection ht = [self resolve:html mode:MPBidiScanHTML];
    XCTAssertEqual(md, ht,
                   @"line %lu: editor and preview disagree on %@",
                   (unsigned long)line, markdown);
    XCTAssertEqual(md, expected,
                   @"line %lu: wrong direction for %@",
                   (unsigned long)line, markdown);
}

// Parameter names must not collide with the selector keywords below, or the
// preprocessor rewrites "html:" into the argument.
#define MPAssertAgree(mdText, htmlText, direction) \
    [self assertAgree:(mdText) html:(htmlText) \
           resolvesTo:(direction) line:__LINE__]


#pragma mark - Editor and preview agreement

- (void)testCodeSpansResolveOnTheProseAfterThem
{
    MPAssertAgree(@"`NSString` שלום עולם",
                  @"<p><code>NSString</code> שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"code `x` then שלום",
                  @"<p>code <code>x</code> then שלום</p>",
                  MPBidiDirectionLTR);
}

- (void)testMathsResolvesOnTheProseAfterIt
{
    // Maths delimiters survive into the HTML verbatim, for MathJax to process
    // in the browser. hoedown needs two backslashes: char_escape lists ( ) [ ]
    // among the escapable characters, so a single \( is an escaped paren.
    MPAssertAgree(@"\\\\(x\\\\) שלום עולם", @"<p>\\(x\\) שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"$x$ שלום עולם", @"<p>$x$ שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"$$x+y$$ שלום", @"<p>$$x+y$$ שלום</p>",
                  MPBidiDirectionRTL);
}

- (void)testEscapedParenthesisIsNotMaths
{
    // A single backslash escapes the paren, so hoedown renders "(שלום)" and
    // its contents are ordinary text that must decide the direction. Were the
    // resolver to mistake this for maths it would skip to "Hello" and answer
    // LTR, so the Hebrew inside the parens is what makes this assertion bite.
    MPAssertAgree(@"\\(שלום\\) Hello", @"<p>(שלום) Hello</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"\\(x\\) שלום עולם", @"<p>(x) שלום עולם</p>",
                  MPBidiDirectionLTR);
}

- (void)testAutolinkDetectionFollowsHoedown
{
    // A scheme needs no "//": hoedown links <tel:…> and shows the URL.
    MPAssertAgree(@"<tel:+15550100> התקשרו אלינו",
                  @"<p><a href=\"tel:+15550100\">tel:+15550100</a> התקשרו אלינו</p>",
                  MPBidiDirectionLTR);
    // A space aborts the autolink, leaving a raw tag the reader never sees.
    MPAssertAgree(@"<http://x.com and 5 > שלום עולם",
                  @"<p><http://x.com and 5 > שלום עולם</p>",
                  MPBidiDirectionRTL);
}

- (void)testFenceClosesOnAnEqualRun
{
    // char_codespan stops at the opening run's length, leaving the surplus as
    // text, and a lone "$" must not pair with the first half of a later "$$".
    MPAssertAgree(@"`x``y` שלום עולם",
                  @"<p><code>x</code><code>y</code> שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"$5 שלום עולם $$x+y$$",
                  @"<p>$5 שלום עולם \\(x+y\\)</p>", MPBidiDirectionRTL);
}

- (void)testAutolinkTextDecidesDirectionOnBothSides
{
    // hoedown renders an angle autolink with the URL as visible link text.
    MPAssertAgree(@"<https://example.com/a> האתר הרשמי",
                  @"<p><a href=\"https://example.com/a\">https://example.com/a</a> האתר הרשמי</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"<info@example.com> שלום",
                  @"<p><a href=\"mailto:info@example.com\">info@example.com</a> שלום</p>",
                  MPBidiDirectionLTR);
}

- (void)testRawHTMLWithAURLInAnAttributeIsStillATag
{
    // The URL sits in an attribute, so hoedown renders a plain tag rather
    // than an autolink and the Hebrew decides on both sides.
    MPAssertAgree(@"<a href=\"https://example.com\">שלום עולם</a>",
                  @"<p><a href=\"https://example.com\">שלום עולם</a></p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"<img src=\"https://cdn.example.com/a.png\" alt=\"\"> שלום",
                  @"<p><img src=\"https://cdn.example.com/a.png\" alt=\"\"> שלום</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"<a href=\"mailto:a@b.com\">שלום עולם</a>",
                  @"<p><a href=\"mailto:a@b.com\">שלום עולם</a></p>",
                  MPBidiDirectionRTL);
}

- (void)testOnlyWhatHoedownCallsATagIsSkipped
{
    // hoedown wants "<", an optional "/", then an alphanumeric. A doctype or
    // a processing instruction is escaped and stays visible.
    MPAssertAgree(@"<!DOCTYPE html> הוא ההכרזה",
                  @"<p>&lt;!DOCTYPE html&gt; הוא ההכרזה</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"<?php echo 1; ?> שלום", @"<p>&lt;?php echo 1; ?&gt; שלום</p>",
                  MPBidiDirectionLTR);
    // Conversely a digit does start a tag, however unlikely it looks.
    MPAssertAgree(@"1 <2 שלום> 3", @"<p>1 <2 שלום> 3</p>", MPBidiDirectionLTR);
}

- (void)testBackslashEscapesDoNotOpenSyntax
{
    // The escaped backtick is a literal backtick, not a code span.
    MPAssertAgree(@"\\`Not code\\` שלום עולם",
                  @"<p>`Not code` שלום עולם</p>", MPBidiDirectionLTR);
    MPAssertAgree(@"\\<x\\> שלום", @"<p>&lt;x&gt; שלום</p>",
                  MPBidiDirectionLTR);
}

- (void)testReferenceImageFollowsWhetherItsLabelResolves
{
    // hoedown builds an <img> only when the label resolves, hiding the alt
    // text in an attribute; otherwise it prints the brackets and every
    // character is visible. A block alone cannot tell, so the editor is told.
    MPTestDefinedLabels = [NSSet setWithObjects:@"img1", @"שלום עולם", nil];

    MPAssertAgree(@"![a][img1] שלום עולם",
                  @"<p><img src=\"i.png\" alt=\"a\"> שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"![שלום עולם] Hello",
                  @"<p><img src=\"i.png\" alt=\"שלום עולם\"> Hello</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"![שלום עולם][] Hello",
                  @"<p><img src=\"i.png\" alt=\"שלום עולם\"> Hello</p>",
                  MPBidiDirectionLTR);
}

- (void)testUndefinedReferenceIsVisibleText
{
    MPTestDefinedLabels = [NSSet set];

    MPAssertAgree(@"![a][] שלום עולם", @"<p>![a][] שלום עולם</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"![a][zz] שלום עולם", @"<p>![a][zz] שלום עולם</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"![שלום עולם] Hello", @"<p>![שלום עולם] Hello</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"[t][zz] שלום", @"<p>[t][zz] שלום</p>", MPBidiDirectionLTR);
}

- (void)testLinkTextStaysVisibleWhicheverFormItTakes
{
    MPTestDefinedLabels = [NSSet setWithObject:@"lnk"];

    MPAssertAgree(@"[שלום][lnk] Hello",
                  @"<p><a href=\"http://x.com\">שלום</a> Hello</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"[t][lnk] שלום",
                  @"<p><a href=\"http://x.com\">t</a> שלום</p>",
                  MPBidiDirectionLTR);
}

- (void)testImageSyntaxWithoutATargetIsLiteral
{
    MPAssertAgree(@"![Image without URL] שלום עולם",
                  @"<p>![Image without URL] שלום עולם</p>",
                  MPBidiDirectionLTR);
}

- (void)testTripleTildeIsNotAnInlineFence
{
    // hoedown's strikethrough takes two tildes; a third leaves text visible.
    MPAssertAgree(@"~~~Too many tildes~~~ שלום עולם",
                  @"<p>~~~Too many tildes~~~ שלום עולם</p>",
                  MPBidiDirectionLTR);
    MPAssertAgree(@"~~שלום~~ עולם", @"<p><del>שלום</del> עולם</p>",
                  MPBidiDirectionRTL);
}

- (void)testBracketedProseIsNotATag
{
    // No tag here: hoedown escapes both angles and the Hebrew decides.
    MPAssertAgree(@"1 < 2 שלום > 3", @"<p>1 &lt; 2 שלום &gt; 3</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"1 < 2 ולכן", @"<p>1 &lt; 2 ולכן</p>", MPBidiDirectionRTL);
}

- (void)testLinkAndImageTargetsDoNotDecideDirection
{
    MPAssertAgree(@"![](logo.png) שלום עולם",
                  @"<p><img src=\"logo.png\"> שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"[שלום](http://example.com)",
                  @"<p><a href=\"http://example.com\">שלום</a></p>",
                  MPBidiDirectionRTL);
}

- (void)testRawHTMLDoesNotDecideDirection
{
    // hoedown sets neither HOEDOWN_HTML_ESCAPE nor HOEDOWN_HTML_SKIP_HTML, so
    // author-written tags reach the preview untouched and must be skipped on
    // both sides.
    MPAssertAgree(@"<img src=\"logo.png\"> שלום עולם",
                  @"<p><img src=\"logo.png\"> שלום עולם</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"<span class=\"note\">שלום עולם</span>",
                  @"<p><span class=\"note\">שלום עולם</span></p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"<br> שלום עולם", @"<p><br> שלום עולם</p>",
                  MPBidiDirectionRTL);
}

- (void)testEntitiesDoNotDecideDirection
{
    // hoedown passes a recognised entity through rather than re-escaping it.
    MPAssertAgree(@"&amp; שלום", @"<p>&amp; שלום</p>", MPBidiDirectionRTL);
    MPAssertAgree(@"&nbsp; שלום", @"<p>&nbsp; שלום</p>", MPBidiDirectionRTL);
}

- (void)testUnclosedDelimiterDoesNotSwallowTheBlock
{
    // Prose about a price, not maths; prose about a backtick, not code.
    MPAssertAgree(@"$5 לחודש בלבד", @"<p>$5 לחודש בלבד</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"100$ לחודש", @"<p>100$ לחודש</p>", MPBidiDirectionRTL);
    MPAssertAgree(@"` שלום עולם", @"<p>` שלום עולם</p>", MPBidiDirectionRTL);
    MPAssertAgree(@"![שלום עולם", @"<p>![שלום עולם</p>", MPBidiDirectionRTL);
}

- (void)testEmphasisAndPlainProseAgree
{
    MPAssertAgree(@"**חשוב** להבין", @"<p><strong>חשוב</strong> להבין</p>",
                  MPBidiDirectionRTL);
    MPAssertAgree(@"שלום עולם", @"<p>שלום עולם</p>", MPBidiDirectionRTL);
    MPAssertAgree(@"Hello world", @"<p>Hello world</p>", MPBidiDirectionLTR);
}


#pragma mark - First-strong resolution

- (void)testStrongScriptsDecideDirection
{
    XCTAssertEqual([self resolve:@"שלום עולם" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"Hebrew is strong RTL");
    XCTAssertEqual([self resolve:@"مرحبا بالعالم" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"Arabic is strong RTL");
    XCTAssertEqual([self resolve:@"Hello world" mode:MPBidiScanMarkdown],
                   MPBidiDirectionLTR, @"Latin is strong LTR");
}

- (void)testWeakAndNeutralCharactersDoNotDecideDirection
{
    // Digits are class EN/AN and emoji class ON: each must fall through to
    // whatever strong character follows.
    XCTAssertEqual([self resolve:@"١٢٣ Hello" mode:MPBidiScanMarkdown],
                   MPBidiDirectionLTR,
                   @"Arabic-Indic digits are not strong: the Latin decides");
    XCTAssertEqual([self resolve:@"123 שלום" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"ASCII digits are not strong");
    XCTAssertEqual([self resolve:@"🎉 שלום" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"emoji are neutral");
    XCTAssertEqual([self resolve:@"> - # שלום" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"Markdown markers are neutral");
}

- (void)testExplicitMarksOutrankContent
{
    XCTAssertEqual([self resolve:@"‏Hello world" mode:MPBidiScanMarkdown],
                   MPBidiDirectionRTL, @"RLM forces RTL over Latin content");
    XCTAssertEqual([self resolve:@"‎שלום" mode:MPBidiScanMarkdown],
                   MPBidiDirectionLTR, @"LRM forces LTR over Hebrew content");
}

- (void)testTextWithNothingStrongTakesTheFallback
{
    NSArray *neutral = @[@"", @"12345", @"--- ... !!!", @"<p></p>"];
    for (NSString *text in neutral)
    {
        const char *utf8 = text.UTF8String;
        XCTAssertEqual(MPBidiResolveDirection((const uint8_t *)utf8,
                                              strlen(utf8),
                                              MPBidiScanMarkdown,
                                              MPBidiDirectionRTL, NULL),
                       MPBidiDirectionRTL,
                       @"%@ should take the caller's fallback", text);
        XCTAssertEqual(MPBidiResolveDirection((const uint8_t *)utf8,
                                              strlen(utf8),
                                              MPBidiScanMarkdown,
                                              MPBidiDirectionLTR, NULL),
                       MPBidiDirectionLTR,
                       @"%@ should take the caller's fallback", text);
    }
}

- (void)testNullAndEmptyBuffersTakeTheFallback
{
    XCTAssertEqual(MPBidiResolveDirection(NULL, 0, MPBidiScanMarkdown,
                                          MPBidiDirectionRTL, NULL),
                   MPBidiDirectionRTL);
    XCTAssertEqual(MPBidiResolveDirection((const uint8_t *)"", 0,
                                          MPBidiScanHTML, MPBidiDirectionLTR,
                                          NULL),
                   MPBidiDirectionLTR);
}

- (void)testTruncatedUTF8Terminates
{
    // A sequence cut short must not loop or read past the end.
    const uint8_t truncated[] = {0xD7, 0xA9, 0xD7};   // "ש" plus a lead byte
    XCTAssertEqual(MPBidiResolveDirection(truncated, sizeof(truncated),
                                          MPBidiScanMarkdown,
                                          MPBidiDirectionLTR, NULL),
                   MPBidiDirectionRTL);

    // A bare continuation byte is neutral in both classifiers, so the scan
    // only terminates if the malformed-byte path advances.
    const uint8_t continuation[] = {0x80, 0x80, 0x80};
    XCTAssertEqual(MPBidiResolveDirection(continuation, sizeof(continuation),
                                          MPBidiScanMarkdown,
                                          MPBidiDirectionRTL, NULL),
                   MPBidiDirectionRTL, @"neutral bytes fall through");

    const uint8_t neutralThenHebrew[] = {0x80, 0xD7, 0xA9};
    XCTAssertEqual(MPBidiResolveDirection(neutralThenHebrew,
                                          sizeof(neutralThenHebrew),
                                          MPBidiScanMarkdown,
                                          MPBidiDirectionLTR, NULL),
                   MPBidiDirectionRTL, @"scan advances past the bad byte");
}

@end
