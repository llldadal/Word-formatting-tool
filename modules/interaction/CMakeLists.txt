# 交互模块。事件处理依赖的类和方法由模块负责人设计。
add_library(EditorInteraction STATIC
    InteractionModule.cpp
    InteractionTypes.h
    HitTest.h
    HitTest.cpp
    TextBoundary.h
    TextBoundary.cpp
    Selection.h
    Selection.cpp
    InteractionController.h
    InteractionController.cpp
)
target_link_libraries(EditorInteraction PUBLIC EditorData)
# 让自测程序和以后链接本模块的代码能直接 #include "HitTest.h"。
target_include_directories(EditorInteraction PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# 自测只依赖仓库自带的假数据（EditorTestFixtures），不需要文档/排版/界面组。
# 目标在根 CMakeLists 末尾的 tests/ 里定义，这里用开关守卫：
# 组长用 -DEDITOR_BUILD_CONTRACT_TESTS=OFF 构建时不会去链接不存在的库。
option(EDITOR_BUILD_INTERACTION_SELFTEST "构建交互模块自测程序" ON)
if(EDITOR_BUILD_INTERACTION_SELFTEST AND EDITOR_BUILD_CONTRACT_TESTS)
    add_executable(InteractionSelfCheck tests/InteractionSelfCheck.cpp)
    target_link_libraries(InteractionSelfCheck PRIVATE EditorInteraction EditorTestFixtures)
endif()
