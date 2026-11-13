#pragma once

#include "InteractionTypes.h"
#include "LayoutResult.h"

#include <QPointF>

namespace editor::interaction {

// 命中测试结果。offset 是相对 containerId 正文的 UTF-16 偏移。
struct Hit {
    data::ContainerId containerId;
    int               offset = -1;

    bool valid() const { return offset >= 0; }
};

// 把纸张坐标换成「哪个文本容器、第几个字」。
// 落在文字片段内时取该片段；落在空白或图片区域时夹到垂直距离最近的一行。
// layout 只读，排版一变调用方就用新的 LayoutResult 重新计算，不缓存矩形。
Hit hitTest(const data::LayoutResult& layout, QPointF paper);

} // namespace editor::interaction
