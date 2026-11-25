#include "ContainerDragEngine.h"

#include <algorithm>
#include <cmath>

namespace editor::interaction {
namespace {

constexpr qreal kMinImageDisplayDim = 24.0;
constexpr qreal kMinContainerWidth = 50.0;
constexpr qreal kBaseSnapThresholdPx = 4.5;
constexpr qreal kMaxFlowCrossLeadPx = 36.0;

const data::ContainerNode* findNodeInTree(const data::ContainerNode& root, const data::ContainerId& id)
{
    if (root.id == id)
        return &root;
    for (const auto& child : root.children) {
        if (const auto* found = findNodeInTree(child, id))
            return found;
    }
    return nullptr;
}

bool findNodeParentAndIndex(
    const data::ContainerNode& root,
    const data::ContainerId& id,
    data::ContainerId& outParentId,
    std::size_t& outIndex)
{
    for (std::size_t i = 0; i < root.children.size(); ++i) {
        if (root.children[i].id == id) {
            outParentId = root.id;
            outIndex = i;
            return true;
        }
        if (findNodeParentAndIndex(root.children[i], id, outParentId, outIndex)) {
            return true;
        }
    }
    return false;
}

data::ContainerNode* findNodeAndParentInTree(
    data::ContainerNode& current,
    const data::ContainerId& id,
    data::ContainerNode* parent,
    data::ContainerNode** outParent)
{
    if (current.id == id) {
        if (outParent)
            *outParent = parent;
        return &current;
    }
    for (auto& child : current.children) {
        if (auto* found = findNodeAndParentInTree(child, id, &current, outParent)) {
            return found;
        }
    }
    return nullptr;
}

const data::ContainerLayout* findBoxInLayout(const data::LayoutResult& layout, const data::ContainerId& id)
{
    for (const auto& box : layout.containers) {
        if (box.containerId == id)
            return &box;
    }
    return nullptr;
}

struct SiblingBoxEntry {
    std::size_t childIndex = 0;
    const data::ContainerLayout* box = nullptr;
};

void computeFlowOrderAndIndicator(
    const data::ContainerNode* parentNode,
    const data::ContainerId& draggedId,
    const data::ContainerId& initialParentId,
    std::size_t initialIndex,
    const data::ContainerId& targetParentId,
    const QRectF& targetParentContentRect,
    const QRectF& targetRect,
    const data::LayoutResult& baseLayout,
    DragDropPlan& plan)
{
    if (!parentNode)
        return;

    std::vector<SiblingBoxEntry> siblings;
    siblings.reserve(parentNode->children.size());
    for (std::size_t i = 0; i < parentNode->children.size(); ++i) {
        const auto& child = parentNode->children[i];
        if (child.id == draggedId)
            continue;
        if (const auto* box = findBoxInLayout(baseLayout, child.id)) {
            siblings.push_back({i, box});
        }
    }

    const bool sameParent = (targetParentId == initialParentId);
    std::size_t countAbove = 0;

    if (sameParent) {
        // 使用前沿越界阈值 min(brotherHeight * 0.5, 36px) 进行方向性重排判定，
        // 避免长文本容器必须拖拽半个高度 (~200px+) 才能越过兄弟造成的拖拽阻塞感。
        for (const auto& entry : siblings) {
            const qreal lead = qMin<qreal>(entry.box->rect.height() * 0.5, kMaxFlowCrossLeadPx);
            if (entry.childIndex < initialIndex) {
                // 原本在上方：只有当被拖容器顶边向上越过该兄弟底边 - lead 时，才翻到其上方
                const qreal thresholdY = entry.box->rect.bottom() - lead;
                if (targetRect.top() >= thresholdY) {
                    ++countAbove;
                }
            } else {
                // 原本在下方：只要被拖容器底边向下越过该兄弟顶边 + lead，就移到其下方
                const qreal thresholdY = entry.box->rect.top() + lead;
                if (targetRect.bottom() > thresholdY) {
                    ++countAbove;
                }
            }
        }
    } else {
        const qreal targetCenterY = targetRect.center().y();
        for (const auto& entry : siblings) {
            if (entry.box->rect.center().y() < targetCenterY) {
                ++countAbove;
            }
        }
    }

    plan.targetIndex = countAbove;

    // 计算同级目标插槽的水平插入指示线
    if (!siblings.empty()) {
        qreal indY = siblings.front().box->rect.top();
        if (countAbove == 0) {
            indY = siblings.front().box->rect.top();
        } else if (countAbove >= siblings.size()) {
            indY = siblings.back().box->rect.bottom();
        } else {
            indY = (siblings[countAbove - 1].box->rect.bottom() + siblings[countAbove].box->rect.top()) * 0.5;
        }
        plan.insertionIndicator = QLineF(
            targetParentContentRect.left(), indY,
            targetParentContentRect.right(), indY);
    }
}

void computeFlowLocalPosition(
    const data::ContainerNode* draggedNode,
    const data::ContainerNode* targetParentNode,
    const data::ContainerId& draggedId,
    const data::ContainerId& initialParentId,
    std::size_t initialIndex,
    const QRectF& initialRect,
    const QPointF& delta,
    QRectF& targetRect,
    const data::LayoutResult& baseLayout,
    DragDropPlan& plan)
{
    // 1. 水平方向夹紧：铺满父宽度的默认文本/分组容器（width == 0）不允许拖出右侧纸张边界，
    // 避免 CanvasWidget::rebuildGeometry() 在拖动时反复扩缩固定尺寸并引发滚动闪烁。
    const bool isFullWidth = !draggedNode ||
        draggedNode->geometry.width == 0 ||
        initialRect.width() >= plan.targetParentContentRect.width() - 1.0;

    qreal relX = 0;
    if (isFullWidth) {
        relX = 0;
        targetRect.moveLeft(plan.targetParentContentRect.left());
    } else {
        const qreal maxRelX = qMax<qreal>(0, plan.targetParentContentRect.width() - initialRect.width());
        relX = qBound<qreal>(0, targetRect.left() - plan.targetParentContentRect.left(), maxRelX);
        targetRect.moveLeft(plan.targetParentContentRect.left() + relX);
    }

    // 2. 纵向坐标：发生换位或跨父迁移时归零以贴合新顺序位置，不残留无意义的纵向空白偏移
    std::size_t siblingCount = 0;
    if (targetParentNode) {
        for (const auto& child : targetParentNode->children) {
            if (child.id != draggedId && findBoxInLayout(baseLayout, child.id) != nullptr) {
                ++siblingCount;
            }
        }
    }

    const bool reordered = (plan.targetParentId != initialParentId) || (plan.targetIndex != initialIndex);
    const qreal initialPosY = draggedNode ? draggedNode->geometry.position.y() : 0;
    qreal relY = 0;

    if (reordered) {
        relY = 0;
    } else if (siblingCount == 0) {
        // 单个容器支持自由上下微调位置
        relY = qMax<qreal>(0, targetRect.top() - plan.targetParentContentRect.top());
    } else if (plan.targetIndex == siblingCount && delta.y() > 0) {
        // 末尾容器继续向下拉以拉开间距
        relY = qMax<qreal>(initialPosY, targetRect.top() - plan.targetParentContentRect.top());
    } else if (plan.targetIndex == 0 && initialPosY > 0 && delta.y() < 0) {
        // 首位容器有顶部留白时允许向上拉回顶部
        relY = qMax<qreal>(0, targetRect.top() - plan.targetParentContentRect.top());
    } else {
        relY = initialPosY;
    }

    plan.targetRect = targetRect;
    plan.localPosition = QPointF(relX, relY);
}

} // namespace

DragDropPlan ContainerDragEngine::planContainerMove(
    const data::ContainerId& draggedId,
    data::ContainerType draggedType,
    const QRectF& initialRect,
    const QPointF& delta,
    const QPointF& paperPos,
    const data::LayoutResult& baseLayout,
    const data::DocumentSnapshot& snapshot,
    qreal zoom)
{
    DragDropPlan plan;
    data::ContainerId initialParentId = snapshot.root.id;
    std::size_t initialIndex = 0;
    findNodeParentAndIndex(snapshot.root, draggedId, initialParentId, initialIndex);
    const auto* draggedNode = findNodeInTree(snapshot.root, draggedId);

    const qreal safeZoom = qMax<qreal>(0.25, zoom);
    const qreal snapThreshold = kBaseSnapThresholdPx / safeZoom;

    QRectF targetRect = initialRect.translated(delta);
    // 顺序容器禁用 Y 轴磁吸，杜绝纵向拖动时遭遇不可见边缘产生的顿挫与阻滞感
    auto snap = SnapEngine::snapMove(targetRect, baseLayout, draggedId, snapThreshold, /*snapY=*/false);
    targetRect.moveTopLeft(snap.snappedTopLeft);

    plan.guidelines = std::move(snap.guidelines);
    plan.targetParentId = snapshot.root.id;
    plan.targetParentContentRect = baseLayout.contentRect;

    // 1. 如果是叶节点文本容器（无子浮动图片），检测是否拖入了某个 Group 分组容器
    const bool isLeaf = !draggedNode || draggedNode->children.empty();
    if (draggedType == data::ContainerType::Text && isLeaf) {
        for (const auto& box : baseLayout.containers) {
            if (box.containerId == draggedId || box.containerId == snapshot.root.id)
                continue;
            const auto* node = findNodeInTree(snapshot.root, box.containerId);
            if (node && node->type == data::ContainerType::Group) {
                if (box.contentRect.contains(paperPos) || box.rect.contains(targetRect.center())) {
                    plan.targetParentId = box.containerId;
                    plan.targetParentContentRect = box.contentRect;
                    plan.targetParentHighlightRect = box.rect;
                    break;
                }
            }
        }
    }

    const auto* parentNode = (plan.targetParentId == snapshot.root.id)
        ? &snapshot.root
        : findNodeInTree(snapshot.root, plan.targetParentId);

    computeFlowOrderAndIndicator(
        parentNode, draggedId, initialParentId, initialIndex,
        plan.targetParentId, plan.targetParentContentRect,
        targetRect, baseLayout, plan);

    computeFlowLocalPosition(
        draggedNode, parentNode, draggedId, initialParentId, initialIndex,
        initialRect, delta, targetRect, baseLayout, plan);

    plan.placementMode = data::PlacementMode::Flow;
    plan.isValid = true;
    return plan;
}

DragDropPlan ContainerDragEngine::planImageMove(
    const data::ContainerId& draggedImageId,
    const data::ContainerId& initialParentId,
    const QRectF& initialRect,
    const QPointF& delta,
    const QPointF& paperPos,
    const data::LayoutResult& baseLayout,
    const data::DocumentSnapshot& snapshot,
    qreal zoom)
{
    DragDropPlan plan;
    data::ContainerId actualParentId = initialParentId;
    std::size_t initialIndex = 0;
    if (actualParentId.isEmpty()) {
        findNodeParentAndIndex(snapshot.root, draggedImageId, actualParentId, initialIndex);
    } else {
        data::ContainerId foundParent;
        if (findNodeParentAndIndex(snapshot.root, draggedImageId, foundParent, initialIndex)) {
            actualParentId = foundParent;
        }
    }

    const auto* initialParentNode = (actualParentId == snapshot.root.id || actualParentId.isEmpty())
        ? &snapshot.root
        : findNodeInTree(snapshot.root, actualParentId);
    const bool originallyFlow = initialParentNode && initialParentNode->type != data::ContainerType::Text;

    const qreal safeZoom = qMax<qreal>(0.25, zoom);
    const qreal snapThreshold = kBaseSnapThresholdPx / safeZoom;

    QRectF targetRect = initialRect.translated(delta);
    auto snap = SnapEngine::snapMove(targetRect, baseLayout, draggedImageId, snapThreshold, /*snapY=*/!originallyFlow);
    targetRect.moveTopLeft(snap.snappedTopLeft);

    plan.targetRect = targetRect;
    plan.guidelines = std::move(snap.guidelines);

    // 寻找包含鼠标指针或图片中心的目标文本容器（用于跨容器浮动绕排迁移）
    const data::ContainerLayout* bestBox = nullptr;
    qreal maxOverlapArea = -1;

    for (const auto& box : baseLayout.containers) {
        if (box.containerId == draggedImageId)
            continue;
        const auto* node = findNodeInTree(snapshot.root, box.containerId);
        if (!node || node->type != data::ContainerType::Text)
            continue;

        if (box.contentRect.contains(paperPos)) {
            bestBox = &box;
            break;
        }
        if (box.rect.contains(paperPos)) {
            if (!bestBox)
                bestBox = &box;
        } else if (box.contentRect.contains(targetRect.center())) {
            if (!bestBox)
                bestBox = &box;
        } else if (!originallyFlow) {
            const QRectF inter = box.rect.intersected(targetRect);
            if (!inter.isEmpty()) {
                const qreal area = inter.width() * inter.height();
                if (area > maxOverlapArea) {
                    maxOverlapArea = area;
                    bestBox = &box;
                }
            }
        }
    }

    if (bestBox) {
        plan.targetParentId = bestBox->containerId;
        plan.targetParentContentRect = bestBox->contentRect;
        plan.placementMode = data::PlacementMode::Floating;

        const auto* targetNode = findNodeInTree(snapshot.root, plan.targetParentId);
        plan.targetIndex = targetNode ? targetNode->children.size() : 0;

        const qreal maxRelX = qMax<qreal>(0, plan.targetParentContentRect.width() - targetRect.width());
        const qreal relX = qBound<qreal>(0, targetRect.left() - plan.targetParentContentRect.left(), maxRelX);
        const qreal relY = qMax<qreal>(0, targetRect.top() - plan.targetParentContentRect.top());
        targetRect.moveTopLeft(QPointF(plan.targetParentContentRect.left() + relX,
                                       plan.targetParentContentRect.top() + relY));
        plan.targetRect = targetRect;
        plan.localPosition = QPointF(relX, relY);
        plan.isValid = true;
        return plan;
    }

    // 未落在任何文本容器内：若原本为顺序图片则保持顺序重排，否则回落到原文本父容器
    if (originallyFlow) {
        plan.targetParentId = actualParentId.isEmpty() ? snapshot.root.id : actualParentId;
        if (plan.targetParentId == snapshot.root.id) {
            plan.targetParentContentRect = baseLayout.contentRect;
        } else if (const auto* pBox = findBoxInLayout(baseLayout, plan.targetParentId)) {
            plan.targetParentContentRect = pBox->contentRect;
        } else {
            plan.targetParentContentRect = baseLayout.contentRect;
        }

        const auto* parentNode = (plan.targetParentId == snapshot.root.id)
            ? &snapshot.root
            : findNodeInTree(snapshot.root, plan.targetParentId);

        computeFlowOrderAndIndicator(
            parentNode, draggedImageId, actualParentId, initialIndex,
            plan.targetParentId, plan.targetParentContentRect,
            targetRect, baseLayout, plan);

        const qreal maxRelX = qMax<qreal>(0, plan.targetParentContentRect.width() - targetRect.width());
        const qreal relX = qBound<qreal>(0, targetRect.left() - plan.targetParentContentRect.left(), maxRelX);
        targetRect.moveLeft(plan.targetParentContentRect.left() + relX);
        const bool reordered = (plan.targetIndex != initialIndex);
        const qreal relY = reordered ? 0.0 : qMax<qreal>(0, targetRect.top() - plan.targetParentContentRect.top());

        plan.targetRect = targetRect;
        plan.localPosition = QPointF(relX, relY);
        plan.placementMode = data::PlacementMode::Flow;
        plan.isValid = true;
        return plan;
    }

    plan.targetParentId = actualParentId;
    if (const auto* initBox = findBoxInLayout(baseLayout, actualParentId)) {
        plan.targetParentContentRect = initBox->contentRect;
    } else {
        plan.targetParentContentRect = baseLayout.contentRect;
    }

    const auto* targetNode = findNodeInTree(snapshot.root, plan.targetParentId);
    plan.targetIndex = targetNode ? targetNode->children.size() : 0;

    const qreal maxRelX = qMax<qreal>(0, plan.targetParentContentRect.width() - targetRect.width());
    const qreal relX = qBound<qreal>(0, targetRect.left() - plan.targetParentContentRect.left(), maxRelX);
    const qreal relY = qMax<qreal>(0, targetRect.top() - plan.targetParentContentRect.top());
    targetRect.moveTopLeft(QPointF(plan.targetParentContentRect.left() + relX,
                                   plan.targetParentContentRect.top() + relY));
    plan.targetRect = targetRect;
    plan.localPosition = QPointF(relX, relY);
    plan.placementMode = data::PlacementMode::Floating;
    plan.isValid = true;
    return plan;
}

DragDropPlan ContainerDragEngine::planResizeWidth(
    const data::ContainerId& draggedId,
    bool isLeftHandle,
    const QRectF& initialRect,
    const QPointF& delta,
    const QRectF& parentContentRect,
    const data::LayoutResult& baseLayout,
    qreal zoom)
{
    DragDropPlan plan;
    const qreal safeZoom = qMax<qreal>(0.25, zoom);
    const qreal snapThreshold = kBaseSnapThresholdPx / safeZoom;

    QRectF targetRect = initialRect;
    if (isLeftHandle) {
        const qreal anchorRight = initialRect.right();
        const qreal rawLeft = initialRect.left() + delta.x();
        const qreal minLeft = parentContentRect.left();
        const qreal maxLeft = anchorRight - kMinContainerWidth;
        const qreal clampedRawLeft = qBound(minLeft, rawLeft, qMax(minLeft, maxLeft));

        QRectF probeRect = initialRect;
        probeRect.setLeft(clampedRawLeft);
        auto snap = SnapEngine::snapResizeWidth(probeRect, true, baseLayout, draggedId, snapThreshold);

        qreal newLeft = snap.snappedX ? snap.snappedTopLeft.x() : clampedRawLeft;
        newLeft = qBound(minLeft, newLeft, qMax(minLeft, maxLeft));
        const qreal newWidth = qMax(kMinContainerWidth, anchorRight - newLeft);

        targetRect.setLeft(anchorRight - newWidth);
        targetRect.setRight(anchorRight);
        if (snap.snappedX && std::abs(newLeft - snap.snappedTopLeft.x()) < 0.5) {
            plan.guidelines = std::move(snap.guidelines);
        }
    } else {
        const qreal anchorLeft = initialRect.left();
        const qreal rawRight = initialRect.right() + delta.x();
        const qreal maxWidth = qMax(kMinContainerWidth, parentContentRect.right() - anchorLeft);

        QRectF probeRect = initialRect;
        probeRect.setRight(anchorLeft + qBound(kMinContainerWidth, rawRight - anchorLeft, maxWidth));
        auto snap = SnapEngine::snapResizeWidth(probeRect, false, baseLayout, draggedId, snapThreshold);

        qreal newWidth = (snap.snappedX && snap.snappedWidth > 0) ? snap.snappedWidth : probeRect.width();
        newWidth = qBound(kMinContainerWidth, newWidth, maxWidth);

        targetRect.setLeft(anchorLeft);
        targetRect.setWidth(newWidth);
        if (snap.snappedX && std::abs(newWidth - snap.snappedWidth) < 0.5) {
            plan.guidelines = std::move(snap.guidelines);
        }
    }

    plan.targetRect = targetRect;
    plan.targetParentContentRect = parentContentRect;
    plan.localPosition = QPointF(
        qMax<qreal>(0, targetRect.left() - parentContentRect.left()),
        qMax<qreal>(0, targetRect.top() - parentContentRect.top()));
    plan.isValid = true;
    return plan;
}

DragDropPlan ContainerDragEngine::planImageResize(
    const data::ContainerId& draggedImageId,
    ContainerHitType handle,
    const QRectF& initialFrameRect,
    const QSizeF& initialDisplaySize,
    const data::ContainerGeometry& initialGeometry,
    const QPointF& delta,
    const QRectF& parentContentRect,
    const data::LayoutResult& baseLayout,
    bool keepAspectRatio,
    qreal zoom)
{
    DragDropPlan plan;
    plan.targetParentContentRect = parentContentRect;

    const qreal safeZoom = qMax<qreal>(0.25, zoom);
    const qreal snapThreshold = kBaseSnapThresholdPx / safeZoom;

    const qreal padH = initialGeometry.padding.left() + initialGeometry.padding.right();
    const qreal padV = initialGeometry.padding.top() + initialGeometry.padding.bottom();
    const qreal initDispW = qMax<qreal>(
        kMinImageDisplayDim,
        initialDisplaySize.width() > 0 ? initialDisplaySize.width() : (initialFrameRect.width() - padH));
    const qreal initDispH = qMax<qreal>(
        kMinImageDisplayDim,
        initialDisplaySize.height() > 0 ? initialDisplaySize.height() : (initialFrameRect.height() - padV));
    const qreal aspect = initDispW / initDispH;

    const qreal availParentW = parentContentRect.width() - initialGeometry.margin.left() - initialGeometry.margin.right();
    const qreal maxContentDispW = qMax<qreal>(kMinImageDisplayDim, availParentW - padH);
    const qreal basePageH = baseLayout.contentRect.isValid() ? baseLayout.contentRect.height() : parentContentRect.height();
    const qreal maxContentDispH = qMax<qreal>(kMinImageDisplayDim, basePageH - padV);

    const bool isLeft = (handle == ContainerHitType::ResizeHandleLeft ||
                         handle == ContainerHitType::ResizeHandleTopLeft ||
                         handle == ContainerHitType::ResizeHandleBottomLeft);
    const bool isRight = (handle == ContainerHitType::ResizeHandleRight ||
                          handle == ContainerHitType::ResizeHandleTopRight ||
                          handle == ContainerHitType::ResizeHandleBottomRight);
    const bool isTop = (handle == ContainerHitType::ResizeHandleTop ||
                        handle == ContainerHitType::ResizeHandleTopLeft ||
                        handle == ContainerHitType::ResizeHandleTopRight);
    const bool isBottom = (handle == ContainerHitType::ResizeHandleBottom ||
                           handle == ContainerHitType::ResizeHandleBottomLeft ||
                           handle == ContainerHitType::ResizeHandleBottomRight);

    const qreal horizAnchorBoundW = isLeft
        ? qMax<qreal>(kMinImageDisplayDim, initialFrameRect.right() - parentContentRect.left() - padH)
        : (isRight
               ? qMax<qreal>(kMinImageDisplayDim, parentContentRect.right() - initialFrameRect.left() - padH)
               : qMax<qreal>(kMinImageDisplayDim, parentContentRect.right() - initialFrameRect.left() - padH));
    const qreal vertAnchorBoundH = isTop
        ? qMax<qreal>(kMinImageDisplayDim, initialFrameRect.bottom() - parentContentRect.top() - padV)
        : maxContentDispH;

    const qreal maxDispW = qMin(maxContentDispW, horizAnchorBoundW);
    const qreal maxDispH = qMin(maxContentDispH, vertAnchorBoundH);

    qreal unsnappedDispW = initDispW;
    qreal unsnappedDispH = initDispH;
    qreal rawDispW = initDispW;
    qreal rawDispH = initDispH;
    SnapResult snapW;
    SnapResult snapH;

    if (isLeft) {
        QRectF probe = initialFrameRect;
        probe.setLeft(initialFrameRect.left() + delta.x());
        if (probe.width() < kMinImageDisplayDim + padH)
            probe.setLeft(initialFrameRect.right() - (kMinImageDisplayDim + padH));
        unsnappedDispW = probe.width() - padH;
        snapW = SnapEngine::snapResizeWidth(probe, true, baseLayout, draggedImageId, snapThreshold);
        const qreal frameW = snapW.snappedX ? snapW.snappedWidth : probe.width();
        rawDispW = frameW - padH;
    } else if (isRight) {
        QRectF probe = initialFrameRect;
        probe.setRight(initialFrameRect.right() + delta.x());
        if (probe.width() < kMinImageDisplayDim + padH)
            probe.setRight(initialFrameRect.left() + (kMinImageDisplayDim + padH));
        unsnappedDispW = probe.width() - padH;
        snapW = SnapEngine::snapResizeWidth(probe, false, baseLayout, draggedImageId, snapThreshold);
        const qreal frameW = snapW.snappedX ? snapW.snappedWidth : probe.width();
        rawDispW = frameW - padH;
    }

    if (isTop) {
        QRectF probe = initialFrameRect;
        probe.setTop(initialFrameRect.top() + delta.y());
        if (probe.height() < kMinImageDisplayDim + padV)
            probe.setTop(initialFrameRect.bottom() - (kMinImageDisplayDim + padV));
        unsnappedDispH = probe.height() - padV;
        snapH = SnapEngine::snapResizeHeight(probe, true, baseLayout, draggedImageId, snapThreshold);
        const qreal frameH = snapH.snappedY ? snapH.snappedHeight : probe.height();
        rawDispH = frameH - padV;
    } else if (isBottom) {
        QRectF probe = initialFrameRect;
        probe.setBottom(initialFrameRect.bottom() + delta.y());
        if (probe.height() < kMinImageDisplayDim + padV)
            probe.setBottom(initialFrameRect.top() + (kMinImageDisplayDim + padV));
        unsnappedDispH = probe.height() - padV;
        snapH = SnapEngine::snapResizeHeight(probe, false, baseLayout, draggedImageId, snapThreshold);
        const qreal frameH = snapH.snappedY ? snapH.snappedHeight : probe.height();
        rawDispH = frameH - padV;
    }

    qreal finalDispW = initDispW;
    qreal finalDispH = initDispH;

    const bool hasHoriz = isLeft || isRight;
    const bool hasVert = isTop || isBottom;

    if (keepAspectRatio) {
        qreal chosenScale = 1.0;
        if (hasHoriz && hasVert) {
            // 先按原始光标主导位移决定主控轴，避免从属轴偶然路过参照线时劫持等比角点缩放
            const qreal rawScaleDiffW = std::abs(unsnappedDispW / initDispW - 1.0);
            const qreal rawScaleDiffH = std::abs(unsnappedDispH / initDispH - 1.0);
            chosenScale = (rawScaleDiffW >= rawScaleDiffH) ? (rawDispW / initDispW) : (rawDispH / initDispH);
        } else if (hasHoriz) {
            chosenScale = rawDispW / initDispW;
        } else if (hasVert) {
            chosenScale = rawDispH / initDispH;
        }

        const qreal minScale = qMax(kMinImageDisplayDim / initDispW, kMinImageDisplayDim / initDispH);
        qreal maxScale = qMin(maxDispW / initDispW, maxDispH / initDispH);
        if (maxScale < minScale)
            maxScale = minScale;
        chosenScale = qBound(minScale, chosenScale, maxScale);

        finalDispW = initDispW * chosenScale;
        finalDispH = finalDispW / aspect;
        if (finalDispH < kMinImageDisplayDim) {
            finalDispH = kMinImageDisplayDim;
            finalDispW = finalDispH * aspect;
        }
        if (snapW.snappedX && hasHoriz && std::abs(finalDispW - rawDispW) < 0.5) {
            plan.guidelines.insert(plan.guidelines.end(), snapW.guidelines.begin(), snapW.guidelines.end());
        }
        if (snapH.snappedY && hasVert && std::abs(finalDispH - rawDispH) < 0.5) {
            plan.guidelines.insert(plan.guidelines.end(), snapH.guidelines.begin(), snapH.guidelines.end());
        }
    } else {
        if (hasHoriz) {
            finalDispW = qBound(kMinImageDisplayDim, rawDispW, maxDispW);
            if (snapW.snappedX && std::abs(finalDispW - rawDispW) < 0.5) {
                plan.guidelines.insert(plan.guidelines.end(), snapW.guidelines.begin(), snapW.guidelines.end());
            }
        }
        if (hasVert) {
            finalDispH = qBound(kMinImageDisplayDim, rawDispH, maxDispH);
            if (snapH.snappedY && std::abs(finalDispH - rawDispH) < 0.5) {
                plan.guidelines.insert(plan.guidelines.end(), snapH.guidelines.begin(), snapH.guidelines.end());
            }
        }
    }

    const qreal finalFrameW = finalDispW + padH;
    const qreal finalFrameH = finalDispH + padV;

    QRectF newFrameRect(initialFrameRect.left(), initialFrameRect.top(), finalFrameW, finalFrameH);
    if (isLeft) {
        newFrameRect.moveRight(initialFrameRect.right());
    }
    if (isTop) {
        newFrameRect.moveBottom(initialFrameRect.bottom());
    }

    const qreal relX = qMax<qreal>(0, newFrameRect.left() - parentContentRect.left());
    const qreal relY = qMax<qreal>(0, newFrameRect.top() - parentContentRect.top());
    newFrameRect.moveTopLeft(QPointF(parentContentRect.left() + relX, parentContentRect.top() + relY));

    if (const auto* imgBox = findBoxInLayout(baseLayout, draggedImageId)) {
        plan.targetParentId = imgBox->parentId;
        plan.placementMode = (!imgBox->parentId.isEmpty() && imgBox->parentId != QStringLiteral("root"))
            ? data::PlacementMode::Floating
            : data::PlacementMode::Flow;
    }
    plan.targetDisplaySize = QSizeF(finalDispW, finalDispH);
    plan.localPosition = QPointF(relX, relY);
    plan.targetRect = newFrameRect;
    plan.isValid = true;
    return plan;
}

bool ContainerDragEngine::applyPlanToCandidateSnapshot(
    data::DocumentSnapshot& candidate,
    const data::ContainerId& draggedId,
    const DragDropPlan& plan,
    const data::ContainerGeometry& geometry)
{
    data::ContainerNode* currentParent = nullptr;
    data::ContainerNode* node = findNodeAndParentInTree(candidate.root, draggedId, nullptr, &currentParent);
    if (!node || !currentParent)
        return false;

    node->geometry = geometry;
    node->geometry.position = plan.localPosition;
    if (plan.targetDisplaySize.has_value() && node->type == data::ContainerType::Image) {
        node->image.displaySize = *plan.targetDisplaySize;
    }

    // 仅在原地调整尺寸/几何时，不改变父级与兄弟排序
    if (plan.targetParentId.isEmpty() || plan.targetDisplaySize.has_value()) {
        return true;
    }

    const auto resolvedTargetParentId = plan.targetParentId;
    data::ContainerNode* targetParent = (resolvedTargetParentId == candidate.root.id)
        ? &candidate.root
        : const_cast<data::ContainerNode*>(findNodeInTree(candidate.root, resolvedTargetParentId));
    if (!targetParent || targetParent->id == draggedId)
        return false;

    node->placement = plan.placementMode;

    if (currentParent == targetParent) {
        auto it = std::find_if(currentParent->children.begin(), currentParent->children.end(),
                               [&](const auto& c) { return c.id == draggedId; });
        if (it != currentParent->children.end()) {
            const auto maxIdx = currentParent->children.size() > 0 ? currentParent->children.size() - 1 : 0;
            const auto clampedIdx = qMin<std::size_t>(plan.targetIndex, maxIdx);
            auto targetIt = currentParent->children.begin() + static_cast<std::ptrdiff_t>(clampedIdx);
            if (it < targetIt) {
                std::rotate(it, it + 1, targetIt + 1);
            } else if (it > targetIt) {
                std::rotate(targetIt, it, it + 1);
            }
        }
    } else {
        auto it = std::find_if(currentParent->children.begin(), currentParent->children.end(),
                               [&](const auto& c) { return c.id == draggedId; });
        if (it != currentParent->children.end()) {
            data::ContainerNode extracted = std::move(*it);
            currentParent->children.erase(it);
            const auto insertIdx = qMin<std::size_t>(plan.targetIndex, targetParent->children.size());
            targetParent->children.insert(targetParent->children.begin() + static_cast<std::ptrdiff_t>(insertIdx),
                                          std::move(extracted));
        }
    }
    return true;
}

} // namespace editor::interaction
