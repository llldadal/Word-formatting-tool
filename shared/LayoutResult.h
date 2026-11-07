#pragma once

#include "DocumentSnapshot.h"

#include <QRectF>
#include <QTextLayout>
#include <memory>
#include <vector>

namespace editor::data {

// 某个内容容器的最终盒模型。矩形均相对纸张左上角，单位为文档逻辑像素。
struct ContainerLayout {
    ContainerId containerId; // 对应源容器的唯一 ID。
    ContainerId parentId;    // 父容器 ID，逻辑根的直接子节点在这里使用空 ID。
    QRectF rect;             // 容器框，包含 padding。
    QRectF contentRect;      // rect 扣除 padding 后的内容区。
    QRectF outerRect;        // rect 加上 margin 后的占位区。
};

// 一行中的一个文字片段。同一视觉行可能被障碍切成多个片段。
// sourceRange 相对文本容器，Qt 行内的位置则相对该 QTextLayout 的文本。
// 交互换算偏移时，以 sourceRange.start 对应 QTextLine::textStart()。
struct TextFragment {
    ContainerId containerId; // 所属文本容器 ID。
    TextRange sourceRange;   // 对应原文的 UTF-16 范围。
    QRectF rect;             // 该片段的可用区域，包含本行行高。
    int layoutIndex = -1;    // 在 LayoutResult::textLayouts 中的索引。
    int lineIndex = -1;      // 对应 QTextLayout 中的行片段索引。
};

// 一张图片的绘制信息。
struct ImageLayout {
    ContainerId containerId; // 所属图片容器 ID。
    ResourceId resourceId;   // 通过同版本文档快照的资源表取得原图。
    QRectF sourceRect;       // 从原图取用的区域，单位为原图像素。
    QRectF rect;             // 图片内容区的绘制位置，使用纸张坐标。
};

// 公共数据二。排版模块交给交互、绘图和导出的结果。
// 页面的左上角为 (0, 0)，缩放和滚动由 UI 换算。
// 所有片段和 QTextLine::position() 都使用同一套纸张坐标。
// 完整输出超页内容，画布范围使用 pageRect 与 layoutBounds 的并集。
struct LayoutResult {
    quint64 documentRevision = 0;                 // 对应输入快照的版本号。
    QRectF pageRect;                              // 固定纸张区域。
    QRectF contentRect;                           // 纸张扣除页边距后的正文区。
    QRectF layoutBounds;                          // 全部容器占位区及文字、图片区域的并集。
    std::vector<ContainerLayout> containers;      // 按 ID 回指源容器的最终区域，忽略逻辑根。
    std::vector<TextFragment> textFragments;      // 各片文字与原文范围的对应关系。
    std::vector<ImageLayout> images;              // 图片资源引用及绘制区域。
    std::vector<std::shared_ptr<const QTextLayout>> textLayouts; // 持有可绘制、可定位光标的 Qt 行对象。
};

}
