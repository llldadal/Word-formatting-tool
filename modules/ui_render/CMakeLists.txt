add_library(EditorUiRender STATIC
    UiRenderModule.cpp
    CanvasWidget.cpp
    CanvasWidget.h
    MainWindow.cpp
    MainWindow.h
    MainWindow.ui
)

set_target_properties(EditorUiRender PROPERTIES AUTOUIC ON AUTOMOC ON)

target_link_libraries(EditorUiRender
    PUBLIC
        EditorData
        EditorLayout
        Qt6::Widgets
)

target_include_directories(EditorUiRender PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")