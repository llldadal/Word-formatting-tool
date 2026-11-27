#pragma once

#include "ContainerInteractionTypes.h"
#include "DocumentSnapshot.h"
#include "LayoutResult.h"
#include "SnapEngine.h"
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <optional>
#include <vector>

namespace editor::interaction {

// 拖拽落点与目标重排方案
struct DragDropPlan {
    data::ContainerId targetParentId;
    std::size_t targetIndex = 0;
    QPointF localPosition;                       // 相对目标父容器内容区的坐标 (用于 geometry.position)
    data::PlacementMode placementMode = data::PlacementMode::Flow;
    QRectF targetRect;                           // 实际吸附后的矩形 (纸张坐标，用于绘制跟随虚影)
    QRectF targetParentContentRect;              // 目标父容器内容区矩形 (纸张坐标)
    QRectF targetParentHighlightRect;            // 拖入 Group 时的高亮目标框 (纸张坐标)
    std::optional<QLineF> insertionIndicator;    // 顺序容器目标插槽参考线 (纸张坐标)
    std::optional<QSizeF> targetDisplaySize;     // 图片缩放时的目标显示尺寸 (用于 image.displaySize)
    std::vector<Guideline> guidelines;           // 磁性吸附对齐线
    bool isValid = true;                         // 落点是否合法
};

class ContainerDragEngine {
public:
    // 计算容器（文本/分组）拖拽的目标父级、位序、坐标及插槽提示线
    // baseLayout: 拖拽开始时的稳定排版结果（用于计算同级兄弟的稳定坐标，防止下移追不上与死区卡顿）
    static DragDropPlan planContainerMove(
        const data::ContainerId& draggedId,
        data::ContainerType draggedType,
        const QRectF& initialRect,
        const QPointF& delta,
        const QPointF& paperPos,
        const data::LayoutResult& baseLayout,
        const data::DocumentSnapshot& snapshot,
        qreal zoom = 1.0);

    // 计算图片拖拽（支持文本内浮动绕排跨容器迁移，也支持顶层/分组流式重排）
    static DragDropPlan planImageMove(
        const data::ContainerId& draggedImageId,
        const data::ContainerId& initialParentId,
        const QRectF& initialRect,
        const QPointF& delta,
        const QPointF& paperPos,
        const data::LayoutResult& baseLayout,
        const data::DocumentSnapshot& snapshot,
        qreal zoom = 1.0);

    // 计算调整容器手柄宽度时的几何方案与吸附线（修复左手柄拉伸未更新 x 坐标问题）
    static DragDropPlan planResizeWidth(
        const data::ContainerId& draggedId,
        bool isLeftHandle,
        const QRectF& initialRect,
        const QPointF& delta,
        const QRectF& parentContentRect,
        const data::LayoutResult& baseLayout,
        qreal zoom = 1.0);

    // 计算调整图片大小（8 个角/边手柄）的目标显示尺寸、位置与吸附线
    static DragDropPlan planImageResize(
        const data::ContainerId& draggedImageId,
        ContainerHitType handle,
        const QRectF& initialFrameRect,
        const QSizeF& initialDisplaySize,
        const data::ContainerGeometry& initialGeometry,
        const QPointF& delta,
        const QRectF& parentContentRect,
        const data::LayoutResult& baseLayout,
        bool keepAspectRatio = true,
        qreal zoom = 1.0);

    // 基于计划方案，在候选快照上应用节点移动、几何与图片尺寸变更（用于纯内存临时排版 composeLayout）
    static bool applyPlanToCandidateSnapshot(
        data::DocumentSnapshot& candidate,
        const data::ContainerId& draggedId,
        const DragDropPlan& plan,
        const data::ContainerGeometry& geometry);
};

} // namespace editor::interaction
