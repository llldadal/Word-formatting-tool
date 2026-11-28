#pragma once

#include "ContainerInteractionTypes.h"
#include "DocumentSnapshot.h"

#include <QMenu>
#include <QWidget>
#include <functional>
#include <memory>

namespace editor::ui {

struct ContextMenuCallbacks {
    std::function<void(const data::ContainerId& parentId)> onAddText;
    std::function<void(const data::ContainerId& parentId)> onAddGroup;
    std::function<void()> onDeleteSelected;
    std::function<void()> onCopy;
    std::function<void()> onCut;
    std::function<void()> onPaste;
    std::function<void()> onUndo;
    std::function<void()> onRedo;
};

class ContextMenuBuilder {
public:
    static std::unique_ptr<QMenu> buildMenu(
        const interaction::ContainerHitResult& hit,
        const interaction::ContainerSelectionState& selection,
        const ContextMenuCallbacks& callbacks,
        bool canUndo = false,
        bool canRedo = false,
        QWidget* parent = nullptr);
};

} // namespace editor::ui
