# Qt 文档编辑器 v0.1

这一版用于分工开发。公共代码定义两份数据契约，各模块的类和方法由负责人设计。CMake 为公共数据和五个业务模块分别建立静态库目标。

## 文件划分

```text
正式项目/
├── CMakeLists.txt
├── shared/
│   ├── DocumentSnapshot.h
│   ├── LayoutResult.h
│   ├── DataContracts.cpp
│   └── CMakeLists.txt
├── modules/
│   ├── document/
│   ├── layout/
│   ├── interaction/
│   ├── ui_render/
│   │   └── MainWindow.ui
│   └── io/
├── tests/
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── 各模块验收.md
│   └── layout/
│       ├── Cases.cpp
│       ├── StandardRecords.inc
│       ├── standards/
│       └── reference/
└── docs/
    └── v0.1-DOCX映射草案.md
```

shared 里的两个头文件包含完整契约及必要的字段类型，例如文本范围、字符格式、段落信息和容器矩形。DataContracts.cpp 包含这两个头文件，使公共数据可以独立编译检查。

## 两份公共数据

| 定义 | 内容 | 使用方 |
|---|---|---|
| DocumentSnapshot | 文档版本、页面设置、资源表和容器树 | 文档模块提供，排版和导出读取 |
| LayoutResult | 页面、实际容器区域、文字片段、图片区域和 Qt 文字布局 | 排版模块提供，交互、绘图和导出读取 |

公共类型位于 editor::data 命名空间，头文件分别使用 DocumentSnapshot.h 和 LayoutResult.h 引入。

几何量采用固定的文档逻辑像素，96 个单位对应一英寸。字号使用 pt。屏幕缩放和滚动由 UI 转换，排版结果以纸张左上角为原点。

文本容器使用一份连续正文。段落用换行分隔，段落范围排除分隔符，文本位置统一采用 UTF-16 的 [start, end)。容器默认格式、段落覆盖和局部区间覆盖依次生效。文档模块在编辑时维护这些范围。

文本与分组的 geometry.width 是包含 padding 的框宽。图片的目标内容尺寸由 image.displaySize 给出，再加 padding 得到图片框尺寸。图片节点的 geometry.width 不参与计算，避免两个宽度来源发生冲突。

## v0.1 约束

- 使用单页、单栏正文。
- 逻辑根下可以有多个顶层容器，顶层容器可以包含多个子容器，子容器的 children 为空。
- 文本容器中的子节点作为固定障碍，正文修改后重新绕排。
- 相邻容器的外边距相加，父容器高度容纳正文、子容器和底部内边距。
- 超页内容继续排版并显示，画布范围由页面区域和 layoutBounds 共同决定。
- 选区限制在同一个文本容器内，容器 ID 加原文偏移用于定位。
- 生成可编辑 DOCX，具体映射及待验证边界见 docs 中的草案。

## 模块职责

| 目录 | CMake 目标 | 交付内容 |
|---|---|---|
| shared | EditorData | 两份公共数据契约 |
| modules/document | EditorDocument | 容器树增删改查、文本及格式修改、段落语义、图片资源、版本快照 |
| modules/layout | EditorLayout | 单页流动排版、盒模型、避让、弹性高度、源文字与片段的对应关系 |
| modules/interaction | EditorInteraction | 光标、容器内选区、键盘和鼠标编辑、中文输入法、修改请求 |
| modules/ui_render | EditorUiRender | .ui 主窗口、文档绘制、缩放滚动、光标选区和输入法显示、事件转交 |
| modules/io | EditorIO | 根据同版本的快照与几何结果导出可编辑 DOCX，并反馈结果 |

每个模块当前的 cpp 文件是带职责注释的编译占位文件。负责人可以增加自己的头文件和实现，并将文件显式写入本模块的 CMakeLists.txt，使 Qt Creator 能在项目树中显示。

五个业务模块分别依赖 EditorData。后续联调时，根据共同商定的模块调用接口增加必要依赖。

## 构建方式

在 Qt Creator 中打开本目录的 CMakeLists.txt，构建方式选择MinGW，运行 CMake 并构建。业务模块目前生成六个静态库；另外默认构建测试数据库、比较器和两个测试程序。主窗口界面文件可以在设计视图中编辑，UI 负责人接入程序入口后再生成应用程序。只构建业务骨架时，设置 EDITOR_BUILD_CONTRACT_TESTS=OFF。

命令行也可以在配置好编译器、CMake、Ninja 和 Qt 环境的终端中运行。

```powershell
cmake -S ./正式项目 -B ./build/editor-v0.1-data -G Ninja -DCMAKE_PREFIX_PATH="D:/Qt/6.12.0/mingw_64"
cmake --build ./build/editor-v0.1-data
```

当前测试位于 tests，共有十道测试题。固定输入、标准 LayoutResult、参考图和接入方法见 tests/README.md。

默认排版模块尚未接入，EditorLayoutTests 返回 NOT RUN 和退出码 2；EditorFixtureSelfCheck 仅检查测试数据及比较器自身，不能代表排版实现通过。
