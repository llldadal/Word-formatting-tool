#pragma once

#include "DocumentSnapshot.h"
#include "InteractionTypes.h"
#include "LayoutResult.h"

#include <optional>
#include <vector>

namespace editor::interaction {

// 按视觉流排序文档中所有文本容器（按 top 升序，top 相近时按 left 升序）
std::vector<data::ContainerId> buildVisualTextOrder(const data::DocumentSnapshot& doc,
                                                    const data::LayoutResult& layout);

// 尝试向左跨容器移动光标（当光标在当前容器最开头 offset == 0 时，跨入前驱容器末尾）
std::optional<InteractionState> tryCrossLeft(const InteractionState& state,
                                             const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout);

// 尝试向右跨容器移动光标（当光标在当前容器末尾 offset >= len 时，跨入后继容器开头）
std::optional<InteractionState> tryCrossRight(const InteractionState& state,
                                              const data::DocumentSnapshot& doc,
                                              const data::LayoutResult& layout);

// 尝试垂直跨容器移动光标（当光标在首行按向上，或在末行按向下时，跨入垂直物理相邻容器并保持 desiredCaretX）
std::optional<InteractionState> tryCrossVertical(const InteractionState& state,
                                                 const data::DocumentSnapshot& doc,
                                                 const data::LayoutResult& layout,
                                                 bool down);

} // namespace editor::interaction
