#include "SnapEngine.h"

#include <cmath>
#include <limits>

namespace editor::interaction {
namespace {

enum class EdgeKind {
    Start,
    Center,
    End
};

struct TargetLine {
    qreal pos = 0;
    qreal start = 0;
    qreal end = 0;
    EdgeKind kind = EdgeKind::Start;
};

bool isOwnedByDragged(
    const data::LayoutResult& layout,
    const data::ContainerId& candidateId,
    const data::ContainerId& draggedId)
{
    if (draggedId.isEmpty())
        return false;
    if (candidateId == draggedId)
        return true;
    for (const auto& box : layout.containers) {
        if (box.containerId == candidateId) {
            return box.parentId == draggedId;
        }
    }
    return false;
}

bool kindsCompatible(EdgeKind sourceKind, EdgeKind targetKind)
{
    // 中线仅吸附中线，边界仅吸附边界，避免拖过他物中线时产生多余黏滞阻力
    if (sourceKind == EdgeKind::Center || targetKind == EdgeKind::Center) {
        return sourceKind == targetKind;
    }
    return true;
}

void collectTargets(
    const data::LayoutResult& layout,
    const data::ContainerId& draggedId,
    std::vector<TargetLine>& xTargets,
    std::vector<TargetLine>& yTargets)
{
    // 1. 页面内容边距（正文区）
    const auto& cr = layout.contentRect;
    xTargets.push_back({cr.left(), cr.top(), cr.bottom(), EdgeKind::Start});
    xTargets.push_back({cr.center().x(), cr.top(), cr.bottom(), EdgeKind::Center});
    xTargets.push_back({cr.right(), cr.top(), cr.bottom(), EdgeKind::End});

    yTargets.push_back({cr.top(), cr.left(), cr.right(), EdgeKind::Start});
    yTargets.push_back({cr.center().y(), cr.left(), cr.right(), EdgeKind::Center});
    yTargets.push_back({cr.bottom(), cr.left(), cr.right(), EdgeKind::End});

    // 2. 其它容器（排除自身及其直接子容器，防止拖动父容器时向自身内部子图产生自吸附）
    for (const auto& box : layout.containers) {
        if (!draggedId.isEmpty() && (box.containerId == draggedId || box.parentId == draggedId))
            continue;
        const auto& r = box.rect;
        xTargets.push_back({r.left(), r.top(), r.bottom(), EdgeKind::Start});
        xTargets.push_back({r.center().x(), r.top(), r.bottom(), EdgeKind::Center});
        xTargets.push_back({r.right(), r.top(), r.bottom(), EdgeKind::End});

        yTargets.push_back({r.top(), r.left(), r.right(), EdgeKind::Start});
        yTargets.push_back({r.center().y(), r.left(), r.right(), EdgeKind::Center});
        yTargets.push_back({r.bottom(), r.left(), r.right(), EdgeKind::End});
    }

    // 3. 浮动/顺序图片（同样排除被拖容器拥有的图片）
    for (const auto& img : layout.images) {
        if (isOwnedByDragged(layout, img.containerId, draggedId))
            continue;
        const auto& r = img.rect;
        xTargets.push_back({r.left(), r.top(), r.bottom(), EdgeKind::Start});
        xTargets.push_back({r.center().x(), r.top(), r.bottom(), EdgeKind::Center});
        xTargets.push_back({r.right(), r.top(), r.bottom(), EdgeKind::End});

        yTargets.push_back({r.top(), r.left(), r.right(), EdgeKind::Start});
        yTargets.push_back({r.center().y(), r.left(), r.right(), EdgeKind::Center});
        yTargets.push_back({r.bottom(), r.left(), r.right(), EdgeKind::End});
    }
}

} // namespace

SnapResult SnapEngine::snapMove(
    const QRectF& currentRect,
    const data::LayoutResult& layout,
    const data::ContainerId& draggedId,
    qreal threshold,
    bool snapY)
{
    SnapResult result;
    result.snappedTopLeft = currentRect.topLeft();
    result.snappedWidth = currentRect.width();

    std::vector<TargetLine> xTargets;
    std::vector<TargetLine> yTargets;
    collectTargets(layout, draggedId, xTargets, yTargets);

    // X 轴吸附
    qreal bestDiffX = std::numeric_limits<qreal>::max();
    TargetLine bestTargetX;

    const std::pair<qreal, EdgeKind> candsX[3] = {
        {currentRect.left(), EdgeKind::Start},
        {currentRect.center().x(), EdgeKind::Center},
        {currentRect.right(), EdgeKind::End}
    };
    for (const auto& target : xTargets) {
        for (const auto& cand : candsX) {
            if (!kindsCompatible(cand.second, target.kind))
                continue;
            const qreal diff = target.pos - cand.first;
            if (std::abs(diff) < std::abs(bestDiffX)) {
                bestDiffX = diff;
                bestTargetX = target;
            }
        }
    }

    qreal deltaX = 0;
    if (std::abs(bestDiffX) <= threshold) {
        deltaX = bestDiffX;
        result.snappedX = true;

        Guideline gl;
        gl.orientation = GuidelineOrientation::Vertical;
        gl.position = bestTargetX.pos;
        gl.start = std::min({currentRect.top(), bestTargetX.start}) - 15.0;
        gl.end = std::max({currentRect.bottom(), bestTargetX.end}) + 15.0;
        result.guidelines.push_back(gl);
    }

    // Y 轴吸附（顺序容器重排时可关闭 Y 轴磁吸，避免拖动时出现减速带阻滞感）
    qreal deltaY = 0;
    if (snapY) {
        qreal bestDiffY = std::numeric_limits<qreal>::max();
        TargetLine bestTargetY;

        const std::pair<qreal, EdgeKind> candsY[3] = {
            {currentRect.top(), EdgeKind::Start},
            {currentRect.center().y(), EdgeKind::Center},
            {currentRect.bottom(), EdgeKind::End}
        };
        for (const auto& target : yTargets) {
            for (const auto& cand : candsY) {
                if (!kindsCompatible(cand.second, target.kind))
                    continue;
                const qreal diff = target.pos - cand.first;
                if (std::abs(diff) < std::abs(bestDiffY)) {
                    bestDiffY = diff;
                    bestTargetY = target;
                }
            }
        }

        if (std::abs(bestDiffY) <= threshold) {
            deltaY = bestDiffY;
            result.snappedY = true;

            Guideline gl;
            gl.orientation = GuidelineOrientation::Horizontal;
            gl.position = bestTargetY.pos;
            gl.start = std::min({currentRect.left(), bestTargetY.start}) - 15.0;
            gl.end = std::max({currentRect.right(), bestTargetY.end}) + 15.0;
            result.guidelines.push_back(gl);
        }
    }

    result.snappedTopLeft = currentRect.topLeft() + QPointF(deltaX, deltaY);
    return result;
}

SnapResult SnapEngine::snapResizeWidth(
    const QRectF& currentRect,
    bool isLeftHandle,
    const data::LayoutResult& layout,
    const data::ContainerId& draggedId,
    qreal threshold)
{
    SnapResult result;
    result.snappedTopLeft = currentRect.topLeft();
    result.snappedWidth = currentRect.width();

    std::vector<TargetLine> xTargets;
    std::vector<TargetLine> yTargets;
    collectTargets(layout, draggedId, xTargets, yTargets);

    const qreal edgeX = isLeftHandle ? currentRect.left() : currentRect.right();
    qreal bestDiffX = std::numeric_limits<qreal>::max();
    TargetLine bestTargetX;

    for (const auto& target : xTargets) {
        if (target.kind == EdgeKind::Center)
            continue;
        const qreal diff = target.pos - edgeX;
        if (std::abs(diff) < std::abs(bestDiffX)) {
            bestDiffX = diff;
            bestTargetX = target;
        }
    }

    if (std::abs(bestDiffX) <= threshold) {
        result.snappedX = true;
        if (isLeftHandle) {
            const qreal newLeft = bestTargetX.pos;
            result.snappedTopLeft.setX(newLeft);
            result.snappedWidth = currentRect.right() - newLeft;
        } else {
            const qreal newRight = bestTargetX.pos;
            result.snappedWidth = newRight - currentRect.left();
        }

        Guideline gl;
        gl.orientation = GuidelineOrientation::Vertical;
        gl.position = bestTargetX.pos;
        gl.start = std::min({currentRect.top(), bestTargetX.start}) - 15.0;
        gl.end = std::max({currentRect.bottom(), bestTargetX.end}) + 15.0;
        result.guidelines.push_back(gl);
    }

    return result;
}

SnapResult SnapEngine::snapResizeHeight(
    const QRectF& currentRect,
    bool isTopHandle,
    const data::LayoutResult& layout,
    const data::ContainerId& draggedId,
    qreal threshold)
{
    SnapResult result;
    result.snappedTopLeft = currentRect.topLeft();
    result.snappedHeight = currentRect.height();

    std::vector<TargetLine> xTargets;
    std::vector<TargetLine> yTargets;
    collectTargets(layout, draggedId, xTargets, yTargets);

    const qreal edgeY = isTopHandle ? currentRect.top() : currentRect.bottom();
    qreal bestDiffY = std::numeric_limits<qreal>::max();
    TargetLine bestTargetY;

    for (const auto& target : yTargets) {
        if (target.kind == EdgeKind::Center)
            continue;
        const qreal diff = target.pos - edgeY;
        if (std::abs(diff) < std::abs(bestDiffY)) {
            bestDiffY = diff;
            bestTargetY = target;
        }
    }

    if (std::abs(bestDiffY) <= threshold) {
        result.snappedY = true;
        if (isTopHandle) {
            const qreal newTop = bestTargetY.pos;
            result.snappedTopLeft.setY(newTop);
            result.snappedHeight = currentRect.bottom() - newTop;
        } else {
            const qreal newBottom = bestTargetY.pos;
            result.snappedHeight = newBottom - currentRect.top();
        }

        Guideline gl;
        gl.orientation = GuidelineOrientation::Horizontal;
        gl.position = bestTargetY.pos;
        gl.start = std::min({currentRect.left(), bestTargetY.start}) - 15.0;
        gl.end = std::max({currentRect.right(), bestTargetY.end}) + 15.0;
        result.guidelines.push_back(gl);
    }

    return result;
}

} // namespace editor::interaction
