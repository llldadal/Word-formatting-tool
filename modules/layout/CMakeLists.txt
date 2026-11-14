# 排版库：保存我们实现的排版功能。
add_library(EditorLayout STATIC
    LayoutModule.cpp
    LayoutModule.h
)

target_link_libraries(EditorLayout PUBLIC EditorData)

target_include_directories(EditorLayout
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
