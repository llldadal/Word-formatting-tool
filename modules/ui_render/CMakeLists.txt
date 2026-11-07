# UI 与绘图模块。窗口在 MainWindow.ui 中编辑，绘图与事件转发由负责人实现。
add_library(EditorUiRender STATIC
    UiRenderModule.cpp
    MainWindow.ui
)
set_target_properties(EditorUiRender PROPERTIES AUTOUIC ON)
target_link_libraries(EditorUiRender PUBLIC EditorData PRIVATE Qt6::Widgets)
