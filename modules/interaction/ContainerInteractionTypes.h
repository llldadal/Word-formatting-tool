#pragma once

#include "DocumentSnapshot.h"
#include <QString>
#include <vector>
#include <algorithm>

namespace editor::interaction {

enum class ContainerHitType {
    None,
    ResizeHandleLeft,       // 选中容器/图片的左宽度手柄
    ResizeHandleRight,      // 选中容器/图片的右宽度手柄
    ResizeHandleTop,        // 选中图片的上高度手柄
    ResizeHandleBottom,     // 选中图片的下高度手柄
    ResizeHandleTopLeft,    // 选中图片的左上角缩放手柄
    ResizeHandleTopRight,   // 选中图片的右上角缩放手柄
    ResizeHandleBottomLeft, // 选中图片的左下角缩放手柄
    ResizeHandleBottomRight,// 选中图片的右下角缩放手柄
    ContainerBorder,        // 容器拖动边框 (按住可拖动)
    FloatingImage,          // 图片主体 (按住可拖动，含浮动/流式图片)
    TextFragment,           // 正文文本内容区 (点击落光标/拖选)
    GroupBlankArea,         // 顶层分组内部空白区 (右键新增子文本)
    PageBlankArea           // 页面空白区 (右键新增顶层容器)
};

struct ContainerHitResult {
    ContainerHitType type = ContainerHitType::None;
    data::ContainerId containerId;
    data::ContainerId parentId;
    int textOffset = -1;

    bool isValid() const { return type != ContainerHitType::None; }
    bool isHandle() const {
        switch (type) {
        case ContainerHitType::ResizeHandleLeft:
        case ContainerHitType::ResizeHandleRight:
        case ContainerHitType::ResizeHandleTop:
        case ContainerHitType::ResizeHandleBottom:
        case ContainerHitType::ResizeHandleTopLeft:
        case ContainerHitType::ResizeHandleTopRight:
        case ContainerHitType::ResizeHandleBottomLeft:
        case ContainerHitType::ResizeHandleBottomRight:
            return true;
        default:
            return false;
        }
    }
};

struct ContainerSelectionState {
    std::vector<data::ContainerId> selectedIds;

    bool isSelected(const data::ContainerId& id) const {
        return std::find(selectedIds.begin(), selectedIds.end(), id) != selectedIds.end();
    }

    void select(const data::ContainerId& id, bool multi = false) {
        if (!multi) {
            selectedIds.clear();
            if (!id.isEmpty())
                selectedIds.push_back(id);
            return;
        }

        auto it = std::find(selectedIds.begin(), selectedIds.end(), id);
        if (it != selectedIds.end()) {
            selectedIds.erase(it);
        } else if (!id.isEmpty()) {
            selectedIds.push_back(id);
        }
    }

    void clear() {
        selectedIds.clear();
    }

    bool empty() const {
        return selectedIds.empty();
    }

    size_t count() const {
        return selectedIds.size();
    }
};

} // namespace editor::interaction
