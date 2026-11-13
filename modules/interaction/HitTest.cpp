#include "HitTest.h"

#include <QTextLayout>
#include <QTextLine>

#include <cmath>
#include <limits>

namespace editor::interaction {
namespace {

// 优先取盖住该点的片段；没盖住时取垂直距离最近的一行，供拖选夹取用。
const data::TextFragment* findFragment(const data::LayoutResult& layout, QPointF paper)
{
    const data::TextFragment* inside  = nullptr;
    const data::TextFragment* nearest = nullptr;
    qreal bestInsideDy = std::numeric_limits<qreal>::max();
    qreal bestNearDy   = std::numeric_limits<qreal>::max();

    for (const auto& f : layout.textFragments) {
        const qreal dy = std::fabs(paper.y() - f.rect.center().y());
        if (f.rect.contains(paper) && dy < bestInsideDy) {
            inside = &f;
            bestInsideDy = dy;
        }
        if (dy < bestNearDy) {
            nearest = &f;
            bestNearDy = dy;
        }
    }
    return inside ? inside : nearest;
}

} // namespace

Hit hitTest(const data::LayoutResult& layout, QPointF paper)
{
    const data::TextFragment* f = findFragment(layout, paper);
    if (!f)
        return {};

    if (f->layoutIndex < 0 || f->layoutIndex >= int(layout.textLayouts.size()))
        return {};
    const auto& textLayout = *layout.textLayouts.at(size_t(f->layoutIndex));
    if (f->lineIndex < 0 || f->lineIndex >= textLayout.lineCount())
        return {};

    const QTextLine line = textLayout.lineAt(f->lineIndex);

    // QTextLine::xToCursor() 吃的是排版坐标系的 x，已经含 line.position()，
    // 即纸张坐标。不要再减 line.position().x()，否则被图片切开的右半片段
    // 所有点击都会被夹到片段起点。
    const int lineBegin = line.textStart();
    const int lineEnd   = lineBegin + line.textLength();
    const int layoutPos = qBound(lineBegin, line.xToCursor(paper.x()), lineEnd);
    const int delta     = layoutPos - lineBegin;               // 片段内偏移
    const int offset    = qBound(f->sourceRange.start,
                                 f->sourceRange.start + delta,
                                 f->sourceRange.end);
    return { f->containerId, offset };
}

} // namespace editor::interaction
