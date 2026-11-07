# 交互模块。事件处理依赖的类和方法由模块负责人设计。
add_library(EditorInteraction STATIC InteractionModule.cpp)
target_link_libraries(EditorInteraction PUBLIC EditorData)
