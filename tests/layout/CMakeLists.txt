# 固定输入与标准结果独立成库，绘图组可以直接链接，不依赖排版实现。
add_library(EditorTestFixtures STATIC
    TestFixtures.h
    Cases.cpp
    FixtureSupport.h
    FixtureSupport.cpp
    StandardLayouts.cpp
    StandardRecords.inc
    assets/figure-1.png
    assets/figure-2.png
)
target_link_libraries(EditorTestFixtures PUBLIC EditorData)
target_include_directories(EditorTestFixtures PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(EditorTestFixtures PRIVATE EDITOR_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# 将标准说明与参考图一并列入 Qt Creator 项目树，便于各组直接查看。
target_sources(EditorTestFixtures PRIVATE ../README.md ../各模块验收.md)
foreach(_editor_case IN ITEMS 01 02 03 04 06 07 08 09 10 11)
    target_sources(EditorTestFixtures PRIVATE
        "standards/${_editor_case}.md" "reference/${_editor_case}.png")
endforeach()

add_library(EditorLayoutComparison STATIC Compare.cpp SnapshotValidation.cpp LayoutValidation.cpp)
target_link_libraries(EditorLayoutComparison PUBLIC EditorTestFixtures)

# 仅替换这个适配文件即可转调实际排版模块，正式项目不增加固定的排版类接口。
set(EDITOR_LAYOUT_TEST_ADAPTER "${CMAKE_CURRENT_SOURCE_DIR}/PendingAdapter.cpp" CACHE FILEPATH "排版测试适配 cpp")
get_filename_component(_editor_adapter "${EDITOR_LAYOUT_TEST_ADAPTER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${_editor_adapter}")
    message(FATAL_ERROR "EDITOR_LAYOUT_TEST_ADAPTER does not exist: ${_editor_adapter}")
endif()
file(TO_CMAKE_PATH "${_editor_adapter}" _editor_adapter)
# 间接包含外部适配文件，避免 MinGW 为中文绝对路径生成异常的目标文件路径。
configure_file(AdapterEntry.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ConnectedAdapter.cpp" @ONLY)
add_executable(EditorLayoutTests TestMain.cpp LayoutAdapter.h AdapterEntry.cpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/ConnectedAdapter.cpp")
target_link_libraries(EditorLayoutTests PRIVATE EditorLayoutComparison)
if(TARGET EditorLayout)
    target_link_libraries(EditorLayoutTests PRIVATE EditorLayout)
endif()
set(EDITOR_LAYOUT_TEST_LIBRARIES "" CACHE STRING "独立构建测试时适配文件所需的附加库或目标")
target_link_libraries(EditorLayoutTests PRIVATE ${EDITOR_LAYOUT_TEST_LIBRARIES})

add_executable(EditorFixtureSelfCheck SelfCheck.cpp)
target_link_libraries(EditorFixtureSelfCheck PRIVATE EditorLayoutComparison)
if(BUILD_TESTING)
    # Qt 的 Windows offscreen 插件不加载系统字体，使用 windows 平台但不创建窗口。
    if(WIN32)
        set(_editor_test_platform windows)
    else()
        set(_editor_test_platform offscreen)
    endif()
    add_test(NAME EditorFixtureSelfCheck COMMAND EditorFixtureSelfCheck)
    set_tests_properties(EditorFixtureSelfCheck PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${_editor_test_platform}")
    # 默认占位实现只返回未接入，不把它注册成会产生虚假通过结果的测试。
    if(NOT _editor_adapter STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/PendingAdapter.cpp")
        add_test(NAME EditorLayoutAcceptance COMMAND EditorLayoutTests --all)
        set_tests_properties(EditorLayoutAcceptance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${_editor_test_platform}")
    endif()
endif()
