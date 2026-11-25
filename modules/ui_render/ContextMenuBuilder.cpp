#include "ContextMenuBuilder.h"

#include <QAction>
#include <QKeySequence>

namespace editor::ui {

std::unique_ptr<QMenu> ContextMenuBuilder::buildMenu(
    const interaction::ContainerHitResult& hit,
    const interaction::ContainerSelectionState& selection,
    const ContextMenuCallbacks& callbacks,
    bool canUndo,
    bool canRedo,
    QWidget* parent)
{
    auto menu = std::make_unique<QMenu>(parent);

    bool addedContainerOp = false;

    // 1. 容器操作项
    if (!selection.empty()) {
        auto* deleteAct = menu->addAction(QStringLiteral("删除容器"));
        deleteAct->setShortcut(QKeySequence::Delete);
        QObject::connect(deleteAct, &QAction::triggered, [callbacks]() {
            if (callbacks.onDeleteSelected)
                callbacks.onDeleteSelected();
        });
        addedContainerOp = true;

        if (hit.type == interaction::ContainerHitType::GroupBlankArea) {
            auto* addText = menu->addAction(QStringLiteral("新增子文本容器"));
            QObject::connect(addText, &QAction::triggered, [callbacks, parentId = hit.containerId]() {
                if (callbacks.onAddText)
                    callbacks.onAddText(parentId);
            });
        }
    } else if (hit.type == interaction::ContainerHitType::GroupBlankArea) {
        auto* addText = menu->addAction(QStringLiteral("新增子文本容器"));
        QObject::connect(addText, &QAction::triggered, [callbacks, parentId = hit.containerId]() {
            if (callbacks.onAddText)
                callbacks.onAddText(parentId);
        });
        addedContainerOp = true;
    } else if (hit.type == interaction::ContainerHitType::PageBlankArea) {
        auto* addTopText = menu->addAction(QStringLiteral("新增顶层文本容器"));
        QObject::connect(addTopText, &QAction::triggered, [callbacks]() {
            if (callbacks.onAddText)
                callbacks.onAddText({});
        });
        auto* addTopGroup = menu->addAction(QStringLiteral("新增顶层分组"));
        QObject::connect(addTopGroup, &QAction::triggered, [callbacks]() {
            if (callbacks.onAddGroup)
                callbacks.onAddGroup({});
        });
        addedContainerOp = true;
    }

    if (addedContainerOp) {
        menu->addSeparator();
    }

    // 2. 撤销 / 重做
    auto* undoAct = menu->addAction(QStringLiteral("撤销"));
    undoAct->setShortcut(QKeySequence::Undo);
    undoAct->setEnabled(canUndo);
    QObject::connect(undoAct, &QAction::triggered, [callbacks]() {
        if (callbacks.onUndo)
            callbacks.onUndo();
    });

    auto* redoAct = menu->addAction(QStringLiteral("重做"));
    redoAct->setShortcut(QKeySequence::Redo);
    redoAct->setEnabled(canRedo);
    QObject::connect(redoAct, &QAction::triggered, [callbacks]() {
        if (callbacks.onRedo)
            callbacks.onRedo();
    });

    menu->addSeparator();

    // 3. 剪贴板操作
    auto* cutAct = menu->addAction(QStringLiteral("剪切"));
    cutAct->setShortcut(QKeySequence::Cut);
    QObject::connect(cutAct, &QAction::triggered, [callbacks]() {
        if (callbacks.onCut)
            callbacks.onCut();
    });

    auto* copyAct = menu->addAction(QStringLiteral("复制"));
    copyAct->setShortcut(QKeySequence::Copy);
    QObject::connect(copyAct, &QAction::triggered, [callbacks]() {
        if (callbacks.onCopy)
            callbacks.onCopy();
    });

    auto* pasteAct = menu->addAction(QStringLiteral("粘贴"));
    pasteAct->setShortcut(QKeySequence::Paste);
    QObject::connect(pasteAct, &QAction::triggered, [callbacks]() {
        if (callbacks.onPaste)
            callbacks.onPaste();
    });

    return menu;
}

} // namespace editor::ui
