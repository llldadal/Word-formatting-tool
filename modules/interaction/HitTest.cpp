#include "HitTest.h"
#include "TextBoundary.h"

#include <QTextLayout>
#include <QTextLine>

#include <cmath>
#include <limits>

namespace editor::interaction {
namespace {

qreal verticalGap(const QRectF& rect, qreal y)
{
    return std::max<qreal>(0, std::max(rect.top() - y, y - rect.bottom()));
}

qreal horizontalGap(const QRectF& rect, qreal x)
{
    return std::max<qreal>(0, std::max(rect.left() - x, x - rect.right()));
}

// 优先取盖住该点的片段。落在空白处时：先找出垂直方向最近的那一行，
// 再在同一行的候选里取水平方向最近的片段。
// 旧实现只比较 y，同一高度并排放着两个文本容器时，点右边容器的空白会
// 被归到左边容器，因为二者 y 距离相同而容器顺序决定胜负。
const data::TextFragment* findFragment(const data::LayoutResult& layout, QPointF paper)
{
    const data::TextFragment* inside = nullptr;
    qreal bestInsideDy = std::numeric_limits<qreal>::max();
    for (const auto& f : layout.textFragments) {
        const qreal dy = verticalGap(f.rect, paper.y());
        if (f.rect.contains(paper) && dy < bestInsideDy) {
            inside = &f;
            bestInsideDy = dy;
        }
    }
    if (inside)
        return inside;
    if (layout.textFragments.empty())
        return nullptr;

    // 落在空白处时综合水平、垂直距离打分。垂直距离乘 2，保证「先找最近的
    // 一行」这个直觉仍然成立；同时水平距离足够大时能把点击归回右边那个容器。
    // 旧实现完全不看 x，同一高度并排放着容器时会一律选到遍历顺序靠前的那个。
    const data::TextFragment* nearest = nullptr;
    qreal bestScore = std::numeric_limits<qreal>::max();
    for (const auto& f : layout.textFragments) {
        const qreal score = horizontalGap(f.rect, paper.x())
                          + 2.0 * verticalGap(f.rect, paper.y());
        if (score < bestScore) {
            bestScore = score;
            nearest = &f;
        }
    }
    return nearest;
}

} // 匿名命名空间结束

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
    // 点在片段外时先把 x 夹回片段的左右边界，避免空白点击换算出一个
    // 与所见位置不符的偏移。
    const qreal x = qBound(f->rect.left(), paper.x(), f->rect.right());
    int layoutPos = qBound(lineBegin, line.xToCursor(x), lineEnd);

    // xToCursor 按 UTF-16 单元返回，可能落在 emoji 代理对或「字母+组合音标」
    // 中间。对齐到最近的字形簇边界，光标才不会把一个字切成两半。
    const QString layoutText = textLayout.text();
    if (layoutPos > lineBegin && layoutPos < lineEnd)
        layoutPos = alignToGrapheme(layoutText, layoutPos);

    const int delta  = layoutPos - lineBegin;               // 片段内偏移
    const int offset = qBound(f->sourceRange.start,
                              f->sourceRange.start + delta,
                              f->sourceRange.end);
    return { f->containerId, offset };
}

} // 命名空间 editor::interaction 结束
