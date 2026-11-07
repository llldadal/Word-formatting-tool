# 排版模块。新增本模块的头文件和源文件时，显式写入此列表。
add_library(EditorLayout STATIC LayoutModule.cpp)
target_link_libraries(EditorLayout PUBLIC EditorData)
