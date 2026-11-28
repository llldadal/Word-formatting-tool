#pragma once

#include "ContainerInteractionTypes.h"
#include "LayoutResult.h"
#include <QPainter>
#include <vector>

namespace editor::ui {

class ContainerOverlay {
public:
    ContainerOverlay() = default;

    // 绘制选中的容器边框与拉伸手柄（非顶层正文/分组绘制左右手柄，图片绘制四角+四边手柄；正文顶层容器仅绘制选中框）
    static void paintSelection(
        QPainter& painter,
        const interaction::ContainerSelectionState& selection,
        const data::LayoutResult& layout,
        qreal zoom,
        const data::DocumentSnapshot* snapshot = nullptr);

    // 悬停在容器边框上时绘制淡虚线提示框
    static void paintHoverBorder(
        QPainter& painter,
        const data::ContainerId& hoveredId,
        const data::LayoutResult& layout,
        qreal zoom);

    // 当文档为空时，在正文区域中心绘制优雅的空白占位提示
    static void paintEmptyPlaceholder(
        QPainter& painter,
        const data::LayoutResult& layout,
        qreal zoom);
};

} // namespace editor::ui
