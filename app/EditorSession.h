#pragma once

#include "DocumentModule.h"
#include "DocxExportTypes.h"
#include "HitTest.h"
#include "LayoutResult.h"
#include "ContainerInteractionTypes.h"
#include "ContainerDragEngine.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <vector>

namespace editor::app {

class EditorSession : public QObject {
    Q_OBJECT

public:
    explicit EditorSession(QObject* parent = nullptr);

    bool newDocument();
    bool loadSnapshot(const data::DocumentSnapshot& snapshot);
    std::optional<data::ContainerId> insertImage(const QString& path, data::PlacementMode placement);

    const data::DocumentSnapshot& snapshot() const { return liveDragSnapshot_ ? *liveDragSnapshot_ : snapshot_; }
    const data::LayoutResult& layout() const { return liveDragLayout_ ? *liveDragLayout_ : layout_; }
    const data::DocumentSnapshot& baseSnapshot() const { return snapshot_; }
    const data::LayoutResult& baseLayout() const { return layout_; }
    const interaction::InteractionState& state() const { return state_; }
    data::TextStyle currentStyle() const;
    const data::ParagraphInfo* currentParagraph() const;

    // 实时排版与几何提交 (v0.2 阶段三)
    bool isDragging() const { return liveDragLayout_.has_value(); }
    void updateLiveDragGeometry(const data::ContainerId& id, const data::ContainerGeometry& geometry);
    void updateLiveDragWithPlan(const data::ContainerId& id,
                                const interaction::DragDropPlan& plan,
                                const data::ContainerGeometry& geometry);
    void updateLiveDragNode(const data::ContainerId& id,
                            const data::ContainerId& targetParentId,
                            std::size_t targetIndex,
                            const data::ContainerGeometry& geometry);
    void updateLiveImageResize(const data::ContainerId& id,
                               const QSizeF& displaySize,
                               const data::ContainerGeometry& geometry);
    void clearLiveDrag();
    bool commitContainerGeometry(const data::ContainerId& id, const data::ContainerGeometry& geometry);
    bool moveContainer(const data::ContainerId& id,
                       const data::ContainerId& targetParentId,
                       std::size_t targetIndex,
                       const data::ContainerGeometry& geometry);
    bool commitImageResize(const data::ContainerId& id,
                           const QSizeF& displaySize,
                           const data::ContainerGeometry& geometry);

    // 容器选中与删除 (v0.2 阶段一)
    const interaction::ContainerSelectionState& containerSelection() const { return containerSelection_; }
    void selectContainer(const data::ContainerId& id, bool multi = false);
    void clearContainerSelection();
    bool removeSelectedContainers();

    // 容器操作与撤销重做 (v0.2 阶段二)
    std::optional<data::ContainerId> addTextContainer(const data::ContainerId& parentId = {}, const QString& initialText = {});
    std::optional<data::ContainerId> addGroupContainer(const data::ContainerId& parentId = {});
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    bool undo();
    bool redo();

    void select(const interaction::Hit& anchor, const interaction::Hit& focus);
    bool beginSelection(QPointF paperPoint, bool extendSelection);
    bool dragSelection(QPointF paperPoint);
    bool endSelection();
    bool key(const interaction::KeyInput& key, const QString& clipboardText);
    bool ime(const interaction::ImeInput& ime);
    bool action(const interaction::EditAction& action);
    io::DocxExportResult exportTo(const QString& path) const;

signals:
    void changed(bool layoutChanged);
    void clipboardRequested(const QString& text);
    void editRejected(const QString& reason);
    void containerSelectionChanged();

private:
    void startDocument(document::Document document, const data::ContainerId& textId);
    void setLayoutSnapshot(data::DocumentSnapshot snapshot);
    bool accept(interaction::InteractionOutput output);

    struct LiveDragKey {
        enum class Kind { Geometry, Plan, ImageResize };
        Kind kind = Kind::Geometry;
        data::ContainerId id;
        data::ContainerId parentId;
        std::size_t index = 0;
        data::PlacementMode placement = data::PlacementMode::Flow;
        QPointF position;
        qreal width = 0;
        QSizeF displaySize;
    };

    struct PendingLiveDrag {
        LiveDragKey key;
        interaction::DragDropPlan plan;
        data::ContainerGeometry geometry;
    };

    static bool sameLiveDragKey(const LiveDragKey& a, const LiveDragKey& b);
    void applyLiveDragImmediate(const PendingLiveDrag& pending);
    void scheduleOrApplyLiveDrag(PendingLiveDrag pending, bool structureChanged);
    void flushPendingLiveDrag();
    void stopLiveDragTimer();

    document::Document document_;
    data::DocumentSnapshot snapshot_;
    data::LayoutResult layout_;
    interaction::InteractionState state_;
    interaction::ContainerSelectionState containerSelection_;
    struct UndoEntry {
        data::DocumentSnapshot snapshot;
        interaction::InteractionState state;
    };
    std::vector<UndoEntry> undoStack_;
    std::vector<UndoEntry> redoStack_;
    std::optional<interaction::Hit> dragAnchor_;
    std::optional<data::DocumentSnapshot> liveDragSnapshot_;
    std::optional<data::LayoutResult> liveDragLayout_;
    std::optional<LiveDragKey> lastLiveDragKey_;
    std::optional<PendingLiveDrag> pendingLiveDrag_;
    QElapsedTimer liveDragElapsed_;
    QTimer liveDragFlushTimer_;
    QHash<QString, data::ResourceId> importedImages_;
};

} // namespace editor::app
