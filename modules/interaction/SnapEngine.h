#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <vector>

namespace editor::interaction {

enum class GuidelineOrientation {
    Horizontal,
    Vertical
};

struct Guideline {
    GuidelineOrientation orientation = GuidelineOrientation::Vertical;
    qreal position = 0;   // 纸张坐标：垂直线为 x，水平线为 y
    qreal start = 0;      // 另一轴向的起点
    qreal end = 0;        // 另一轴向的终点
    QColor color = QColor(255, 0, 127); // #FF007F 洋红色
};

struct SnapResult {
    QPointF snappedTopLeft;
    qreal snappedWidth = 0;
    qreal snappedHeight = 0;
    bool snappedX = false;
    bool snappedY = false;
    std::vector<Guideline> guidelines;
};

class SnapEngine {
public:
    // 计算移动容器或浮动图片时的吸附与辅助线
    static SnapResult snapMove(
        const QRectF& currentRect,
        const data::LayoutResult& layout,
        const data::ContainerId& draggedId,
        qreal threshold = 6.0,
        bool snapY = true);

    // 计算拉伸左右手柄时的吸附与辅助线
    static SnapResult snapResizeWidth(
        const QRectF& currentRect,
        bool isLeftHandle,
        const data::LayoutResult& layout,
        const data::ContainerId& draggedId,
        qreal threshold = 6.0);

    // 计算拉伸上下手柄时的吸附与辅助线
    static SnapResult snapResizeHeight(
        const QRectF& currentRect,
        bool isTopHandle,
        const data::LayoutResult& layout,
        const data::ContainerId& draggedId,
        qreal threshold = 6.0);
};

} // namespace editor::interaction
