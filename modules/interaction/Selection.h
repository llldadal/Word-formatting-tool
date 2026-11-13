#pragma once

#include "InteractionTypes.h"
#include "HitTest.h"
#include "LayoutResult.h"
#include "DocumentSnapshot.h"

#include <QRectF>
#include <vector>

namespace editor::interaction {

// 把任意两个偏移归一化成 [start, end)，反向拖选也得到升序范围。
data::TextRange normalizeRange(int a, int b);
data::TextRange normalizeRange(data::TextRange range);

// 该容器在排版结果里的合法正文范围：所有片段的最小起点与最大终点。
data::TextRange containerRange(const data::LayoutResult& layout,
                               const data::ContainerId& id);

// I04：拖到别的容器或页边空白时，焦点锁死在 anchor 的容器内，
// 并把偏移夹到最近合法位置。跨容器选区 v0.1 不实现。
Hit clampFocus(const data::LayoutResult& layout, const Hit& anchor, QPointF paperFocus);

// I05：Ctrl+A 只选当前容器全部正文，不跨容器。
data::TextRange selectAllRange(const data::DocumentSnapshot& doc,
                               const data::ContainerId& id);

// 逐片段求交生成高亮矩形。图片占据的区域没有文本片段，天然被跳过。
std::vector<QRectF> selectionRects(const data::LayoutResult& layout,
                                   const data::ContainerId& id,
                                   data::TextRange range);

SelectionState makeSelection(const data::LayoutResult& layout,
                             const Hit& anchor, const Hit& focus);

SelectionState makeSelection(const data::LayoutResult& layout,
                             const data::ContainerId& id,
                             data::TextRange range);

// 光标矩形。返回宽度 1 逻辑像素的竖条，UI 可直接绘制或换算成窗口坐标。
QRectF caretRect(const data::LayoutResult& layout,
                 const data::ContainerId& id, int offset);

} // namespace editor::interaction
