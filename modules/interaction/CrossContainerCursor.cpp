#include "CrossContainerCursor.h"
#include "InteractionController.h"
#include "TextBoundary.h"

#include <QTextLayout>
#include <QTextLine>
#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::interaction {
namespace {

void collectTextNodes(const data::ContainerNode& node, std::vector<data::ContainerId>& out)
{
    if (node.type == data::ContainerType::Text) {
        out.push_back(node.id);
    }
    for (const auto& child : node.children) {
        collectTextNodes(child, out);
    }
}

QRectF findContainerContentBox(const data::ContainerId& id, const data::LayoutResult& layout)
{
    for (const auto& c : layout.containers) {
        if (c.containerId == id) {
            return c.contentRect;
        }
    }
    QRectF box;
    bool found = false;
    for (const auto& f : layout.textFragments) {
        if (f.containerId == id) {
            box = found ? box.united(f.rect) : f.rect;
            found = true;
        }
    }
    return box;
}

int calculateTextOffsetAtX(const std::vector<const data::TextFragment*>& lineFrags,
                           qreal targetX,
                           const data::LayoutResult& layout)
{
    if (lineFrags.empty())
        return 0;

    const data::TextFragment* bestF = nullptr;
    qreal bestDist = std::numeric_limits<qreal>::max();
    for (const auto* f : lineFrags) {
        qreal d = 0;
        if (targetX < f->rect.left()) {
            d = f->rect.left() - targetX;
        } else if (targetX > f->rect.right()) {
            d = targetX - f->rect.right();
        }
        if (d < bestDist) {
            bestDist = d;
            bestF = f;
        }
    }

    if (!bestF)
        return 0;

    if (bestF->layoutIndex >= 0 && size_t(bestF->layoutIndex) < layout.textLayouts.size()) {
        const auto& tl = *layout.textLayouts[size_t(bestF->layoutIndex)];
        if (bestF->lineIndex >= 0 && bestF->lineIndex < tl.lineCount()) {
            QTextLine line = tl.lineAt(bestF->lineIndex);
            const int lineBegin = line.textStart();
            const int lineEnd = lineBegin + line.textLength();
            const qreal xClamped = qBound(bestF->rect.left(), targetX, bestF->rect.right());
            int layoutPos = qBound(lineBegin, line.xToCursor(xClamped), lineEnd);
            const QString layoutText = tl.text();
            if (layoutPos > lineBegin && layoutPos < lineEnd) {
                layoutPos = alignToGrapheme(layoutText, layoutPos);
            }
            const int delta = layoutPos - lineBegin;
            return qBound(bestF->sourceRange.start, bestF->sourceRange.start + delta, bestF->sourceRange.end);
        }
    }

    return bestF->sourceRange.start;
}

} // namespace

std::vector<data::ContainerId> buildVisualTextOrder(const data::DocumentSnapshot& doc,
                                                    const data::LayoutResult& layout)
{
    std::vector<data::ContainerId> textIds;
    collectTextNodes(doc.root, textIds);
    if (textIds.size() <= 1)
        return textIds;

    struct Item {
        data::ContainerId id;
        QRectF rect;
        size_t treeIndex;
    };

    std::vector<Item> items;
    items.reserve(textIds.size());
    for (size_t i = 0; i < textIds.size(); ++i) {
        const auto& id = textIds[i];
        items.push_back({id, findContainerContentBox(id, layout), i});
    }

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (std::abs(a.rect.top() - b.rect.top()) > 10.0) {
            return a.rect.top() < b.rect.top();
        }
        if (std::abs(a.rect.left() - b.rect.left()) > 5.0) {
            return a.rect.left() < b.rect.left();
        }
        return a.treeIndex < b.treeIndex;
    });

    std::vector<data::ContainerId> sorted;
    sorted.reserve(items.size());
    for (const auto& item : items) {
        sorted.push_back(item.id);
    }
    return sorted;
}

std::optional<InteractionState> tryCrossLeft(const InteractionState& state,
                                             const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout)
{
    if (state.selection.has_value() || !state.preedit.text.isEmpty())
        return std::nullopt;

    if (state.caret.offset > 0)
        return std::nullopt;

    const auto order = buildVisualTextOrder(doc, layout);
    const auto it = std::find(order.begin(), order.end(), state.caret.containerId);
    if (it == order.end() || it == order.begin())
        return std::nullopt;

    const auto targetId = *(it - 1);
    const int targetOffset = textLength(doc, targetId);

    InteractionState newState = makeInitialState(doc, layout, targetId, targetOffset);
    newState.pendingCharStyle = state.pendingCharStyle; // 保留待用字符格式
    newState.desiredCaretX.reset();
    return newState;
}

std::optional<InteractionState> tryCrossRight(const InteractionState& state,
                                              const data::DocumentSnapshot& doc,
                                              const data::LayoutResult& layout)
{
    if (state.selection.has_value() || !state.preedit.text.isEmpty())
        return std::nullopt;

    const int currentLen = textLength(doc, state.caret.containerId);
    if (state.caret.offset < currentLen)
        return std::nullopt;

    const auto order = buildVisualTextOrder(doc, layout);
    const auto it = std::find(order.begin(), order.end(), state.caret.containerId);
    if (it == order.end() || (it + 1) == order.end())
        return std::nullopt;

    const auto targetId = *(it + 1);

    InteractionState newState = makeInitialState(doc, layout, targetId, 0);
    newState.pendingCharStyle = state.pendingCharStyle; // 保留待用字符格式
    newState.desiredCaretX.reset();
    return newState;
}

std::optional<InteractionState> tryCrossVertical(const InteractionState& state,
                                                 const data::DocumentSnapshot& doc,
                                                 const data::LayoutResult& layout,
                                                 bool down)
{
    if (state.selection.has_value() || !state.preedit.text.isEmpty())
        return std::nullopt;

    const auto currId = state.caret.containerId;
    const qreal targetX = state.desiredCaretX.value_or(state.caret.rect.center().x());

    std::vector<const data::TextFragment*> currFrags;
    for (const auto& f : layout.textFragments) {
        if (f.containerId == currId)
            currFrags.push_back(&f);
    }

    if (!currFrags.empty()) {
        qreal minTop = std::numeric_limits<qreal>::max();
        qreal maxBottom = -std::numeric_limits<qreal>::max();
        for (const auto* f : currFrags) {
            if (f->rect.top() < minTop)
                minTop = f->rect.top();
            if (f->rect.bottom() > maxBottom)
                maxBottom = f->rect.bottom();
        }

        const data::TextFragment* caretFrag = nullptr;
        for (const auto* f : currFrags) {
            if (state.caret.offset >= f->sourceRange.start && state.caret.offset <= f->sourceRange.end) {
                caretFrag = f;
                if (state.caret.offset < f->sourceRange.end)
                    break;
            }
        }

        if (caretFrag) {
            if (!down) {
                // 向上移动：如果不是在当前容器首行，则不跨容器
                if (caretFrag->rect.top() > minTop + 4.0)
                    return std::nullopt;
            } else {
                // 向下移动：如果不是在当前容器末行，则不跨容器
                if (caretFrag->rect.bottom() < maxBottom - 4.0)
                    return std::nullopt;
            }
        }
    }

    const QRectF currBox = findContainerContentBox(currId, layout);
    const auto order = buildVisualTextOrder(doc, layout);
    data::ContainerId bestTarget;
    qreal bestScore = std::numeric_limits<qreal>::max();

    for (const auto& candId : order) {
        if (candId == currId)
            continue;

        const QRectF candBox = findContainerContentBox(candId, layout);
        if (!candBox.isValid() && candBox.isEmpty())
            continue;

        if (!down) {
            // 候选容器必须在上方
            if (candBox.bottom() > currBox.top() + 5.0)
                continue;
            const qreal dy = qMax<qreal>(0, currBox.top() - candBox.bottom());
            qreal dx = 0;
            if (targetX < candBox.left())
                dx = candBox.left() - targetX;
            else if (targetX > candBox.right())
                dx = targetX - candBox.right();
            const qreal score = dy * 2.0 + dx;
            if (score < bestScore) {
                bestScore = score;
                bestTarget = candId;
            }
        } else {
            // 候选容器必须在下方
            if (candBox.top() < currBox.bottom() - 5.0)
                continue;
            const qreal dy = qMax<qreal>(0, candBox.top() - currBox.bottom());
            qreal dx = 0;
            if (targetX < candBox.left())
                dx = candBox.left() - targetX;
            else if (targetX > candBox.right())
                dx = targetX - candBox.right();
            const qreal score = dy * 2.0 + dx;
            if (score < bestScore) {
                bestScore = score;
                bestTarget = candId;
            }
        }
    }

    if (bestTarget.isEmpty())
        return std::nullopt;

    // 计算落点目标容器中的字符偏移
    std::vector<const data::TextFragment*> targetFrags;
    for (const auto& f : layout.textFragments) {
        if (f.containerId == bestTarget)
            targetFrags.push_back(&f);
    }

    int targetOffset = 0;
    if (!targetFrags.empty()) {
        if (!down) {
            // 跨入上方容器：落在底行
            qreal maxB = -std::numeric_limits<qreal>::max();
            for (const auto* f : targetFrags) {
                if (f->rect.bottom() > maxB)
                    maxB = f->rect.bottom();
            }
            std::vector<const data::TextFragment*> lineFrags;
            for (const auto* f : targetFrags) {
                if (f->rect.bottom() >= maxB - 4.0)
                    lineFrags.push_back(f);
            }
            targetOffset = calculateTextOffsetAtX(lineFrags, targetX, layout);
        } else {
            // 跨入下方容器：落在首行
            qreal minT = std::numeric_limits<qreal>::max();
            for (const auto* f : targetFrags) {
                if (f->rect.top() < minT)
                    minT = f->rect.top();
            }
            std::vector<const data::TextFragment*> lineFrags;
            for (const auto* f : targetFrags) {
                if (f->rect.top() <= minT + 4.0)
                    lineFrags.push_back(f);
            }
            targetOffset = calculateTextOffsetAtX(lineFrags, targetX, layout);
        }
    }

    InteractionState newState = makeInitialState(doc, layout, bestTarget, targetOffset);
    newState.pendingCharStyle = state.pendingCharStyle; // 保留待用字符格式
    newState.desiredCaretX = targetX;                   // 保留期望列 X 坐标
    return newState;
}

} // namespace editor::interaction
