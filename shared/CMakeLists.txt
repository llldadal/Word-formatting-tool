# 公共数据单独建立目标，头文件显式列出，供 Qt Creator 项目树显示。
add_library(EditorData STATIC
    DocumentSnapshot.h
    LayoutResult.h
    DataContracts.cpp
)
target_compile_features(EditorData PUBLIC cxx_std_17)
target_include_directories(EditorData PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EditorData PUBLIC Qt6::Gui)
