cmake_minimum_required(VERSION 3.21)

# 既可随正式项目构建，也可单独在 Qt Creator 打开 tests/CMakeLists.txt。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EditorContractTests LANGUAGES CXX)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)
    if(MSVC)
        add_compile_options(/utf-8)
    endif()
    add_subdirectory(../shared "${CMAKE_CURRENT_BINARY_DIR}/shared")
    option(BUILD_TESTING "注册 CTest 测试" ON)
    if(BUILD_TESTING)
        enable_testing()
    endif()
endif()

add_subdirectory(layout)
