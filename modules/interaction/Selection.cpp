#include "Selection.h"

#include "TextBoundary.h"

#include <QTextLayout>
#include <QTextLine>

#include <algorithm>
#include <limits>

namespace editor::interaction {
namespace {

bool lineOf(const data::LayoutResult& layout, const data::TextFragment& f, QTextLine* out)
{
    if (f.layoutIndex < 0 || f.layoutIndex >= int(layout.textLayouts.size()))
        return false;
    const auto& textLayout = *layout.textLayouts.at(size_t(f.layoutIndex));
    if (f.lineIndex < 0 || f.lineIndex >= textLayout.lineCount())
        return false;
    *out = textLayout.lineAt(f.lineIndex);
    return true;
}

// 源偏移 → 纸张 x。cursorToX 在不同 Qt 版本里可能返回绝对坐标或相对行首，
// 用「相对 textStart 的增量 + 片段真实左边界」可以同时兼容两者：
// f.rect.left() 就是该行 setPosition() 时的纸张 x。
qreal xAt(const data::TextFragment& f, const QTextLine& line, int offset)
{
    const int begin = line.textStart();
    const int end   = begin + line.textLength();
    const int pos   = qBound(begin, begin + (offset - f.sourceRange.start), end);
    const qreal base = line.cursorToX(begin);
    const qreal x    = f.rect.left() + (line.cursorToX(pos) - base);
    return qBound(f.rect.left(), x, f.rect.right());
}

QRectF rectFor(const data::TextFragment& f, const QTextLine& line, int a, int b)
{
    const qreal x1 = xAt(f, line, a);
    const qreal x2 = xAt(f, line, b);
    return QRectF(x1, f.rect.top(), qMax<qreal>(0, x2 - x1), f.rect.height());
}

} // 匿名命名空间结束

data::TextRange normalizeRange(int a, int b)
{
    return a <= b ? data::TextRange{a, b} : data::TextRange{b, a};
}

data::TextRange normalizeRange(data::TextRange range)
{
    return normalizeRange(range.start, range.end);
}

data::TextRange containerRange(const data::LayoutResult& layout,
                               const data::ContainerId& id)
{
    int lo = std::numeric_limits<int>::max();
    int hi = 0;
    for (const auto& f : layout.textFragments) {
        if (f.containerId != id)
            continue;
        lo = std::min(lo, f.sourceRange.start);
        hi = std::max(hi, f.sourceRange.end);
    }
    return lo == std::numeric_limits<int>::max()
        ? data::TextRange{0, 0} : data::TextRange{lo, hi};
}

Hit clampFocus(const data::LayoutResult& layout, const Hit& anchor, QPointF paperFocus)
{
    const data::TextRange legal = containerRange(layout, anchor.containerId);

    QRectF box;
    bool any = false;
    for (const auto& f : layout.textFragments) {
        if (f.containerId != anchor.containerId)
            continue;
        box = any ? box.united(f.rect) : f.rect;
        any = true;
    }
    if (!any)
        return { anchor.containerId, qBound(legal.start, anchor.offset, legal.end) };

    // 先把纸面点夹回本容器外接框，再命中测试；完全落在别处时按上下取首尾。
    const QPointF p(qBound(box.left(), paperFocus.x(), box.right()),
                    qBound(box.top(),  paperFocus.y(), box.bottom()));
    const Hit hit = hitTest(layout, p);
    if (!hit.valid() || hit.containerId != anchor.containerId) {
        const int edge = paperFocus.y() < box.center().y() ? legal.start : legal.end;
        return { anchor.containerId, edge };
    }
    return { anchor.containerId, qBound(legal.start, hit.offset, legal.end) };
}

data::TextRange selectAllRange(const data::DocumentSnapshot& doc,
                               const data::ContainerId& id)
{
    return { 0, textLength(doc, id) };
}

std::vector<QRectF> selectionRects(const data::LayoutResult& layout,
                                   const data::ContainerId& id,
                                   data::TextRange range)
{
    std::vector<QRectF> rects;
    const data::TextRange sel = normalizeRange(range);
    for (const auto& f : layout.textFragments) {
        if (f.containerId != id)
            continue;
        const int a = std::max(sel.start, f.sourceRange.start);
        const int b = std::min(sel.end,   f.sourceRange.end);
        if (a >= b)
            continue;  // 空段落、零宽片段不产生高亮
        QTextLine line;
        if (!lineOf(layout, f, &line))
            continue;
        rects.push_back(rectFor(f, line, a, b));
    }
    return rects;
}

SelectionState makeSelection(const data::LayoutResult& layout,
                             const Hit& anchor, const Hit& focus)
{
    const int end = (focus.valid() && focus.containerId == anchor.containerId)
                  ? focus.offset : anchor.offset;
    return makeSelection(layout, anchor.containerId, normalizeRange(anchor.offset, end));
}

SelectionState makeSelection(const data::LayoutResult& layout,
                             const data::ContainerId& id,
                             data::TextRange range)
{
    SelectionState state;
    state.containerId = id;
    state.range = normalizeRange(range);
    state.rects = selectionRects(layout, id, state.range);
    return state;
}

QRectF caretRect(const data::LayoutResult& layout,
                 const data::ContainerId& id, int offset)
{
    const data::TextRange legal = containerRange(layout, id);
    const int at = qBound(legal.start, offset, legal.end);

    const data::TextFragment* best = nullptr;
    for (const auto& f : layout.textFragments) {
        if (f.containerId != id)
            continue;
        if (at >= f.sourceRange.start && at <= f.sourceRange.end) {
            best = &f;
            if (at < f.sourceRange.end)
                break;
        }
    }
    if (!best)
        return {};

    QTextLine line;
    if (!lineOf(layout, *best, &line))
        return {};

    return QRectF(xAt(*best, line, at), best->rect.top(), 1.0, best->rect.height());
}

} // 命名空间 editor::interaction 结束
