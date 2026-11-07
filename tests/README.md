# v0.1 测试说明

这批测试直接使用 shared 中的 DocumentSnapshot 和 LayoutResult。输入由 C++ 代码建立，测试程序调用实际排版代码，再与固定标准比对。绘图组使用同一份标准结果和 PNG 参考图，人工判断绘制是否正确。

当前排版模块仍是占位文件。默认运行 EditorLayoutTests 会输出 NOT RUN，返回 2，表示尚未接入。EditorFixtureSelfCheck 只检查测试数据和比较器，不能据此认定排版算法已经通过。

## 文件怎么分

| 文件 | 用途 |
|---|---|
| layout/Cases.cpp | 固定容器树、正文、格式、图片和页面设置 |
| layout/TestFixtures.h | 测试数据及标准结果的读取入口 |
| layout/LayoutAdapter.h | 测试程序调用实际排版代码的接入点 |
| layout/PendingAdapter.cpp | 尚未接入时的占位文件 |
| layout/TestMain.cpp | 选题、调用排版、比对并报告结果 |
| layout/Compare.cpp | 比较版本、矩形、字符范围、字体格式和 Qt 行对象 |
| layout/SnapshotValidation.cpp | 检查测试输入是否合法 |
| layout/LayoutValidation.cpp | 独立检查盒模型、全文覆盖、避让和 layoutBounds |
| layout/StandardRecords.inc | 已固定的标准坐标和字符范围 |
| layout/StandardLayouts.cpp | 按固定记录恢复可绘制、可定位的 QTextLayout |
| layout/FixtureSupport.cpp | 恢复标准所需的字体和格式辅助代码 |
| layout/SelfCheck.cpp | 检查标准，并故意制造错误验证比较器能否发现 |
| layout/standards | 每道题的标准位置和字符范围，供人阅读 |
| layout/reference | 每道题对应的标准绘图 |
| layout/assets | 两张原始图片资源 |
| 各模块验收.md | 尚未固定调用接口的模块，按操作和预期结果验收 |

没有另建一份旧版 contract.h，也没有提供绘图测试程序。正式模块的类名和内部方法仍由负责人设计。

## 题目范围

| 题号 | 内容 | 检查重点 |
|---|---|---|
| 01 | 标题和两段正文 | 单栏顺序、标题语义、容器间距 |
| 02 | 单图嵌入正文 | 上下全宽、中间先左后右接续正文 |
| 03 | 双图嵌入正文 | 同一行依次经过左、中、右区域 |
| 04 | 图片独立排在正文之后 | 独立图片容器和顺序排版 |
| 06 | 右下角嵌套小段落 | 两份独立正文、固定障碍、父容器弹性高度 |
| 07 | 多段落和局部格式 | 标题级别、缩进、段距、右对齐、继承及显式取消斜体 |
| 08 | 超出单页 | 页面保持一张，纸张外正文和图片完整输出 |
| 09 | 分组和盒模型 | 内外边距、外边距相加、图片尺寸来源、最小高度 |
| 10 | Unicode 和空段落 | UTF-16、代理对、组合字符、中间及末尾空段落 |
| 11 | 空文档 | 保留页面和内容区，没有容器、片段或图片，layoutBounds 为空 |

原第 05 题双栏暂不启用。自动续页、手动分页、多栏、两层以上嵌套、表格和公式不进入这批自动测试。第 06 题的子文本块使用明确的相对位置，不要求实现底部锚定。

## 固定标准采用什么规则

以下规则把现有约定具体到测试数据。修改规则时，需要同时审核输入、固定记录和参考图，不能只改标准让实际输出通过。

- 页面原点为 (0, 0)，不是旧测试的 (20, 20)。参考图额外留了 20 个单位的显示边距，绘图时可以自行决定。
- 几何量每英寸 96 个单位，字号为 pt。正文 12 pt、行高 26；基础题标题 19.5 pt、行高 40。第 07 题单独设置章节标题格式。
- 使用 Microsoft YaHei、Qt 6.12.0 的排字结果。换行采用 WrapAtWordBoundaryOrAnywhere，useDesignMetrics 为 true。除题目显式设置外，左对齐、不附加 QTextDocument 默认边距。
- 逻辑根不输出容器区域，也不计嵌套深度。它的直接子节点在 ContainerLayout 中 parentId 为空；其他节点保留实际父 ID。
- Flow 节点按 children 顺序纵向排。框左边不早于父内容区左边加左外边距；框顶边不早于期望 y，也不早于前一顺序节点占位区底边加当前上外边距。位置字段表示框的期望位置，顺序约束可以将其下推。
- 宽度为 0 的文本或分组填满父内容区扣除左右外边距后的宽度。显式宽度包含 padding。图片框由 displaySize 加 padding 得到，忽略图片节点的 geometry.width。
- Floating 子节点的框位置严格取父内容区原点加 position。它的 outerRect 作为正文障碍。图片或文本子节点都按同样的矩形规则避让；这些输入中的固定子节点彼此不重叠。
- 用完整行高与障碍判断相交。可用区域从左到右使用，正文字符连续接续；整行被挡住时保留行高继续向下尝试。图片下方恢复全宽。这批题没有接近零宽的缝隙，不额外规定最小缝隙阈值。
- 段落范围排除换行符，换行符不生成文字片段。空段落生成一个空 sourceRange 的行片段，仍占一行高度并保留插入位置。段前、段后间距均计入文本高度。
- 父容器高度覆盖正文末端、子容器占位区底边及底部 padding，并满足 minimumHeight。矩形使用几何坐标，不按整数像素的“减一”规则计算底边。
- 每个 TextFragment.rect 是可用区域，包含固定行高。QTextLine.position() 使用纸张坐标。sourceRange 相对所属容器；Qt 行内下标可以相对段落或其他子布局，两者按各自起点换算。
- layoutBounds 是全部容器 outerRect、文字区域和图片区域的并集，不强制包含整张纸。第 08 题不分页，也不在纸边截断。

标准坐标和范围保存在 StandardRecords.inc。运行测试时只按这些数值恢复 Qt 行对象，不根据实际排版输出生成答案。恢复时若 Qt 断行与数值不符，会报告环境不兼容，不能当成算法通过或失败。

## 排版组怎么接

新建一个适配 cpp，实现下面的测试入口即可。这个函数仅用于测试，不要求正式排版类继承某个基类。

```cpp
#include "LayoutAdapter.h"
#include "你的排版入口头文件.h"

namespace editor::testing {
data::LayoutResult composeForTest(const data::DocumentSnapshot& document)
{
    // 在这里调用实际排版模块，返回它计算出的结果。
    return /* 你的排版调用 */;
}
}
```

上面是接入位置示意，不是可直接编译的实现。适配文件不能调用 makeStandardLayout，也不能读取 StandardRecords.inc。测试程序只把文档快照传给它。

在 Qt Creator 的 CMake 配置中设置 EDITOR_LAYOUT_TEST_ADAPTER 为该 cpp 的绝对路径，或通过命令行指定。完整项目已经链接 EditorLayout；新增头文件、源文件和内部依赖写入排版模块自己的 CMake。额外库可以填入 EDITOR_LAYOUT_TEST_LIBRARIES。

```powershell
cmake -S ./正式项目 -B ./build/v01-tests -G Ninja `
  -DCMAKE_PREFIX_PATH="D:/Qt/6.12.0/mingw_64" `
  -DEDITOR_LAYOUT_TEST_ADAPTER="E:/your-project/MyLayoutAdapter.cpp"
cmake --build ./build/v01-tests
./build/v01-tests/tests/layout/EditorLayoutTests.exe --all
./build/v01-tests/tests/layout/EditorLayoutTests.exe --case 03
```

命令在工作区根目录运行。编译器、Ninja 和 Qt DLL 需要在环境中可用；Qt Creator 选好 Kit 后会负责这些设置。已有旧接口缓存时，使用新的构建目录。

```powershell
./build/v01-tests/tests/layout/EditorLayoutTests.exe --list
ctest --test-dir ./build/v01-tests --output-on-failure
```

默认适配文件未接入时，CTest 只注册 EditorFixtureSelfCheck。指定自定义适配文件后，另注册 EditorLayoutAcceptance，调用实际排版测试。Windows 下测试使用 windows 平台插件，但不创建窗口；不要将 QT_QPA_PLATFORM 强制设为 offscreen，那个插件在这里没有加载系统字体。

如果只需要固定输入和绘图标准，也可以在 Qt Creator 单独打开 tests/CMakeLists.txt，或者执行 `cmake -S ./正式项目/tests -B ./build/v01-fixtures`。它会自行加载 shared；需要实际排版时应同时接入对应模块及其依赖。

## 程序如何判定

ID、父子关系、资源 ID、版本号、片段数量及字符范围必须一致。矩形和光标横坐标允许 0.5 个逻辑单位误差。比较器会检查漏字、重字、遗漏或重复的片段、障碍重叠、内外边距、超页范围，以及 Qt 行索引是否有效。

不要求 vector 顺序、layoutIndex 数值或 QTextLayout 的拆分方式相同。只要每个片段对应的原文、位置、格式和字符边界一致即可。颜色、字号、粗体、斜体和下划线按最终有效格式检查，而不是要求格式区间的内部拆法相同。

| 退出码 | 含义 |
|---|---|
| 0 | 选中的实际排版测试全部通过 |
| 1 | 实际结果与标准不一致 |
| 2 | 参数错误、尚未接入、字体环境不兼容或调用无法完成 |

EditorFixtureSelfCheck 是另一个程序，它返回 0 只表示测试数据及比较器自身通过检查。它不会调用实际排版模块。

## 绘图组怎么用

链接 EditorTestFixtures，使用标准结果和对应快照中的资源即可。

```cpp
#include "TestFixtures.h"

const auto fixture = editor::testing::makeLayoutCase("03");
const auto layout = editor::testing::makeStandardLayout("03");
// 把 layout 和 fixture.document.resources 交给自己的绘图代码。
```

文字片段用 layoutIndex 找到 Qt 布局，再用 lineIndex 取行。行位置已经是纸张坐标；不要再加一次父容器偏移。图片使用 rect、sourceRect 和 resourceId。需要缩放或滚动时，在画布坐标变换中处理。

对照 layout/reference 中的 PNG 检查文字顺序、绕排、嵌套、格式及溢出内容。背景、阴影和页面显示尺寸可以不同，不比较截图像素。standards 下的表格提供准确的区域和字符范围。
