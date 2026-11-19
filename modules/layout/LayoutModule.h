#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"

namespace editor::layout {

// 计算段落的默认字符样式，局部文字的格式仍可覆盖它。
data::TextStyle resolveParagraphStyle(data::TextStyle base, const data::ParagraphInfo& paragraph);

data::LayoutResult composeLayout(
    const data::DocumentSnapshot& document
    );

}
