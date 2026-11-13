#pragma once

// 纵切自测用的本地替身：按 v0.1 的单页单栏顺序流、盒模型和浮动子节点位置，
// 从任意（包括刚编辑过的）DocumentSnapshot 投影出一份完整 LayoutResult。
// 同时提供交互层需要的 QTextLayout/QTextLine 和导出层需要的容器/图片几何。
// 正式集成时由排版模块替换。

#include "LayoutResult.h"
#include "DocumentSnapshot.h"

namespace editor::slice {

data::LayoutResult projectLayout(const data::DocumentSnapshot& doc);

} // 命名空间 editor::slice 结束
