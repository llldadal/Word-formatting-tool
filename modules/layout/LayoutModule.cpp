#include "LayoutModule.h"

namespace editor::layout {

data::LayoutResult composeLayout(const data::DocumentSnapshot& document)
{
    data::LayoutResult result;

    // 输出结果对应输入文档的同一版本。
    result.documentRevision = document.revision;

    // 纸张左上角为 (0, 0)，尺寸来自文档设置。
    result.pageRect = QRectF(QPointF(0, 0), document.page.size);

    // 读取页边距，计算正文区域。
    const auto& margins = document.page.margins;

    qreal contentWidth = document.page.size.width()
                         - margins.left() - margins.right();

    qreal contentHeight = document.page.size.height()
                          - margins.top() - margins.bottom();

    result.contentRect = QRectF(
        margins.left(),
        margins.top(),
        contentWidth,
        contentHeight
        );

    return result;
}

} // namespace editor::layout
// 排版模块的工作区。
// 读取 DocumentSnapshot，生成同一版本的 LayoutResult。
// 负责单栏盒模型、同级避让、固定子容器障碍、文字流动绕排与弹性高度。
// 使用字符格式和段落设置排字，输出原文范围及可供绘制、光标定位的 Qt 文字布局。
// 超页内容继续排版，layoutBounds 覆盖完整结果。
// 算法、内部类和方法由模块负责人设计。
