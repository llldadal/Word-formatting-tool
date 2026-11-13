# 文档模块。新增本模块的头文件和源文件时，显式写入此列表。
add_library(EditorDocument STATIC
    DocumentModule.cpp
    DocumentModule.h)
target_include_directories(EditorDocument PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EditorDocument PUBLIC EditorData)
