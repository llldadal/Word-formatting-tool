# 排版库：保存我们实现的排版功能。
add_library(EditorLayout STATIC
    LayoutModule.cpp
    LayoutModule.h
    PagedLayout.cpp
    PagedLayout.h
)

target_link_libraries(EditorLayout PUBLIC EditorData)

target_include_directories(EditorLayout
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(BUILD_TESTING AND EDITOR_BUILD_CONTRACT_TESTS)
    add_executable(EditorPagedLayoutTest tests/PagedLayoutTest.cpp)
    target_link_libraries(EditorPagedLayoutTest PRIVATE EditorLayout)
    add_test(NAME EditorPagedLayoutTest COMMAND EditorPagedLayoutTest)
    if(WIN32)
        set_tests_properties(EditorPagedLayoutTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    else()
        set_tests_properties(EditorPagedLayoutTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
