#include "LayoutModule.h"
#include "PagedLayout.h"

#include <QGuiApplication>
#include <QTextCharFormat>

#include <algorithm>

namespace {

namespace data = editor::data;
namespace paged = editor::layout::v02;

void check(bool condition, const char* message)
{
    if (!condition)
        qFatal("%s", message);
}

data::DocumentSnapshot document()
{
    data::DocumentSnapshot doc;
    doc.page.size = QSizeF(260, 100);
    doc.page.margins = QMarginsF(14, 14, 14, 14);
    return doc;
}

data::ContainerNode text(QString id, QString value)
{
    data::ContainerNode node;
    node.id = std::move(id);
    node.type = data::ContainerType::Text;
    node.text.defaultStyle.fontFamily = QStringLiteral("Arial");
    node.text.text = std::move(value);
    int start = 0;
    for (const auto& line : node.text.text.split(u'\n')) {
        data::ParagraphInfo paragraph;
        paragraph.range = {start, start + int(line.size())};
        paragraph.format.lineHeight = 24;
        node.text.paragraphs.push_back(paragraph);
        start = paragraph.range.end + 1;
    }
    return node;
}

data::ContainerNode image(data::DocumentSnapshot& doc, QString id, QSizeF displaySize)
{
    auto& resource = doc.resources[QStringLiteral("photo")];
    resource = QImage(20, 30, QImage::Format_RGB32);
    resource.fill(Qt::red);
    data::ContainerNode node;
    node.id = std::move(id);
    node.type = data::ContainerType::Image;
    node.image = {QStringLiteral("photo"), displaySize};
    return node;
}

void checkPageGeometry(const paged::PagedLayoutResult& result)
{
    for (const auto& page : result.pages) {
        check(page.pageRect.topLeft() == QPointF(), "Page coordinates must have a local origin");
        const auto area = page.contentRect.adjusted(-0.01, -0.01, 0.01, 0.01);
        for (const auto& fragment : page.textFragments) {
            check(area.contains(fragment.rect), "Text fragment crosses the printable region");
            const auto line = result.textLayouts.at(fragment.layoutIndex)->lineAt(fragment.lineIndex);
            check(line.isValid() && line.position() == fragment.rect.topLeft(),
                  "QTextLine and its fragment disagree on the paper position");
            check(line.textLength() == fragment.sourceRange.end - fragment.sourceRange.start
                      && fragment.rect.height() >= line.height(),
                  "Text range or row height disagrees with the shaped line");
        }
        for (const auto& box : page.containers)
            check(area.contains(box.outerRect.topLeft()) && area.contains(box.outerRect.bottomRight()),
                  "Container crosses the printable region");
        for (const auto& picture : page.images)
            check(area.contains(picture.rect), "Image crosses the printable region");
    }
}

void pageGrowthAndShrink()
{
    auto doc = document();
    doc.revision = 42;
    doc.root.children = {text("body", "a\nb\nc")};
    auto result = paged::composeLayout(doc);
    check(result.documentRevision == 42 && result.issues.empty() && result.pages.size() == 1 &&
          result.pages.front().textFragments.size() == 3, "Exactly full page must not create a trailing page");
    const auto legacy = editor::layout::composeLayout(doc);
    check(result.pages.front().textFragments[2].rect == legacy.textFragments[2].rect,
          "Simple one-page layout must keep v0.1 geometry");

    ++doc.revision;
    doc.root.children = {text("body", "a\nb\nc\nd")};
    result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() == 2 &&
          result.pages[1].textFragments.front().sourceRange.start == 6, "One additional line must move whole to page two");
    check(result.pages[1].textFragments.front().rect.top() == result.pages[1].contentRect.top(),
          "Text must resume inside the top margin");
    checkPageGeometry(result);

    ++doc.revision;
    doc.root.children = {text("body", "a\nb\nc")};
    result = paged::composeLayout(doc);
    check(result.documentRevision == 44 && result.pages.size() == 1,
          "Editing must recompute the page count and revision");
}

const data::ContainerLayout& boxFor(const paged::LayoutPage& page, const QString& id)
{
    const auto box = std::find_if(page.containers.begin(), page.containers.end(),
        [&](const auto& item) { return item.containerId == id; });
    check(box != page.containers.end(), "Expected a container slice on this page");
    return *box;
}

void verticalInsetsAppearOnlyAtContainerEnds()
{
    auto doc = document();
    doc.page.size.setHeight(800);
    doc.page.margins.setTop(50);
    doc.page.margins.setBottom(50);
    auto body = text("body", QStringLiteral("line\n").repeated(31) + QStringLiteral("last"));
    body.geometry.margin = QMarginsF(4, 30, 4, 30);
    body.geometry.padding = QMarginsF(5, 20, 5, 20);
    for (auto& paragraph : body.text.paragraphs)
        paragraph.format.lineHeight = 50;
    doc.root.children = {body, text("following", "next")};

    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() == 3, "Repeated insets must not change the page count");
    const auto& first = result.pages[0];
    const auto& middle = result.pages[1];
    const auto& last = result.pages[2];
    const auto& firstBox = boxFor(first, body.id);
    const auto& middleBox = boxFor(middle, body.id);
    const auto& lastBox = boxFor(last, body.id);
    check(first.textFragments.size() == 13 && first.textFragments.front().rect.top() == 100 &&
          first.textFragments.back().rect.bottom() == 750 && firstBox.rect.top() == 80 &&
          firstBox.outerRect.bottom() == 750, "First slice uses only its opening margin and padding");
    check(middle.textFragments.size() == 14 && middle.textFragments.front().rect.top() == 50 &&
          middle.textFragments.back().rect.bottom() == 750 && middleBox.rect.top() == 50 &&
          middleBox.rect.bottom() == 750 &&
          middleBox.outerRect.top() == 50 && middleBox.outerRect.bottom() == 750,
          "Middle slice uses full height inside the page margins");
    check(last.textFragments.front().rect.top() == 50 && lastBox.contentRect.bottom() == 300 &&
          lastBox.rect.bottom() == 320 && lastBox.outerRect.bottom() == 350 &&
          last.textFragments.back().containerId == QStringLiteral("following") &&
          last.textFragments.back().rect.top() == 350, "Only the last slice uses the closing insets");
    check(firstBox.contentRect.left() == middleBox.contentRect.left() &&
          middleBox.contentRect.left() == lastBox.contentRect.left(), "Side insets must remain on every page");
    checkPageGeometry(result);
}

void closingSpaceStaysWithLastRow()
{
    auto doc = document();
    auto body = text("body", "a\nb\nc");
    body.geometry.padding.setBottom(5);
    data::ContainerNode group;
    group.id = QStringLiteral("group");
    group.geometry.margin.setBottom(7);
    group.children = {body};
    doc.root.children = {group, text("following", "next")};

    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() == 2 &&
          result.pages[0].textFragments.size() == 2 && result.pages[1].textFragments.size() == 2,
          "Move the last row with its closing insets instead of creating an empty page");
    const auto& second = result.pages[1];
    check(second.textFragments[0].sourceRange.start == 4 && second.textFragments[0].rect.top() == 14 &&
          boxFor(second, group.id).outerRect.bottom() == 50 && second.textFragments[1].rect.top() == 50,
          "Closing insets from child and parent apply only once");
    checkPageGeometry(result);
}

void wrappedTextAndStyles()
{
    auto doc = document();
    auto body = text("styled", QStringLiteral("ABCDE😀FGHIJK").repeated(18));
    body.geometry.width = 100;
    body.text.paragraphs[0].role = data::ParagraphRole::Heading;
    body.text.paragraphs[0].headingLevel = 2;
    data::TextFormatSpan span{{16, 28}, {}};
    span.style.color = QColor(Qt::red);
    span.style.fontSizePt = 30;
    body.text.formatSpans.push_back(span);
    doc.root.children = {body};
    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() > 2, "Long styled paragraph must span multiple pages");
    check(result.textLayouts.size() == 1 && result.textLayouts[0]->font().bold(), "Heading style was lost");
    const auto formats = result.textLayouts[0]->formats();
    check(formats.size() == 3 && formats[1].start == 16 && formats[1].length == 12 &&
          formats[1].format.foreground().color() == Qt::red, "Local style range was lost");
    int offset = 0;
    bool tallRow = false;
    for (const auto& page : result.pages) {
        for (const auto& fragment : page.textFragments) {
            check(fragment.sourceRange.start == offset && fragment.sourceRange.end > offset,
                  "Characters were repeated or lost while moving rows between pages");
            offset = fragment.sourceRange.end;
            check(offset == body.text.text.size() || !body.text.text.at(offset).isLowSurrogate(),
                  "A line boundary split a UTF-16 surrogate pair");
            tallRow |= fragment.rect.height() > 24;
        }
    }
    check(offset == body.text.text.size() && tallRow, "Large characters must increase row height");
    checkPageGeometry(result);
}

void flowImageAndGroup()
{
    auto doc = document();
    auto picture = image(doc, "flow", QSizeF(64, 30));
    picture.geometry.margin = QMarginsF(3, 4, 3, 4);
    auto following = text("following", "next");
    data::ContainerNode group;
    group.id = QStringLiteral("group");
    group.geometry.padding = QMarginsF(4, 2, 4, 2);
    group.children = {text("body", "a\nb"), picture};
    doc.root.children = {group, following};
    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() == 2 && result.pages[0].images.empty() &&
          result.pages[1].images.size() == 1, "Flow image should move to the next page in one piece");
    const auto& page = result.pages[1];
    check(page.images.front().rect.size() == QSizeF(64, 30) &&
          page.images.front().sourceRect.size() == QSizeF(20, 30), "The image was cropped or resized");
    check(page.textFragments.front().containerId == following.id &&
          page.textFragments.front().rect.top() >= page.images.front().rect.bottom() &&
          page.containers.front().containerId == group.id, "Following containers must wait for the group's image");
    checkPageGeometry(result);
}

void floatingImageReflowsText()
{
    auto doc = document();
    auto body = text("body", QStringLiteral("word ").repeated(44));
    auto first = image(doc, "photo-first", QSizeF(70, 42));
    first.placement = data::PlacementMode::Floating;
    first.geometry.position = QPointF(75, 55);
    auto second = first;
    second.id = QStringLiteral("photo-second");
    body.children = {first, second};
    doc.root.children = {body, text("following", "end")};
    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() >= 3 && result.pages[0].images.empty() &&
          result.pages[1].images.size() == 1 && result.pages[2].images.size() == 1,
          "Overlapping floating images should move down or to later pages");

    int offset = 0;
    bool alongside = false;
    for (const auto& page : result.pages) {
        for (const auto& picture : page.images) {
            check(picture.rect.x() == page.contentRect.x() + 75, "Floating image changed X when moved");
            for (const auto& fragment : page.textFragments) {
                check(!fragment.rect.intersects(picture.rect), "Text overlapped a moved floating image");
                alongside |= fragment.containerId == body.id && fragment.rect.top() < picture.rect.bottom()
                    && fragment.rect.bottom() > picture.rect.top();
            }
        }
        for (const auto& fragment : page.textFragments) {
            if (fragment.containerId != body.id)
                continue;
            check(fragment.sourceRange.start == offset, "Wrapping across pages lost text");
            offset = fragment.sourceRange.end;
        }
    }
    check(alongside && offset == body.text.text.size(), "Text did not wrap beside the moved image");
    check(result.pages.back().textFragments.back().containerId == QStringLiteral("following"),
          "Following text must start after all content and images of its predecessor");
    checkPageGeometry(result);
}

void laterLargeTextDoesNotMoveTheFirstRow()
{
    auto doc = document();
    auto body = text("body", QStringLiteral("small\u2028BIG"));
    data::TextFormatSpan span{{6, int(body.text.text.size())}, {}};
    span.style.fontSizePt = 36;
    body.text.formatSpans = {span};
    doc.root.children = {body};
    const auto plain = paged::composeLayout(doc);
    const auto firstRow = plain.pages[0].textFragments.front().rect;

    auto picture = image(doc, "full-width-float", QSizeF(firstRow.width(), 32));
    picture.placement = data::PlacementMode::Floating;
    picture.geometry.position.setY(firstRow.height() + 1);
    doc.root.children[0].children = {picture};
    const auto wrapped = paged::composeLayout(doc);
    check(wrapped.issues.empty() && wrapped.pages.size() == 2 &&
          wrapped.pages.front().images.size() == 1 && wrapped.pages.front().textFragments.size() == 1 &&
          wrapped.pages.front().textFragments.front().rect == firstRow,
          "A larger row later in the same paragraph must not move the small row above the image");
    check(wrapped.pages[1].textFragments.size() == 1 &&
          wrapped.pages[1].textFragments.front().sourceRange.start == 6 &&
          wrapped.pages[1].textFragments.front().rect.height() > firstRow.height(),
          "The large row must still avoid the image and move whole to the next page");
    checkPageGeometry(wrapped);
}

void rowSplitAroundImageRetriesAsAWhole()
{
    auto doc = document();
    auto body = text("body", QStringLiteral("a\n") + QStringLiteral("word ").repeated(24));
    body.text.paragraphs.front().format.lineHeight = 60;
    auto picture = image(doc, "small-float", QSizeF(66, 12));
    picture.placement = data::PlacementMode::Floating;
    picture.geometry.position = QPointF(83, 56);
    body.children = {picture};
    doc.root.children = {body};

    const auto result = paged::composeLayout(doc);
    check(result.issues.empty() && result.pages.size() >= 2 && result.pages[0].images.size() == 1 &&
          result.pages[0].textFragments.size() == 1, "No part of a row may stay in the gap at the page bottom");
    int offset = 2;
    for (std::size_t index = 1; index < result.pages.size(); ++index) {
        for (const auto& fragment : result.pages[index].textFragments) {
            check(fragment.sourceRange.start == offset, "Retrying a row around an obstacle lost or repeated text");
            offset = fragment.sourceRange.end;
        }
    }
    check(result.pages[1].textFragments.front().rect.width() == result.pages[1].contentRect.width() &&
          offset == body.text.text.size(), "Retry the whole row at the next page's available width");
    checkPageGeometry(result);
}

void invalidObjects()
{
    auto doc = document();
    auto picture = image(doc, "too-tall", QSizeF(30, 80));
    auto missing = picture;
    missing.id = QStringLiteral("missing");
    missing.image = {QStringLiteral("absent"), QSizeF(30, 20)};
    auto large = text("oversize-text", "W\nnormal");
    data::TextFormatSpan enlarged{{0, 1}, {}};
    enlarged.style.fontSizePt = 96;
    large.text.formatSpans = {enlarged};
    doc.root.children = {picture, missing, large, text("valid", "ok")};
    const auto result = paged::composeLayout(doc);
    check(result.issues.size() == 3 && result.issues[0].kind == paged::LayoutIssueKind::ImageDoesNotFit &&
          result.issues[0].containerId == picture.id && result.issues[1].kind == paged::LayoutIssueKind::MissingImage &&
          result.issues[2].kind == paged::LayoutIssueKind::TextRowDoesNotFit && result.issues[2].sourceRange,
          "Unplaceable objects must report an ID, kind and source range where applicable");
    check(result.pages.size() == 2 && result.pages[0].textFragments.size() == 1 &&
          result.pages[1].textFragments.size() == 2, "Preserve the oversized line on an isolated page");
    const auto& overflow = result.pages[0].textFragments.front();
    const auto line = result.textLayouts[overflow.layoutIndex]->lineAt(overflow.lineIndex);
    check(overflow.containerId == large.id && overflow.sourceRange.start == 0 && overflow.sourceRange.end == 1 &&
          line.isValid() && line.position() == overflow.rect.topLeft() &&
          overflow.rect.bottom() > result.pages[0].contentRect.bottom() &&
          result.pages[0].layoutBounds.contains(overflow.rect),
          "Oversized text must keep its original range and a usable preview rectangle");
    const auto& next = result.pages[1].textFragments;
    check(next[0].containerId == large.id && next[0].sourceRange.start == 2 &&
          next[1].containerId == QStringLiteral("valid") && next[1].rect.top() >= next[0].rect.bottom(),
          "Following text must continue after the overflow without overlapping");

    auto narrow = document();
    auto wide = text("narrow", "W");
    wide.geometry.width = 5;
    narrow.root.children = {text("before", "before"), wide, text("after", "after")};
    const auto wideResult = paged::composeLayout(narrow);
    check(wideResult.issues.size() == 1 && wideResult.pages.size() == 3 &&
          wideResult.pages[1].textFragments.size() == 1 &&
          wideResult.pages[1].textFragments[0].containerId == wide.id &&
          wideResult.pages[1].textFragments[0].rect.width() > wide.geometry.width &&
          wideResult.pages[2].textFragments[0].containerId == QStringLiteral("after"),
          "Too-wide glyph must remain selectable without sharing its page with adjacent containers");

    doc.page.margins.setBottom(100);
    const auto invalid = paged::composeLayout(doc);
    check(invalid.pages.empty() && invalid.issues.size() == 1 &&
          invalid.issues[0].kind == paged::LayoutIssueKind::InvalidPage, "Invalid paper must have a diagnostic");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    pageGrowthAndShrink();
    verticalInsetsAppearOnlyAtContainerEnds();
    closingSpaceStaysWithLastRow();
    wrappedTextAndStyles();
    flowImageAndGroup();
    floatingImageReflowsText();
    laterLargeTextDoesNotMoveTheFirstRow();
    rowSplitAroundImageRetriesAsAWhole();
    invalidObjects();
    qInfo("Paged layout acceptance passed");
}
