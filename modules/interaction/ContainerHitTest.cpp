#include "ContainerHitTest.h"
#include "HitTest.h"

#include <cmath>
#include <optional>

namespace editor::interaction {
namespace {

const data::ContainerNode* findNodeRecursive(const data::ContainerNode& parent, const data::ContainerId& id)
{
    if (parent.id == id)
        return &parent;
    for (const auto& child : parent.children) {
        if (const auto* found = findNodeRecursive(child, id))
            return found;
    }
    return nullptr;
}

data::ContainerType nodeType(const data::DocumentSnapshot& snapshot, const data::ContainerId& id)
{
    if (const auto* node = findNodeRecursive(snapshot.root, id))
        return node->type;
    return data::ContainerType::Text;
}

bool isTopLevelTextContainer(const data::DocumentSnapshot& snapshot, const data::ContainerId& id)
{
    for (const auto& child : snapshot.root.children) {
        if (child.id == id) {
            return child.type == data::ContainerType::Text;
        }
    }
    return false;
}

QRectF makeHorizHandleRect(QPointF center, qreal safeZoom)
{
    const qreal hw = 8.0 / safeZoom;
    const qreal hh = 16.0 / safeZoom;
    return QRectF(center.x() - hw / 2.0, center.y() - hh / 2.0, hw, hh);
}

QRectF makeVertHandleRect(QPointF center, qreal safeZoom)
{
    const qreal hw = 16.0 / safeZoom;
    const qreal hh = 8.0 / safeZoom;
    return QRectF(center.x() - hw / 2.0, center.y() - hh / 2.0, hw, hh);
}

QRectF makeCornerHandleRect(QPointF center, qreal safeZoom)
{
    const qreal size = 10.0 / safeZoom;
    return QRectF(center.x() - size / 2.0, center.y() - size / 2.0, size, size);
}

std::optional<ContainerHitResult> hitTestImageHandles(
    const QPointF& paperPos,
    const QRectF& rect,
    const data::ContainerId& id,
    const data::ContainerId& parentId,
    qreal safeZoom)
{
    // 优先检测 4 个角点手柄，防止小图下边缘手柄抢占角点
    if (makeCornerHandleRect(rect.topLeft(), safeZoom).contains(paperPos))
        return ContainerHitResult{ContainerHitType::ResizeHandleTopLeft, id, parentId, -1};
    if (makeCornerHandleRect(rect.topRight(), safeZoom).contains(paperPos))
        return ContainerHitResult{ContainerHitType::ResizeHandleTopRight, id, parentId, -1};
    if (makeCornerHandleRect(rect.bottomLeft(), safeZoom).contains(paperPos))
        return ContainerHitResult{ContainerHitType::ResizeHandleBottomLeft, id, parentId, -1};
    if (makeCornerHandleRect(rect.bottomRight(), safeZoom).contains(paperPos))
        return ContainerHitResult{ContainerHitType::ResizeHandleBottomRight, id, parentId, -1};

    const qreal minEdgeSpan = 24.0 / safeZoom;
    if (rect.height() >= minEdgeSpan) {
        if (makeHorizHandleRect(QPointF(rect.left(), rect.center().y()), safeZoom).contains(paperPos))
            return ContainerHitResult{ContainerHitType::ResizeHandleLeft, id, parentId, -1};
        if (makeHorizHandleRect(QPointF(rect.right(), rect.center().y()), safeZoom).contains(paperPos))
            return ContainerHitResult{ContainerHitType::ResizeHandleRight, id, parentId, -1};
    }
    if (rect.width() >= minEdgeSpan) {
        if (makeVertHandleRect(QPointF(rect.center().x(), rect.top()), safeZoom).contains(paperPos))
            return ContainerHitResult{ContainerHitType::ResizeHandleTop, id, parentId, -1};
        if (makeVertHandleRect(QPointF(rect.center().x(), rect.bottom()), safeZoom).contains(paperPos))
            return ContainerHitResult{ContainerHitType::ResizeHandleBottom, id, parentId, -1};
    }
    return std::nullopt;
}

} // namespace

ContainerHitResult hitTestScene(
    const QPointF& paperPos,
    const data::LayoutResult& layout,
    const data::DocumentSnapshot& snapshot,
    const ContainerSelectionState& selectionState,
    qreal zoom)
{
    const qreal safeZoom = qMax(0.1, zoom);
    const qreal borderTol = 5.0 / safeZoom;

    // 1. 优先检测已选中容器/图片的调整手柄
    for (const auto& selectedId : selectionState.selectedIds) {
        bool checkedContainer = false;
        for (const auto& box : layout.containers) {
            if (box.containerId != selectedId)
                continue;
            checkedContainer = true;
            const auto type = nodeType(snapshot, box.containerId);
            // 正文顶层容器不支持调整宽度，不暴露左右拉伸手柄
            if (type == data::ContainerType::Group ||
                (type == data::ContainerType::Text && !isTopLevelTextContainer(snapshot, box.containerId))) {
                const QRectF leftHandle = makeHorizHandleRect(QPointF(box.rect.left(), box.rect.center().y()), safeZoom);
                if (leftHandle.contains(paperPos)) {
                    return {ContainerHitType::ResizeHandleLeft, box.containerId, box.parentId, -1};
                }

                const QRectF rightHandle = makeHorizHandleRect(QPointF(box.rect.right(), box.rect.center().y()), safeZoom);
                if (rightHandle.contains(paperPos)) {
                    return {ContainerHitType::ResizeHandleRight, box.containerId, box.parentId, -1};
                }
            } else if (type == data::ContainerType::Image) {
                if (const auto hit = hitTestImageHandles(paperPos, box.rect, box.containerId, box.parentId, safeZoom)) {
                    return *hit;
                }
            }
        }
        if (!checkedContainer) {
            for (const auto& img : layout.images) {
                if (img.containerId != selectedId)
                    continue;
                if (const auto hit = hitTestImageHandles(paperPos, img.rect, img.containerId, {}, safeZoom)) {
                    return *hit;
                }
            }
        }
    }

    // 2. 检测文本/分组容器边框 (5px 边缘带；图片边缘交由图片主体命中，不误判为 ContainerBorder)
    // 逆序遍历，后排版的（子容器/顶层容器）优先命中
    for (auto it = layout.containers.rbegin(); it != layout.containers.rend(); ++it) {
        const auto& box = *it;
        if (nodeType(snapshot, box.containerId) == data::ContainerType::Image)
            continue;
        const QRectF outer = box.rect.adjusted(-borderTol, -borderTol, borderTol, borderTol);
        const QRectF inner = box.rect.adjusted(borderTol, borderTol, -borderTol, -borderTol);

        if (outer.contains(paperPos) && !inner.contains(paperPos)) {
            return {ContainerHitType::ContainerBorder, box.containerId, box.parentId, -1};
        }
    }

    // 3. 检测图片主体（优先按容器框命中以覆盖带 padding 的边框区域）
    for (auto it = layout.containers.rbegin(); it != layout.containers.rend(); ++it) {
        const auto& box = *it;
        if (nodeType(snapshot, box.containerId) == data::ContainerType::Image && box.rect.contains(paperPos)) {
            return {ContainerHitType::FloatingImage, box.containerId, box.parentId, -1};
        }
    }
    for (auto it = layout.images.rbegin(); it != layout.images.rend(); ++it) {
        const auto& img = *it;
        if (img.rect.contains(paperPos)) {
            return {ContainerHitType::FloatingImage, img.containerId, {}, -1};
        }
    }

    // 4. 检测容器内容区
    // 先找最深层的子容器，再看顶层容器
    for (auto it = layout.containers.rbegin(); it != layout.containers.rend(); ++it) {
        const auto& box = *it;
        if (!box.rect.contains(paperPos))
            continue;

        const auto type = nodeType(snapshot, box.containerId);
        if (type == data::ContainerType::Text) {
            // 文字容器，调用底层正文精确命中
            const Hit textHit = hitTest(layout, paperPos);
            if (textHit.valid() && textHit.containerId == box.containerId) {
                return {ContainerHitType::TextFragment, box.containerId, box.parentId, textHit.offset};
            }
            return {ContainerHitType::TextFragment, box.containerId, box.parentId, 0};
        }

        if (type == data::ContainerType::Group) {
            // 检查是否落入其任何子容器内
            bool inChild = false;
            for (const auto& other : layout.containers) {
                if (other.parentId == box.containerId && other.rect.contains(paperPos)) {
                    inChild = true;
                    break;
                }
            }
            if (!inChild) {
                return {ContainerHitType::GroupBlankArea, box.containerId, box.parentId, -1};
            }
        }
    }

    // 5. 检测页面空白区
    if (layout.pageRect.contains(paperPos)) {
        return {ContainerHitType::PageBlankArea, snapshot.root.id, {}, -1};
    }

    return {ContainerHitType::None, {}, {}, -1};
}

} // namespace editor::interaction
