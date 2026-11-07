# 导入导出模块。v0.1 的交付内容为可编辑 DOCX 导出。
add_library(EditorIO STATIC IoModule.cpp)
target_link_libraries(EditorIO PUBLIC EditorData)
