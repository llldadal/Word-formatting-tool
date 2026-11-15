cmake_minimum_required(VERSION 3.21)
project(QtDocumentEditorApp VERSION 0.1.0 LANGUAGES CXX)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(BUILD_TESTING "注册 CTest 测试" ON)
option(EDITOR_BUILD_CONTRACT_TESTS "构建原项目的契约测试和自测" ON)
if(BUILD_TESTING)
    enable_testing()
endif()
if(EDITOR_BUILD_CONTRACT_TESTS)
    set(EDITOR_LAYOUT_TEST_ADAPTER "${CMAKE_CURRENT_SOURCE_DIR}/tests/LayoutAdapter.cpp"
        CACHE FILEPATH "排版测试适配 cpp")
endif()

set(editor_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_subdirectory("${editor_root}" core)

add_library(EditorApplication STATIC
    EditorSession.h
    EditorSession.cpp
    EditorInput.h
    EditorInput.cpp
    EditorIntegration.h
    EditorIntegration.cpp
)
set_target_properties(EditorApplication PROPERTIES AUTOMOC ON)
target_include_directories(EditorApplication PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EditorApplication PUBLIC
    EditorDocument EditorLayout EditorInteraction EditorUiRender EditorIO Qt6::Widgets
)

add_executable(QtDocumentEditor WIN32 main.cpp)
target_link_libraries(QtDocumentEditor PRIVATE EditorApplication)

if(BUILD_TESTING AND TARGET EditorTestFixtures)
    add_executable(EditorIntegrationTest tests/EditorIntegrationTest.cpp)
    target_link_libraries(EditorIntegrationTest PRIVATE EditorApplication EditorTestFixtures)
    add_test(NAME EditorIntegrationTest COMMAND EditorIntegrationTest)
    set_tests_properties(EditorIntegrationTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30
    )
endif()
