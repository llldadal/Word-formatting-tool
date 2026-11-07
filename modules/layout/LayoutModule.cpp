#include "DocumentSnapshot.h"
#include "LayoutResult.h"

// 排版模块的工作区。
// 读取 DocumentSnapshot，生成同一版本的 LayoutResult。
// 负责单栏盒模型、同级避让、固定子容器障碍、文字流动绕排与弹性高度。
// 使用字符格式和段落设置排字，输出原文范围及可供绘制、光标定位的 Qt 文字布局。
// 超页内容继续排版，layoutBounds 覆盖完整结果。
// 算法、内部类和方法由模块负责人设计。
