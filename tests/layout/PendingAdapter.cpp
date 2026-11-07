#include "LayoutAdapter.h"
#include <stdexcept>

namespace editor::testing {

data::LayoutResult composeForTest(const data::DocumentSnapshot&)
{
    // 这里只标记未接入，不能返回标准答案冒充排版实现。
    // 用 EDITOR_LAYOUT_TEST_ADAPTER 指向自己的 cpp 文件，并实现同名函数。
    throw std::runtime_error("Layout implementation is not connected; set EDITOR_LAYOUT_TEST_ADAPTER.");
}

} // namespace editor::testing
