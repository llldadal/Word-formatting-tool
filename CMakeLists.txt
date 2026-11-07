cmake_minimum_required(VERSION 3.21)
project(QtDocumentEditor VERSION 0.1.0 LANGUAGES CXX)

find_package(Qt6 6.12 REQUIRED COMPONENTS Gui Widgets)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 各业务模块只依赖公共数据，组员可以分别增加内部类、函数和源文件。
add_subdirectory(shared)
add_subdirectory(modules/document)
add_subdirectory(modules/layout)
add_subdirectory(modules/interaction)
add_subdirectory(modules/ui_render)
add_subdirectory(modules/io)

# 测试使用同一份公共数据，可关闭；各模块内部实现仍由负责人自行设计。
option(BUILD_TESTING "注册 CTest 测试" ON)
if(BUILD_TESTING)
    enable_testing()
endif()
option(EDITOR_BUILD_CONTRACT_TESTS "构建文档到排版的契约测试和绘图标准" ON)
if(EDITOR_BUILD_CONTRACT_TESTS)
    add_subdirectory(tests)
endif()
