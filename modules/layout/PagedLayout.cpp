#include "PagedLayout.h"

#include "LayoutModule.h"

#include <QFont>
#include <QTextCharFormat>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace editor::layout::v02 {
namespace {

constexpr qreal tolerance = 0.00001;
constexpr int pageLimit = 10000;

struct Cursor {
    int page;
    qreal y;
};

bool operator<(Cursor a, Cursor b)
{
    return a.page < b.page || (a.page == b.page && a.y < b.y);
}

struct VerticalSpan {
    qreal top;
    qreal bottom;
};

// A container can appear on more than one page. Its ID is stable across slices.
using Spans = std::map<int, VerticalSpan>;
using Obstacles = std::map<int, std::vector<QRectF>>;

void include(Spans& spans, int page, qreal top, qreal bottom)
{
    auto [item, inserted] = spans.try_emplace(page, VerticalSpan{top, bottom});
    if (!inserted) {
        item->second.top = qMin(item->second.top, top);
        item->second.bottom = qMax(item->second.bottom, bottom);
    }
}

QRectF inset(const QRectF& rect, const QMarginsF& edges)
{
    return rect.adjusted(edges.left(), edges.top(), -edges.right(), -edges.bottom());
}

bool containsWidth(const QRectF& area, qreal left, qreal right)
{
    return left >= area.left() - tolerance && right <= area.right() + tolerance;
}

QTextCharFormat charFormat(data::TextStyle style, const data::TextStyleOverride& change = {})
{
    QFont font(change.fontFamily.value_or(style.fontFamily));
    const auto points = change.fontSizePt.value_or(style.fontSizePt);
    font.setPixelSize(qMax(1, qRound(points * data::DocumentUnitsPerInch / 72.0)));
    font.setBold(change.bold.value_or(style.bold));
    font.setItalic(change.italic.value_or(style.italic));
    font.setUnderline(change.underline.value_or(style.underline));
    QTextCharFormat format;
    format.setFont(font);
    format.setForeground(change.color.value_or(style.color));
    return format;
}

Qt::Alignment alignment(data::ParagraphAlignment value)
{
    switch (value) {
    case data::ParagraphAlignment::Center: return Qt::AlignHCenter;
    case data::ParagraphAlignment::Right: return Qt::AlignRight;
    case data::ParagraphAlignment::Justify: return Qt::AlignJustify;
    case data::ParagraphAlignment::Left: return Qt::AlignLeft;
    }
    return Qt::AlignLeft;
}

std::shared_ptr<QTextLayout> makeParagraphLayout(const data::TextContent& text,
                                                const data::ParagraphInfo& paragraph)
{
    const auto base = resolveParagraphStyle(text.defaultStyle, paragraph);
    const auto baseFormat = charFormat(base);
    const auto [start, end] = paragraph.range;
    auto layout = std::make_shared<QTextLayout>(text.text.mid(start, end - start), baseFormat.font());
    QTextOption option;
    option.setUseDesignMetrics(true);
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(alignment(paragraph.format.alignment));
    layout->setTextOption(option);
    layout->setCacheEnabled(true);

    QList<QTextLayout::FormatRange> formats;
    int next = start;
    auto span = std::lower_bound(text.formatSpans.begin(), text.formatSpans.end(), start,
        [](const auto& entry, int offset) { return entry.range.end <= offset; });
    for (; span != text.formatSpans.end() && span->range.start < end; ++span) {
        const int left = qMax(start, span->range.start);
        const int right = qMin(end, span->range.end);
        if (left >= right)
            continue;
        if (next < left)
            formats.push_back({next - start, left - next, baseFormat});
        formats.push_back({left - start, right - left, charFormat(base, span->style)});
        next = right;
    }
    if (next < end)
        formats.push_back({next - start, end - next, baseFormat});
    layout->setFormats(formats);
    return layout;
}

struct RowSlots {
    std::vector<QRectF> areas;
    qreal nextClearY;
    bool obstructed = false;
};

RowSlots availableSlots(const QRectF& band, const Obstacles& obstacles, int page)
{
    RowSlots result{{band}, band.bottom()};
    const auto found = obstacles.find(page);
    if (found == obstacles.end())
        return result;

    for (const auto& obstacle : found->second) {
        if (!band.intersects(obstacle))
            continue;
        if (!result.obstructed)
            result.nextClearY = obstacle.bottom();
        else
            result.nextClearY = qMin(result.nextClearY, obstacle.bottom());
        result.obstructed = true;
        std::vector<QRectF> remaining;
        for (const auto& area : result.areas) {
            if (!area.intersects(obstacle)) {
                remaining.push_back(area);
                continue;
            }
            if (obstacle.left() - area.left() > tolerance)
                remaining.emplace_back(area.left(), area.top(), obstacle.left() - area.left(), area.height());
            if (area.right() - obstacle.right() > tolerance)
                remaining.emplace_back(obstacle.right(), area.top(), area.right() - obstacle.right(), area.height());
        }
        result.areas.swap(remaining);
    }
    return result;
}

struct DraftFragment {
    QTextLine line;
    QRectF rect;
};

struct DraftRow {
    std::vector<DraftFragment> fragments;
    qreal height;
    bool finished = false;
    bool tooWide = false;
};

bool sameHorizontalSlots(const RowSlots& a, const RowSlots& b)
{
    return std::equal(a.areas.begin(), a.areas.end(), b.areas.begin(), b.areas.end(),
        [](const QRectF& left, const QRectF& right) {
            return left.x() == right.x() && left.width() == right.width();
        });
}

class ParagraphLines {
public:
    explicit ParagraphLines(QTextLayout& layout) : layout_(layout) { layout_.beginLayout(); }
    ~ParagraphLines() { layout_.endLayout(); }

    DraftRow tryRow(const RowSlots& rowSlots, qreal minimumHeight)
    {
        DraftRow row{{}, minimumHeight};
        for (const auto& slot : rowSlots.areas) {
            if (!pending_.isValid())
                pending_ = layout_.createLine();
            if (!pending_.isValid()) {
                row.finished = true;
                break;
            }
            pending_.setLineWidth(slot.width());
            const bool tooWide = pending_.naturalTextWidth() > slot.width() + tolerance;
            if (tooWide && rowSlots.obstructed)
                continue;
            row.tooWide = tooWide;
            row.height = qMax(row.height, pending_.height());
            row.finished = pending_.textStart() + pending_.textLength() >= layout_.text().size();
            row.fragments.push_back({pending_, slot});
            pending_ = {};
            if (row.finished)
                break;
        }
        return row;
    }

    void commit(const DraftRow& row)
    {
        for (const auto& fragment : row.fragments) {
            auto line = fragment.line;
            line.setPosition(fragment.rect.topLeft());
            committed_.push_back(fragment.rect);
        }
    }

    void retry(const DraftRow& row)
    {
        if (row.fragments.size() == 1 && !pending_.isValid()) {
            // Qt can re-fit the last line at the width available on the next page.
            pending_ = row.fragments.front().line;
            return;
        }
        // A row split around a picture may have consumed multiple Qt lines.
        // Restore the paragraph's prior lines before retrying it as one row.
        layout_.endLayout();
        layout_.beginLayout();
        pending_ = {};
        for (const auto& placed : committed_) {
            auto line = layout_.createLine();
            line.setLineWidth(placed.width());
            line.setPosition(placed.topLeft());
        }
    }

private:
    QTextLayout& layout_;
    QTextLine pending_;
    std::vector<QRectF> committed_;
};

DraftRow fitRowAroundObstacles(ParagraphLines& lines, QRectF band, qreal minimumHeight,
                               qreal remainingHeight, const Obstacles& obstacles, int page,
                               RowSlots& rowSlots)
{
    rowSlots = availableSlots(band, obstacles, page);
    auto row = lines.tryRow(rowSlots, minimumHeight);
    while (true) {
        const qreal actualBandHeight = qMin(row.height, remainingHeight);
        if (actualBandHeight <= band.height() + tolerance)
            return row;

        // Grow only the current row's band. Keeping that height through retries
        // avoids oscillation when narrowing the row pushes a large glyph out.
        band.setHeight(actualBandHeight);
        auto reduced = availableSlots(band, obstacles, page);
        if (sameHorizontalSlots(rowSlots, reduced))
            return row;
        lines.retry(row);
        rowSlots = std::move(reduced);
        row = lines.tryRow(rowSlots, minimumHeight);
    }
}

class Paginator {
public:
    explicit Paginator(const data::DocumentSnapshot& document)
        : document_(document), paper_(QPointF(0, 0), document.page.size),
          content_(inset(paper_, document.page.margins))
    {
        result_.documentRevision = document.revision;
    }

    PagedLayoutResult run()
    {
        const auto margins = document_.page.margins;
        if (!validArea(paper_) || !validArea(content_) || margins.left() < 0 ||
            margins.top() < 0 || margins.right() < 0 || margins.bottom() < 0) {
            issue(LayoutIssueKind::InvalidPage, {}, QStringLiteral("页面尺寸或页边距无法形成正文区"));
            return std::move(result_);
        }

        page(0);
        const Cursor start{0, content_.top()};
        layoutChildren(document_.root, content_, start, false, {});
        for (auto& item : result_.pages) {
            std::stable_sort(item.containers.begin(), item.containers.end(), [&](const auto& a, const auto& b) {
                return order_.value(a.containerId) < order_.value(b.containerId);
            });
        }
        return std::move(result_);
    }

private:
    static bool validArea(const QRectF& rect)
    {
        return rect.width() > tolerance && rect.height() > tolerance &&
               std::isfinite(rect.right()) && std::isfinite(rect.bottom());
    }

    void issue(LayoutIssueKind kind, const data::ContainerId& id, QString reason,
               std::optional<data::TextRange> range = {})
    {
        result_.issues.push_back({kind, id, std::move(reason), range});
    }

    bool validPage(int index, const data::ContainerId& id)
    {
        if (index < pageLimit)
            return true;
        issue(LayoutIssueKind::PageLimitExceeded, id, QStringLiteral("对象位置超出分页上限（10000 页）"));
        return false;
    }

    LayoutPage& page(int index)
    {
        while (int(result_.pages.size()) <= index)
            result_.pages.push_back(LayoutPage{paper_, content_, {}, {}, {}});
        return result_.pages[index];
    }

    // Measure a requested Y offset in usable space, skipping paper margins.
    static Cursor offsetFrom(Cursor start, qreal offset, const QRectF& area)
    {
        if (offset <= 0)
            return start;
        const qreal remaining = area.bottom() - start.y;
        if (offset <= remaining)
            return {start.page, start.y + offset};
        const qreal rest = offset - remaining;
        const qreal pages = std::floor(rest / area.height());
        if (!std::isfinite(pages) || pages >= pageLimit - start.page)
            return {pageLimit, area.top()};
        return {start.page + 1 + static_cast<int>(pages),
                area.top() + std::fmod(rest, area.height())};
    }

    Spans layoutChildren(const data::ContainerNode& parent, const QRectF& area, Cursor start,
                         bool floating, const data::ContainerId& parentId)
    {
        Spans spans;
        Obstacles occupied;
        Cursor previous = start;
        for (const auto& node : parent.children) {
            order_.insert(node.id, nextOrder_++);
            const auto& g = node.geometry;
            auto position = offsetFrom(start, qMax(0.0, g.position.y() - g.margin.top()), area);
            if (!floating)
                position = std::max(position, previous);
            if (!validPage(position.page, node.id))
                continue;

            const qreal requestedX = area.left() + g.position.x();
            const qreal x = floating ? requestedX : qMax(requestedX, area.left() + g.margin.left());
            const qreal width = node.type == data::ContainerType::Image
                ? node.image.displaySize.width() + g.padding.left() + g.padding.right()
                : g.width == 0 ? area.right() - x - g.margin.right() : g.width;
            auto boxes = node.type == data::ContainerType::Image
                ? layoutImage(node, parentId, area, position, x, width, occupied)
                : layoutContainer(node, parentId, area, position, x, width, occupied);
            for (const auto& [index, outer] : boxes) {
                include(spans, index, outer.top(), outer.bottom());
                if (floating)
                    occupied[index].push_back(outer);
                else
                    previous = {index, outer.bottom()};
            }
        }
        return spans;
    }

    using Boxes = std::vector<std::pair<int, QRectF>>;

    Boxes layoutImage(const data::ContainerNode& node, const data::ContainerId& parentId,
                      const QRectF& area, Cursor position, qreal x, qreal width,
                      const Obstacles& occupied)
    {
        const auto& g = node.geometry;
        const qreal height = qMax(g.minimumHeight, node.image.displaySize.height() + g.padding.top() + g.padding.bottom());
        const qreal outerHeight = g.margin.top() + height + g.margin.bottom();
        if (!node.image.displaySize.isValid() || node.image.displaySize.isEmpty() ||
            !containsWidth(area, x - g.margin.left(), x + width + g.margin.right()) ||
            !std::isfinite(outerHeight) || outerHeight > area.height() + tolerance) {
            issue(LayoutIssueKind::ImageDoesNotFit, node.id, QStringLiteral("图片及其边距无法完整放入正文区"));
            return {};
        }
        const auto source = document_.resources.constFind(node.image.resourceId);
        if (source == document_.resources.constEnd() || source->isNull()) {
            issue(LayoutIssueKind::MissingImage, node.id, QStringLiteral("找不到图片资源"));
            return {};
        }

        while (validPage(position.page, node.id)) {
            if (position.y + outerHeight > area.bottom() + tolerance) {
                position = {position.page + 1, area.top()};
                continue;
            }
            const QRectF frame(x, position.y + g.margin.top(), width, height);
            const auto outer = frame.marginsAdded(g.margin);
            qreal clearY = position.y;
            const auto found = occupied.find(position.page);
            if (found != occupied.end()) {
                for (const auto& other : found->second)
                    if (outer.intersects(other))
                        clearY = qMax(clearY, other.bottom());
            }
            if (clearY > position.y) {
                position.y = clearY;
                continue;
            }
            const auto inner = inset(frame, g.padding);
            auto& placed = page(position.page);
            placed.containers.push_back({node.id, parentId, frame, inner, outer});
            placed.images.push_back({node.id, node.image.resourceId,
                                     QRectF(QPointF(0, 0), QSizeF(source->size())),
                                     QRectF(inner.topLeft(), node.image.displaySize)});
            return {{position.page, outer}};
        }
        return {};
    }

    Boxes layoutContainer(const data::ContainerNode& node, const data::ContainerId& parentId,
                          const QRectF& area, Cursor position, qreal x, qreal width,
                          const Obstacles& externalObstacles)
    {
        const auto& g = node.geometry;
        const qreal top = g.margin.top() + g.padding.top();
        const qreal bottom = g.margin.bottom() + g.padding.bottom();
        const auto inner = QRectF(x + g.padding.left(), area.top() + top,
                                  width - g.padding.left() - g.padding.right(), area.height() - top - bottom);
        if (!validArea(inner) || !containsWidth(area, x - g.margin.left(), x + width + g.margin.right()) ||
            !std::isfinite(g.minimumHeight)) {
            issue(LayoutIssueKind::ContainerDoesNotFit, node.id, QStringLiteral("容器及其边距无法形成页内正文区"));
            return {};
        }
        Cursor start{position.page, position.y + top};
        if (start.y >= inner.bottom() - tolerance)
            start = {start.page + 1, inner.top()};
        if (!validPage(start.page, node.id))
            return {};

        const bool isText = node.type == data::ContainerType::Text;
        auto spans = layoutChildren(node, inner, start, isText, node.id);
        if (isText) {
            auto obstacles = externalObstacles;
            for (const auto& [index, unused] : spans) {
                for (const auto& box : page(index).containers)
                    if (box.parentId == node.id)
                        obstacles[index].push_back(box.outerRect);
            }
            layoutText(node, inner, start, obstacles, spans);
        }

        // Minimum height belongs to the container, including when it has no text.
        qreal extra = qMax(0.0, g.minimumHeight - g.padding.top() - g.padding.bottom());
        if (spans.empty() || extra > 0) {
            for (auto cursor = start; validPage(cursor.page, node.id); cursor = {cursor.page + 1, inner.top()}) {
                const qreal available = inner.bottom() - cursor.y;
                include(spans, cursor.page, cursor.y, cursor.y + qMin(available, extra));
                if (extra <= available + tolerance)
                    break;
                extra -= available;
            }
        }
        auto first = spans.find(start.page);
        if (first != spans.end())
            first->second.top = start.y;

        Boxes boxes;
        for (const auto& [index, span] : spans) {
            const QRectF frame(x, span.top - g.padding.top(), width,
                               span.bottom - span.top + g.padding.top() + g.padding.bottom());
            const auto outer = frame.marginsAdded(g.margin);
            page(index).containers.push_back({node.id, parentId, frame, inset(frame, g.padding), outer});
            boxes.emplace_back(index, outer);
        }
        return boxes;
    }

    void layoutText(const data::ContainerNode& node, const QRectF& area, Cursor cursor,
                    const Obstacles& obstacles, Spans& spans)
    {
        for (const auto& paragraph : node.text.paragraphs) {
            if (!validPage(cursor.page, node.id))
                return;
            const auto& format = paragraph.format;
            cursor.y = qMin(area.bottom(), cursor.y + format.spaceBefore);
            auto layout = makeParagraphLayout(node.text, paragraph);
            const int layoutIndex = static_cast<int>(result_.textLayouts.size());
            result_.textLayouts.push_back(layout);
            ParagraphLines lines(*layout);
            bool firstRow = true;
            bool finished = false;
            while (!finished) {
                if (cursor.y >= area.bottom() - tolerance)
                    cursor = {cursor.page + 1, area.top()};
                if (!validPage(cursor.page, node.id))
                    return;
                const qreal left = qBound(area.left(), area.left() + format.leftIndent +
                    (firstRow ? format.firstLineIndent : 0), area.right());
                const qreal width = area.right() - left - format.rightIndent;
                // Match v0.1: when indents exhaust the width, use the whole content area.
                const qreal remainingHeight = area.bottom() - cursor.y;
                const QRectF band(width > tolerance ? left : area.left(), cursor.y,
                                  width > tolerance ? width : area.width(),
                                  qMin(format.lineHeight, remainingHeight));
                RowSlots rowSlots;
                const auto row = fitRowAroundObstacles(lines, band, format.lineHeight,
                                                        remainingHeight, obstacles, cursor.page, rowSlots);
                if (row.fragments.empty()) {
                    if (row.finished)
                        break;
                    cursor.y = rowSlots.nextClearY;
                    continue;
                }
                const data::TextRange range{paragraph.range.start + row.fragments.front().line.textStart(),
                    paragraph.range.start + row.fragments.back().line.textStart() + row.fragments.back().line.textLength()};
                if (row.tooWide || (!rowSlots.obstructed && row.height > area.height() + tolerance)) {
                    issue(LayoutIssueKind::TextRowDoesNotFit, node.id,
                          QStringLiteral("文字行超出页内可用宽度或高度"), range);
                    lines.commit(row);
                    finished = row.finished;
                    firstRow = false;
                    continue;
                }
                if (cursor.y + row.height > area.bottom() + tolerance) {
                    lines.retry(row);
                    cursor = {cursor.page + 1, area.top()};
                    continue;
                }

                lines.commit(row);
                auto& fragments = page(cursor.page).textFragments;
                for (const auto& draft : row.fragments) {
                    const int start = paragraph.range.start + draft.line.textStart();
                    auto rect = draft.rect;
                    rect.setHeight(row.height);
                    fragments.push_back({node.id, {start, start + draft.line.textLength()}, rect,
                                         layoutIndex, draft.line.lineNumber()});
                }
                include(spans, cursor.page, cursor.y, cursor.y + row.height);
                cursor.y += row.height;
                finished = row.finished;
                firstRow = false;
            }
            // Trailing paragraph space stops at the paper edge and cannot by itself create a page.
            const qreal after = qMin(area.bottom(), cursor.y + format.spaceAfter);
            if (spans.count(cursor.page))
                include(spans, cursor.page, cursor.y, after);
            cursor.y = after;
        }
    }

    const data::DocumentSnapshot& document_;
    const QRectF paper_;
    const QRectF content_;
    PagedLayoutResult result_;
    QHash<data::ContainerId, int> order_;
    int nextOrder_ = 0;
};

} // namespace

PagedLayoutResult composeLayout(const data::DocumentSnapshot& document)
{
    return Paginator(document).run();
}

} // namespace editor::layout::v02
