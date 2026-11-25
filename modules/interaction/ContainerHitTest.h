#pragma once

#include "ContainerInteractionTypes.h"
#include "DocumentSnapshot.h"
#include "LayoutResult.h"
#include <QPointF>

namespace editor::interaction {

// 层次化命中检测：
// 优先级：手柄 > 边框 > 浮动图片 > 正文文字 > 分组空白 > 页面空白
// 解决原 HitTest.cpp 盲目贪婪匹配正文导致空白与边框无法命中的问题
ContainerHitResult hitTestScene(
    const QPointF& paperPos,
    const data::LayoutResult& layout,
    const data::DocumentSnapshot& snapshot,
    const ContainerSelectionState& selectionState,
    qreal zoom);

} // namespace editor::interaction
