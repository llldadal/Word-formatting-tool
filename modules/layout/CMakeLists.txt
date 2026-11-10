# 排版库：保存我们实现的排版功能。
add_library(EditorLayout STATIC
    LayoutModule.cpp
    LayoutModule.h
)

target_link_libraries(EditorLayout PUBLIC EditorData)

target_include_directories(EditorLayout
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

# 示例程序：调用排版库，打印页面计算结果。
add_executable(EditorLayoutExample
    LayoutExample.cpp
)

target_link_libraries(EditorLayoutExample
    PRIVATE EditorLayout
)