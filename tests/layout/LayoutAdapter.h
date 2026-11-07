#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"

namespace editor::testing {

// 测试接入点。由适配文件转调组员的实际排版代码，不限定正式模块内部的类和方法。
// 输入来自 Cases.cpp，输出必须使用 shared/LayoutResult.h。
data::LayoutResult composeForTest(const data::DocumentSnapshot& document);

} // namespace editor::testing
