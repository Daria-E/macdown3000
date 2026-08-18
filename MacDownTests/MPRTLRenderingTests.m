//
//  MPRTLRenderingTests.m
//  MacDown 3000
//
//  Verifies that the preview declares an explicit writing direction on every
//  text-bearing block, resolved from the block's own content. Covers each
//  block type in both directions, an LTR block nested in an RTL container, the
//  shared table-cell callback for both <th> and <td>, and that code stays LTR
//  and task-list and text-align markup survive. See plans/per-paragraph-rtl.md.
//

#import <XCTest/XCTest.h>
#import <hoedown/document.h>
#import "MPRendererTestHelpers.h"
#import "hoedown_html_patch.h"

// A Hebrew phrase (strong RTL) and its English gloss (strong LTR), used to make
// each block resolve one way or the other.
#define MP_RTL_TEXT @"שלום עולם"
#define MP_LTR_TEXT @"Hello world"


@interface MPRTLRenderingTests : XCTestCase
@property (nonatomic, strong) MPRenderer *renderer;
@property (nonatomic, strong) MPMockRendererDataSource *dataSource;
@property (nonatomic, strong) MPMockRendererDelegate *delegate;
@end


@implementation MPRTLRenderingTests

- (void)setUp
{
    [super setUp];
    self.dataSource = [[MPMockRendererDataSource alloc] init];
    self.delegate = [[MPMockRendererDelegate alloc] init];
    self.renderer = [[MPRenderer alloc] init];
    self.renderer.dataSource = self.dataSource;
    self.renderer.delegate = self.delegate;
}

- (void)tearDown
{
    self.renderer = nil;
    self.dataSource = nil;
    self.delegate = nil;
    [super tearDown];
}

- (NSString *)render:(NSString *)markdown ext:(int)ext flags:(int)flags
{
    self.delegate.extensions = ext;
    self.renderer.rendererFlags = flags;
    self.dataSource.markdown = markdown;
    [self.renderer parseMarkdown:markdown];
    return [self.renderer currentHtml];
}

#pragma mark - Paragraph, header, blockquote

- (void)testParagraphDeclaresDirection
{
    XCTAssertTrue([[self render:MP_RTL_TEXT ext:0 flags:0]
                   containsString:@"<p dir=\"rtl\">"]);
    XCTAssertTrue([[self render:MP_LTR_TEXT ext:0 flags:0]
                   containsString:@"<p dir=\"ltr\">"]);
}

- (void)testHeaderDeclaresDirectionAlongsideSlug
{
    XCTAssertTrue([[self render:@"# " MP_RTL_TEXT ext:0 flags:0]
                   containsString:@"<h1 dir=\"rtl\" id=\""]);
    XCTAssertTrue([[self render:@"# " MP_LTR_TEXT ext:0 flags:0]
                   containsString:@"<h1 dir=\"ltr\" id=\""]);
}

- (void)testBlockquoteDeclaresDirection
{
    XCTAssertTrue([[self render:@"> " MP_RTL_TEXT ext:0 flags:0]
                   containsString:@"<blockquote dir=\"rtl\">"]);
    XCTAssertTrue([[self render:@"> " MP_LTR_TEXT ext:0 flags:0]
                   containsString:@"<blockquote dir=\"ltr\">"]);
}

- (void)testDigitsOnlyParagraphFallsBackToLTR
{
    // Nothing strong: the shared fallback is LTR, on every surface.
    XCTAssertTrue([[self render:@"123 456 789" ext:0 flags:0]
                   containsString:@"<p dir=\"ltr\">"]);
}

#pragma mark - Lists

- (void)testListAndItemsDeclareDirection
{
    NSString *html = [self render:@"- " MP_RTL_TEXT ext:0 flags:0];
    XCTAssertTrue([html containsString:@"<ul dir=\"rtl\">"]);
    XCTAssertTrue([html containsString:@"<li dir=\"rtl\">"]);
}

- (void)testLTRItemInsideRTLListKeepsItsOwnDirection
{
    // The list resolves RTL from its first item, but the English item declares
    // its own dir="ltr" and is not dragged right-to-left by its container.
    NSString *md = @"- " MP_RTL_TEXT @"\n- " MP_LTR_TEXT;
    NSString *html = [self render:md ext:0 flags:0];
    XCTAssertTrue([html containsString:@"<ul dir=\"rtl\">"]);
    XCTAssertTrue([html containsString:@"<li dir=\"rtl\">"]);
    XCTAssertTrue([html containsString:@"<li dir=\"ltr\">"]);
}

- (void)testOrderedListDeclaresDirection
{
    XCTAssertTrue([[self render:@"1. " MP_RTL_TEXT ext:0 flags:0]
                   containsString:@"<ol dir=\"rtl\">"]);
}

#pragma mark - Tables (the shared <th>/<td> callback)

- (void)testTableAndBothCellKindsDeclareDirection
{
    NSString *md = @"| " MP_RTL_TEXT @" |\n| --- |\n| " MP_LTR_TEXT @" |";
    NSString *html = [self render:md ext:HOEDOWN_EXT_TABLES flags:0];
    XCTAssertTrue([html containsString:@"<table dir=\"rtl\">"]);
    // The header cell is Hebrew, the body cell English: both kinds carry a dir,
    // each resolved from its own content.
    XCTAssertTrue([html containsString:@"<th dir=\"rtl\""]);
    XCTAssertTrue([html containsString:@"<td dir=\"ltr\""]);
}

- (void)testTableCellsKeepTextAlign
{
    // A centred column keeps hoedown's style attribute next to the new dir.
    NSString *md = @"| h |\n|:---:|\n| x |";
    NSString *html = [self render:md ext:HOEDOWN_EXT_TABLES flags:0];
    XCTAssertTrue([html containsString:@"<th dir=\"ltr\" style=\"text-align: center\">"]);
    XCTAssertTrue([html containsString:@"<td dir=\"ltr\" style=\"text-align: center\">"]);
}

#pragma mark - Code stays LTR

- (void)testFencedCodeIsAlwaysLTR
{
    // Even Hebrew inside a fence renders left-to-right, so brackets and
    // indentation are not reordered.
    NSString *md = @"```\n" MP_RTL_TEXT @" = 1\n```";
    NSString *html = [self render:md ext:HOEDOWN_EXT_FENCED_CODE flags:0];
    XCTAssertTrue([html containsString:@"<pre dir=\"ltr\""]);
}

#pragma mark - Footnotes

- (void)testFootnoteDefinitionDeclaresDirection
{
    NSString *md = MP_RTL_TEXT @"[^1]\n\n[^1]: " MP_RTL_TEXT;
    NSString *html = [self render:md ext:HOEDOWN_EXT_FOOTNOTES flags:0];
    XCTAssertTrue([html containsString:@"<li dir=\"rtl\" id=\"fn1\">"]);
}

#pragma mark - Task lists

- (void)testTaskListItemKeepsCheckboxAndDeclaresDirection
{
    NSString *md = @"- [ ] " MP_RTL_TEXT;
    NSString *html = [self render:md ext:0 flags:HOEDOWN_HTML_USE_TASK_LIST];
    XCTAssertTrue([html containsString:
                   @"<li dir=\"rtl\" class=\"task-list-item\">"]);
    XCTAssertTrue([html containsString:@"<input type=\"checkbox\""]);
}

@end
