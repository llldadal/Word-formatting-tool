# 导入导出模块。v0.1 的交付内容为可编辑 DOCX 导出。
add_library(EditorIO STATIC
    IoModule.cpp
    ZipWriter.h
    ZipWriter.cpp
    DocxExportTypes.h
    DocxExporter.h
    DocxExporter.cpp
)
target_link_libraries(EditorIO PUBLIC EditorData)
# 让自测程序和以后链接本模块的代码能直接 #include "DocxExporter.h"。
target_include_directories(EditorIO PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# 自测只依赖仓库自带的假数据（EditorTestFixtures），不需要文档/排版/界面组。
# 目标在根 CMakeLists 末尾的 tests/ 里定义，这里用开关守卫：
# 组长用 -DEDITOR_BUILD_CONTRACT_TESTS=OFF 构建时不会去链接不存在的库。
option(EDITOR_BUILD_IO_SELFTEST "构建导入导出模块自测程序" ON)
if(EDITOR_BUILD_IO_SELFTEST AND EDITOR_BUILD_CONTRACT_TESTS)
    add_executable(IoSelfCheck tests/IoSelfCheck.cpp)
    target_link_libraries(IoSelfCheck PRIVATE EditorIO EditorTestFixtures)
endif()

# 编辑 -> 导出的纵切自测。用本地替身顶掉文档/排版模块，交互 + 导出两条链独立联调。
# 正式集成后删除这一组目标即可，不影响 EditorIO 本身。
option(EDITOR_BUILD_VERTICAL_SLICE_SELFTEST "构建编辑到导出的纵切自测" ON)
if(EDITOR_BUILD_VERTICAL_SLICE_SELFTEST AND EDITOR_BUILD_CONTRACT_TESTS)
    add_executable(VerticalSliceSelfCheck
        tests/slice/VerticalSliceSelfCheck.cpp
        tests/slice/LocalDocumentExecutor.h
        tests/slice/LocalDocumentExecutor.cpp
        tests/slice/FlowGeometry.h
        tests/slice/FlowGeometry.cpp
    )
    target_include_directories(VerticalSliceSelfCheck PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/slice")
    target_link_libraries(VerticalSliceSelfCheck PRIVATE
        EditorIO EditorInteraction EditorTestFixtures)
endif()

# 第六刀：真实交互窗口 + 无窗口事件自测。
# EditorDemo 是可以手动点击、输入、导出的完整窗口；UiSelfCheck 在离屏平台下
# 投递真实 Qt 事件做同样的验证。两个目标共用 demo/ 与本地替身源码，
# 正式集成后随替身一起删除即可，不影响 EditorIO 本身。
option(EDITOR_BUILD_UI_DEMO "构建真实交互窗口与事件自测" ON)
if(EDITOR_BUILD_UI_DEMO AND EDITOR_BUILD_CONTRACT_TESTS)
    set(DEMO_SHARED_SOURCES
        demo/DocumentCanvas.h
        demo/DocumentCanvas.cpp
        demo/EditorWindow.h
        demo/EditorWindow.cpp
        tests/slice/LocalDocumentExecutor.h
        tests/slice/LocalDocumentExecutor.cpp
        tests/slice/FlowGeometry.h
        tests/slice/FlowGeometry.cpp
    )
    set(DEMO_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demo"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/slice"
    )

    add_executable(EditorDemo
        demo/main.cpp
        ${DEMO_SHARED_SOURCES}
    )
    target_include_directories(EditorDemo PRIVATE ${DEMO_INCLUDES})
    target_link_libraries(EditorDemo PRIVATE
        EditorIO EditorInteraction EditorTestFixtures Qt6::Widgets)

    add_executable(UiSelfCheck
        tests/UiSelfCheck.cpp
        ${DEMO_SHARED_SOURCES}
    )
    target_include_directories(UiSelfCheck PRIVATE ${DEMO_INCLUDES})
    target_link_libraries(UiSelfCheck PRIVATE
        EditorIO EditorInteraction EditorTestFixtures Qt6::Widgets)
endif()
