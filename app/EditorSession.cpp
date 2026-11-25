#include "EditorSession.h"

#include "DocxExporter.h"
#include "CrossContainerCursor.h"
#include "InteractionController.h"
#include "LayoutModule.h"
#include "Selection.h"
#include "TextBoundary.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <utility>
#include <QDebug>

namespace editor::app {
namespace {

using interaction::EditOp;
using interaction::EditRequest;

data::ContainerId firstTextId(const data::ContainerNode& node)
{
    if (node.type == data::ContainerType::Text)
        return node.id;
    for (const auto& child : node.children) {
        const auto id = firstTextId(child);
        if (!id.isEmpty())
            return id;
    }
    return {};
}

void collectTextContainers(const data::ContainerNode& node, std::vector<data::ContainerId>& out)
{
    if (node.type == data::ContainerType::Text)
        out.push_back(node.id);
    for (const auto& child : node.children)
        collectTextContainers(child, out);
}

const data::ParagraphInfo* paragraphAt(const data::TextContent& text, int offset)
{
    const auto paragraph = std::upper_bound(text.paragraphs.begin(), text.paragraphs.end(), offset,
        [](int position, const data::ParagraphInfo& info) { return position < info.range.start; });
    if (paragraph == text.paragraphs.begin())
        return nullptr;
    const auto& candidate = *(paragraph - 1);
    return offset <= candidate.range.end ? &candidate : nullptr;
}

void mergeStyle(data::TextStyle& style, const data::TextStyleOverride& change)
{
    if (change.fontFamily) style.fontFamily = *change.fontFamily;
    if (change.fontSizePt) style.fontSizePt = *change.fontSizePt;
    if (change.color) style.color = *change.color;
    if (change.bold) style.bold = *change.bold;
    if (change.italic) style.italic = *change.italic;
    if (change.underline) style.underline = *change.underline;
}

bool applyRequest(document::Document& document, const data::DocumentSnapshot& snapshot,
                  const EditRequest& request)
{
    const auto& id = request.containerId;
    const auto range = request.range;
    switch (request.op) {
    case EditOp::Insert:
    case EditOp::Replace:
        return document.replaceText(id, range, request.text);
    case EditOp::Delete:
        return document.replaceText(id, range, {});
    case EditOp::SetCharFormat:
        return document.applyTextStyle(id, range, request.charStyle);
    case EditOp::SetParagraphFormat: {
        const auto* text = interaction::findTextContent(snapshot, id);
        const auto* paragraph = text ? paragraphAt(*text, range.start) : nullptr;
        if (!paragraph)
            return false;
        if (request.paragraphFormat &&
            !document.setParagraphFormat(id, range.start, *request.paragraphFormat, paragraph->defaultStyle))
            return false;

        const auto role = request.role.value_or(paragraph->role);
        if (request.role || (request.headingLevel && role == data::ParagraphRole::Heading)) {
            const int level = request.headingLevel.value_or(paragraph->headingLevel);
            return document.setParagraphRole(id, range.start, role, level);
        }
        return true;
    }
    }
    return false;
}

QSize imageSize(const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    auto size = reader.size();
    if (size.isEmpty())
        return reader.read().size();
    if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        size.transpose();
    return size;
}

} // namespace

EditorSession::EditorSession(QObject* parent) : QObject(parent)
{
    liveDragFlushTimer_.setSingleShot(true);
    connect(&liveDragFlushTimer_, &QTimer::timeout, this, &EditorSession::flushPendingLiveDrag);
    newDocument();
}

bool EditorSession::newDocument()
{
    document::Document document;
    const auto textId = document.addText(QStringLiteral("root"));
    if (!textId) {
        emit editRejected(QStringLiteral("无法创建空白正文"));
        return false;
    }
    startDocument(std::move(document), *textId);
    return true;
}

bool EditorSession::loadSnapshot(const data::DocumentSnapshot& snapshot)
{
    QString error;
    auto document = document::Document::fromSnapshot(snapshot, &error);
    if (!document) {
        emit editRejected(error);
        return false;
    }
    auto textId = firstTextId(snapshot.root);
    if (textId.isEmpty()) {
        const auto added = document->addText(snapshot.root.id);
        if (!added) {
            emit editRejected(QStringLiteral("无法为文档添加正文"));
            return false;
        }
        textId = *added;
    }
    startDocument(std::move(*document), textId);
    return true;
}

void EditorSession::startDocument(document::Document document, const data::ContainerId& textId)
{
    stopLiveDragTimer();
    liveDragSnapshot_.reset();
    liveDragLayout_.reset();
    setLayoutSnapshot(document.toSnapshot());
    if (!textId.isEmpty())
        state_ = interaction::makeInitialState(snapshot_, layout_, textId);
    else
        state_ = interaction::InteractionState{};
    containerSelection_.clear();
    dragAnchor_.reset();
    document_ = std::move(document);
    importedImages_.clear();
    emit containerSelectionChanged();
    emit changed(true);
}

void EditorSession::selectContainer(const data::ContainerId& id, bool multi)
{
    containerSelection_.select(id, multi);
    // 选中容器时，折叠文字选区
    state_.selection.reset();
    state_.selectionAnchor.reset();
    emit containerSelectionChanged();
    emit changed(false);
}

void EditorSession::clearContainerSelection()
{
    if (!containerSelection_.empty()) {
        containerSelection_.clear();
        emit containerSelectionChanged();
        emit changed(false);
    }
}

bool EditorSession::removeSelectedContainers()
{
    if (containerSelection_.empty())
        return false;

    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();

    std::vector<data::ContainerId> textOrderBefore;
    collectTextContainers(snapshot_.root, textOrderBefore);

    const auto toDelete = containerSelection_.selectedIds;
    const auto currentCaretContainer = state_.caret.containerId;
    containerSelection_.clear();

    for (const auto& id : toDelete) {
        document_.removeNode(id);
    }

    auto newSnapshot = document_.toSnapshot();
    std::vector<data::ContainerId> textOrderAfter;
    collectTextContainers(newSnapshot.root, textOrderAfter);

    const bool caretDeleted = std::find(textOrderAfter.begin(), textOrderAfter.end(), currentCaretContainer) == textOrderAfter.end();

    setLayoutSnapshot(std::move(newSnapshot));

    if (textOrderAfter.empty()) {
        state_ = interaction::InteractionState{};
    } else if (caretDeleted) {
        auto it = std::find(textOrderBefore.begin(), textOrderBefore.end(), currentCaretContainer);
        const int deletedIdx = it != textOrderBefore.end() ? static_cast<int>(std::distance(textOrderBefore.begin(), it)) : 0;

        data::ContainerId targetId;
        int targetOffset = 0;
        bool found = false;

        for (int i = deletedIdx - 1; i >= 0; --i) {
            const auto& candidate = textOrderBefore[i];
            if (std::find(textOrderAfter.begin(), textOrderAfter.end(), candidate) != textOrderAfter.end()) {
                targetId = candidate;
                targetOffset = interaction::textLength(snapshot_, targetId);
                found = true;
                break;
            }
        }
        if (!found) {
            for (size_t i = deletedIdx + 1; i < textOrderBefore.size(); ++i) {
                const auto& candidate = textOrderBefore[i];
                if (std::find(textOrderAfter.begin(), textOrderAfter.end(), candidate) != textOrderAfter.end()) {
                    targetId = candidate;
                    targetOffset = 0;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            targetId = textOrderAfter.front();
            targetOffset = 0;
        }

        state_ = interaction::makeInitialState(snapshot_, layout_, targetId, targetOffset);
    } else {
        state_ = interaction::refreshStateGeometry(state_, layout_);
    }

    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

std::optional<data::ContainerId> EditorSession::addTextContainer(const data::ContainerId& parentId, const QString& initialText)
{
    const auto targetParent = parentId.isEmpty() ? snapshot_.root.id : parentId;
    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();

    const auto added = document_.addText(targetParent, initialText);
    if (!added) {
        undoStack_.pop_back();
        emit editRejected(QStringLiteral("无法添加文本容器"));
        return std::nullopt;
    }

    setLayoutSnapshot(document_.toSnapshot());
    containerSelection_.clear();

    const auto savedPending = state_.pendingCharStyle;
    state_ = interaction::makeInitialState(snapshot_, layout_, *added, 0);
    state_.pendingCharStyle = savedPending;

    emit containerSelectionChanged();
    emit changed(true);
    return added;
}

std::optional<data::ContainerId> EditorSession::addGroupContainer(const data::ContainerId& parentId)
{
    const auto targetParent = parentId.isEmpty() ? snapshot_.root.id : parentId;
    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();

    const auto added = document_.addGroup(targetParent);
    if (!added) {
        undoStack_.pop_back();
        emit editRejected(QStringLiteral("无法添加分组容器"));
        return std::nullopt;
    }

    setLayoutSnapshot(document_.toSnapshot());
    containerSelection_.clear();
    containerSelection_.select(*added, false);
    state_ = interaction::refreshStateGeometry(state_, layout_);

    emit containerSelectionChanged();
    emit changed(true);
    return added;
}

bool EditorSession::undo()
{
    if (undoStack_.empty())
        return false;

    stopLiveDragTimer();
    liveDragSnapshot_.reset();
    liveDragLayout_.reset();

    auto prevEntry = std::move(undoStack_.back());
    undoStack_.pop_back();

    redoStack_.push_back({snapshot_, state_});

    QString error;
    auto restored = document::Document::fromSnapshot(prevEntry.snapshot, &error);
    if (!restored) {
        emit editRejected(error);
        return false;
    }

    document_ = std::move(*restored);
    setLayoutSnapshot(std::move(prevEntry.snapshot));
    containerSelection_.clear();

    std::vector<data::ContainerId> textNodes;
    collectTextContainers(snapshot_.root, textNodes);
    if (textNodes.empty()) {
        state_ = interaction::InteractionState{};
    } else {
        const auto it = std::find(textNodes.begin(), textNodes.end(), prevEntry.state.caret.containerId);
        if (it != textNodes.end()) {
            state_ = interaction::refreshStateGeometry(prevEntry.state, layout_);
        } else {
            state_ = interaction::makeInitialState(snapshot_, layout_, textNodes.front(), 0);
        }
    }

    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

bool EditorSession::redo()
{
    if (redoStack_.empty())
        return false;

    stopLiveDragTimer();
    liveDragSnapshot_.reset();
    liveDragLayout_.reset();

    auto nextEntry = std::move(redoStack_.back());
    redoStack_.pop_back();

    undoStack_.push_back({snapshot_, state_});

    QString error;
    auto restored = document::Document::fromSnapshot(nextEntry.snapshot, &error);
    if (!restored) {
        emit editRejected(error);
        return false;
    }

    document_ = std::move(*restored);
    setLayoutSnapshot(std::move(nextEntry.snapshot));
    containerSelection_.clear();

    std::vector<data::ContainerId> textNodes;
    collectTextContainers(snapshot_.root, textNodes);
    if (textNodes.empty()) {
        state_ = interaction::InteractionState{};
    } else {
        const auto it = std::find(textNodes.begin(), textNodes.end(), nextEntry.state.caret.containerId);
        if (it != textNodes.end()) {
            state_ = interaction::refreshStateGeometry(nextEntry.state, layout_);
        } else {
            state_ = interaction::makeInitialState(snapshot_, layout_, textNodes.front(), 0);
        }
    }

    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

void EditorSession::setLayoutSnapshot(data::DocumentSnapshot snapshot)
{
    auto newLayout = layout::composeLayout(snapshot);
    snapshot_ = std::move(snapshot);
    layout_ = std::move(newLayout);
}

static data::ContainerNode* findNodeMut(data::ContainerNode& root, const data::ContainerId& id)
{
    if (root.id == id)
        return &root;
    for (auto& child : root.children) {
        if (auto* found = findNodeMut(child, id))
            return found;
    }
    return nullptr;
}

static data::ContainerNode* findNodeAndParentInTree(
    data::ContainerNode& current,
    const data::ContainerId& id,
    data::ContainerNode* parent,
    data::ContainerNode** outParent)
{
    if (current.id == id) {
        if (outParent)
            *outParent = parent;
        return &current;
    }
    for (auto& child : current.children) {
        if (auto* found = findNodeAndParentInTree(child, id, &current, outParent)) {
            return found;
        }
    }
    return nullptr;
}

bool EditorSession::sameLiveDragKey(const LiveDragKey& a, const LiveDragKey& b)
{
    if (a.kind != b.kind || a.id != b.id || a.parentId != b.parentId ||
        a.index != b.index || a.placement != b.placement)
        return false;
    return std::abs(a.position.x() - b.position.x()) < 0.5 &&
           std::abs(a.position.y() - b.position.y()) < 0.5 &&
           std::abs(a.width - b.width) < 0.5 &&
           std::abs(a.displaySize.width() - b.displaySize.width()) < 0.5 &&
           std::abs(a.displaySize.height() - b.displaySize.height()) < 0.5;
}

void EditorSession::stopLiveDragTimer()
{
    liveDragFlushTimer_.stop();
    pendingLiveDrag_.reset();
    lastLiveDragKey_.reset();
    liveDragElapsed_.invalidate();
}

void EditorSession::flushPendingLiveDrag()
{
    if (!pendingLiveDrag_)
        return;
    const auto pending = std::move(*pendingLiveDrag_);
    pendingLiveDrag_.reset();
    applyLiveDragImmediate(pending);
}

void EditorSession::applyLiveDragImmediate(const PendingLiveDrag& pending)
{
    auto candidate = snapshot_;
    switch (pending.key.kind) {
    case LiveDragKey::Kind::Geometry:
        if (auto* node = findNodeMut(candidate.root, pending.key.id)) {
            node->geometry = pending.geometry;
        } else {
            return;
        }
        break;
    case LiveDragKey::Kind::Plan:
        if (!interaction::ContainerDragEngine::applyPlanToCandidateSnapshot(
                candidate, pending.key.id, pending.plan, pending.geometry)) {
            return;
        }
        break;
    case LiveDragKey::Kind::ImageResize:
        if (auto* node = findNodeMut(candidate.root, pending.key.id);
            node && node->type == data::ContainerType::Image) {
            node->image.displaySize = pending.key.displaySize;
            node->geometry = pending.geometry;
        } else {
            return;
        }
        break;
    }

    auto candLayout = layout::composeLayout(candidate);
    liveDragSnapshot_ = std::move(candidate);
    liveDragLayout_ = std::move(candLayout);
    lastLiveDragKey_ = pending.key;
    liveDragElapsed_.restart();
    state_ = interaction::refreshStateGeometry(state_, *liveDragLayout_);
    emit changed(true);
}

void EditorSession::scheduleOrApplyLiveDrag(PendingLiveDrag pending, bool structureChanged)
{
    if (liveDragLayout_.has_value() && lastLiveDragKey_.has_value() &&
        sameLiveDragKey(*lastLiveDragKey_, pending.key)) {
        pendingLiveDrag_.reset();
        liveDragFlushTimer_.stop();
        return;
    }

    const bool mustRunNow = !liveDragLayout_.has_value() ||
        structureChanged ||
        !liveDragElapsed_.isValid() ||
        liveDragElapsed_.elapsed() >= 16;

    if (mustRunNow) {
        pendingLiveDrag_.reset();
        liveDragFlushTimer_.stop();
        applyLiveDragImmediate(pending);
    } else {
        pendingLiveDrag_ = std::move(pending);
        if (!liveDragFlushTimer_.isActive()) {
            const int remaining = qMax<int>(1, 16 - static_cast<int>(liveDragElapsed_.elapsed()));
            liveDragFlushTimer_.start(remaining);
        }
    }
}

void EditorSession::updateLiveDragGeometry(const data::ContainerId& id, const data::ContainerGeometry& geometry)
{
    PendingLiveDrag pending;
    pending.key.kind = LiveDragKey::Kind::Geometry;
    pending.key.id = id;
    pending.key.position = geometry.position;
    pending.key.width = geometry.width;
    pending.geometry = geometry;
    scheduleOrApplyLiveDrag(std::move(pending), false);
}

void EditorSession::updateLiveDragWithPlan(const data::ContainerId& id,
                                           const interaction::DragDropPlan& plan,
                                           const data::ContainerGeometry& geometry)
{
    PendingLiveDrag pending;
    pending.key.kind = LiveDragKey::Kind::Plan;
    pending.key.id = id;
    pending.key.parentId = plan.targetParentId.isEmpty() ? snapshot_.root.id : plan.targetParentId;
    pending.key.index = plan.targetIndex;
    pending.key.placement = plan.placementMode;
    pending.key.position = plan.localPosition;
    pending.key.width = geometry.width;
    if (plan.targetDisplaySize)
        pending.key.displaySize = *plan.targetDisplaySize;
    pending.plan = plan;
    pending.geometry = geometry;

    const bool structureChanged = !lastLiveDragKey_.has_value() ||
        lastLiveDragKey_->parentId != pending.key.parentId ||
        lastLiveDragKey_->index != pending.key.index ||
        lastLiveDragKey_->placement != pending.key.placement;

    scheduleOrApplyLiveDrag(std::move(pending), structureChanged);
}

void EditorSession::updateLiveDragNode(const data::ContainerId& id,
                                       const data::ContainerId& targetParentId,
                                       std::size_t targetIndex,
                                       const data::ContainerGeometry& geometry)
{
    interaction::DragDropPlan plan;
    plan.targetParentId = targetParentId;
    plan.targetIndex = targetIndex;
    plan.localPosition = geometry.position;
    const auto* parentNode = (targetParentId.isEmpty() || targetParentId == snapshot_.root.id)
        ? &snapshot_.root
        : findNodeMut(snapshot_.root, targetParentId);
    if (parentNode && parentNode->type == data::ContainerType::Text) {
        plan.placementMode = data::PlacementMode::Floating;
    } else {
        plan.placementMode = data::PlacementMode::Flow;
    }
    updateLiveDragWithPlan(id, plan, geometry);
}

void EditorSession::updateLiveImageResize(const data::ContainerId& id,
                                          const QSizeF& displaySize,
                                          const data::ContainerGeometry& geometry)
{
    PendingLiveDrag pending;
    pending.key.kind = LiveDragKey::Kind::ImageResize;
    pending.key.id = id;
    pending.key.position = geometry.position;
    pending.key.displaySize = displaySize;
    pending.geometry = geometry;
    scheduleOrApplyLiveDrag(std::move(pending), false);
}

void EditorSession::clearLiveDrag()
{
    stopLiveDragTimer();
    if (liveDragLayout_) {
        liveDragLayout_.reset();
        liveDragSnapshot_.reset();
        state_ = interaction::refreshStateGeometry(state_, layout_);
        emit changed(true);
    }
}

bool EditorSession::commitContainerGeometry(const data::ContainerId& id, const data::ContainerGeometry& geometry)
{
    clearLiveDrag();
    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();

    if (!document_.setContainerGeometry(id, geometry)) {
        undoStack_.pop_back();
        emit editRejected(QStringLiteral("无法更新容器几何信息"));
        return false;
    }

    setLayoutSnapshot(document_.toSnapshot());
    state_ = interaction::refreshStateGeometry(state_, layout_);
    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

bool EditorSession::moveContainer(const data::ContainerId& id,
                                   const data::ContainerId& targetParentId,
                                   std::size_t targetIndex,
                                   const data::ContainerGeometry& geometry)
{
    clearLiveDrag();
    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();

    const auto resolvedTargetParentId = targetParentId.isEmpty() ? snapshot_.root.id : targetParentId;

    const auto nodeInfo = document_.queryNode(id);
    const auto parentInfo = document_.queryNode(resolvedTargetParentId);
    if (!nodeInfo || !parentInfo) {
        undoStack_.pop_back();
        emit editRejected(QStringLiteral("无法找到移动目标节点"));
        return false;
    }

    const bool sameParent = nodeInfo->parentId == resolvedTargetParentId;
    const std::size_t maxAllowedIndex = sameParent
        ? (parentInfo->childIds.empty() ? 0 : parentInfo->childIds.size() - 1)
        : parentInfo->childIds.size();
    const std::size_t clampedIndex = qMin(targetIndex, maxAllowedIndex);

    if (!document_.moveNode(id, resolvedTargetParentId, clampedIndex, geometry.position)) {
        undoStack_.pop_back();
        emit editRejected(QStringLiteral("无法移动容器"));
        return false;
    }

    document_.setContainerGeometry(id, geometry);

    setLayoutSnapshot(document_.toSnapshot());
    state_ = interaction::refreshStateGeometry(state_, layout_);
    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

bool EditorSession::commitImageResize(const data::ContainerId& id,
                                      const QSizeF& displaySize,
                                      const data::ContainerGeometry& geometry)
{
    clearLiveDrag();
    if (!std::isfinite(displaySize.width()) || displaySize.width() <= 0 ||
        !std::isfinite(displaySize.height()) || displaySize.height() <= 0) {
        emit editRejected(QStringLiteral("图片显示尺寸无效"));
        return false;
    }

    auto updated = snapshot_;
    auto* node = findNodeMut(updated.root, id);
    if (!node || node->type != data::ContainerType::Image) {
        emit editRejected(QStringLiteral("无法找到目标图片容器"));
        return false;
    }

    node->image.displaySize = displaySize;
    node->geometry = geometry;
    updated.revision = snapshot_.revision + 1;

    QString error;
    auto restored = document::Document::fromSnapshot(updated, &error);
    if (!restored) {
        emit editRejected(error);
        return false;
    }

    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();
    document_ = std::move(*restored);
    setLayoutSnapshot(document_.toSnapshot());
    state_ = interaction::refreshStateGeometry(state_, layout_);
    emit containerSelectionChanged();
    emit changed(true);
    return true;
}

std::optional<data::ContainerId> EditorSession::insertImage(const QString& path, data::PlacementMode placement)
{
    const auto reject = [this](const QString& reason) -> std::optional<data::ContainerId> {
        emit editRejected(reason);
        return std::nullopt;
    };
    const bool floating = placement == data::PlacementMode::Floating;
    const auto active = document_.queryNode(state_.caret.containerId);
    const auto parentId = floating ? state_.caret.containerId : snapshot_.root.id;
    QRectF area = layout_.contentRect;
    data::ContainerGeometry geometry;
    geometry.margin = QMarginsF(0, 8, 0, 8);

    if (floating) {
        if (!active || active->node.type != data::ContainerType::Text ||
            active->parentId != snapshot_.root.id)
            return reject(QStringLiteral("请先把光标放在顶层正文中"));
        const auto box = std::find_if(layout_.containers.begin(), layout_.containers.end(),
            [&parentId](const auto& item) { return item.containerId == parentId; });
        if (box == layout_.containers.end())
            return reject(QStringLiteral("找不到当前正文的排版区域"));
        area = box->contentRect;
        geometry.margin = QMarginsF(8, 6, 8, 6);
    }

    const QFileInfo source(path);
    const auto canonical = source.canonicalFilePath();
    const auto resourceKey = canonical.isEmpty() ? source.absoluteFilePath() : canonical;
    const auto resourceId = importedImages_.value(resourceKey);
    const auto existing = snapshot_.resources.constFind(resourceId);
    const bool reuseImage = existing != snapshot_.resources.cend();
    const QSize originalSize = reuseImage ? existing->size() : imageSize(path);
    if (originalSize.isEmpty())
        return reject(QStringLiteral("无法读取图片 %1").arg(source.fileName()));

    const qreal maxWidth = area.width() * (floating ? 0.4 : 1)
        - geometry.margin.left() - geometry.margin.right();
    const qreal maxHeight = layout_.contentRect.height() * (floating ? 0.4 : 1)
        - geometry.margin.top() - geometry.margin.bottom();
    if (maxWidth <= 0 || maxHeight <= 0)
        return reject(QStringLiteral("正文区域太小，无法放入图片"));
    const auto scale = std::min({qreal(1), maxWidth / originalSize.width(), maxHeight / originalSize.height()});
    const QSizeF displaySize = QSizeF(originalSize) * scale;

    if (floating) {
        const qreal x = area.width() - geometry.margin.right() - displaySize.width();
        qreal y = qMax(geometry.margin.top(), state_.caret.rect.top() - area.top());
        const qreal left = area.left() + x - geometry.margin.left();
        for (const auto& box : layout_.containers) {
            if (box.parentId == parentId && box.outerRect.right() > left &&
                box.outerRect.left() < area.right())
                y = qMax(y, box.outerRect.bottom() - area.top() + geometry.margin.top());
        }
        geometry.position = QPointF(x, y);
    }

    // 图片及其位置一起提交。路径和资源 ID 的对应关系由会话保留。
    QString error;
    auto staged = document::Document::fromSnapshot(snapshot_, &error);
    if (!staged)
        return reject(error);
    const auto added = reuseImage ? staged->addImage(parentId, resourceId, displaySize)
                                  : staged->addImageFromFile(parentId, path, displaySize);
    if (!added || !staged->setContainerGeometry(*added, geometry))
        return reject(QStringLiteral("无法插入图片 %1").arg(source.fileName()));

    const auto insertedResource = staged->queryNode(*added)->node.image.resourceId;
    undoStack_.push_back({snapshot_, state_});
    redoStack_.clear();
    setLayoutSnapshot(staged->toSnapshot());
    document_ = std::move(*staged);
    importedImages_.insert(resourceKey, insertedResource);
    state_ = interaction::refreshStateGeometry(state_, layout_);
    emit changed(true);
    return added;
}

const data::ParagraphInfo* EditorSession::currentParagraph() const
{
    const auto* text = interaction::findTextContent(snapshot_, state_.caret.containerId);
    return text ? paragraphAt(*text, state_.caret.offset) : nullptr;
}

data::TextStyle EditorSession::currentStyle() const
{
    const auto* text = interaction::findTextContent(snapshot_, state_.caret.containerId);
    if (!text)
        return {};
    const int offset = state_.selection ? state_.selection->range.start : state_.caret.offset;
    auto style = text->defaultStyle;
    if (const auto* paragraph = paragraphAt(*text, offset))
        style = layout::resolveParagraphStyle(style, *paragraph);
    const int character = offset == text->text.size() ? offset - 1 : offset;
    const auto span = std::lower_bound(text->formatSpans.begin(), text->formatSpans.end(), character,
        [](const data::TextFormatSpan& entry, int position) { return entry.range.end <= position; });
    if (span != text->formatSpans.end() && span->range.start <= character)
        mergeStyle(style, span->style);
    if (state_.pendingCharStyle)
        mergeStyle(style, *state_.pendingCharStyle);
    return style;
}

void EditorSession::select(const interaction::Hit& anchor, const interaction::Hit& focus)
{
    if (!anchor.valid() || !focus.valid() || anchor.containerId != focus.containerId)
        return;
    const auto* content = interaction::findTextContent(snapshot_, focus.containerId);
    if (!content)
        return;

    const int start = interaction::alignToGrapheme(content->text, anchor.offset);
    const int end = interaction::alignToGrapheme(content->text, focus.offset);
    const std::optional<int> newAnchor = (start == end) ? std::nullopt : std::optional<int>(start);
    if (state_.caret.containerId == focus.containerId &&
        state_.caret.offset == end &&
        state_.selectionAnchor == newAnchor &&
        state_.selection.has_value() == (start != end)) {
        return;
    }
    //里面直接默认构造了，所以出问题
    auto state = interaction::makeInitialState(snapshot_, layout_, focus.containerId, end);
    //这里直接赋值回原来的就好，目前最小的改动就是这样，之后建议增加interaction::makeInitialState(snapshot_, layout_, containerId, offset, &state_);
    state.pendingCharStyle = state_.pendingCharStyle;
    if (start != end) {
        state.selection = interaction::makeSelection(layout_, {anchor.containerId, start},
                                                    {focus.containerId, end});
        state.selectionAnchor = start;
    }
    state_ = std::move(state);
    emit changed(false);
}

bool EditorSession::beginSelection(QPointF paperPoint, bool extendSelection)
{
    const auto hit = interaction::hitTest(layout_, paperPoint);
    if (!hit.valid())
        return false;

    dragAnchor_ = hit;
    if (extendSelection) {
        dragAnchor_ = interaction::Hit{state_.caret.containerId,
                                      state_.selectionAnchor.value_or(state_.caret.offset)};
        return dragSelection(paperPoint);
    }
    select(hit, hit);
    return true;
}

bool EditorSession::dragSelection(QPointF paperPoint)
{
    if (!dragAnchor_)
        return false;
    select(*dragAnchor_, interaction::clampFocus(layout_, *dragAnchor_, paperPoint));
    return true;
}

bool EditorSession::endSelection()
{
    return std::exchange(dragAnchor_, std::nullopt).has_value();
}

bool EditorSession::key(const interaction::KeyInput& key, const QString& clipboardText)
{
    const bool control = key.mods.testFlag(Qt::ControlModifier);
    const bool shift = key.mods.testFlag(Qt::ShiftModifier);
    const bool alt = key.mods.testFlag(Qt::AltModifier);
    const bool meta = key.mods.testFlag(Qt::MetaModifier);

    // 快捷键撤销 / 重做
    if (control && !shift && !alt && !meta && key.key == Qt::Key_Z) {
        return undo();
    }
    if ((control && shift && key.key == Qt::Key_Z) || (control && !shift && !alt && !meta && key.key == Qt::Key_Y)) {
        return redo();
    }

    // 方向键（未按 Shift）走 action 统一处理跨容器导航
    if (!shift && !alt && !meta) {
        interaction::EditAction act;
        if (key.key == Qt::Key_Left) {
            act.kind = interaction::EditActionKind::MoveLeft;
            return action(act);
        }
        if (key.key == Qt::Key_Right) {
            act.kind = interaction::EditActionKind::MoveRight;
            return action(act);
        }
        if (key.key == Qt::Key_Up) {
            act.kind = interaction::EditActionKind::MoveUp;
            return action(act);
        }
        if (key.key == Qt::Key_Down) {
            act.kind = interaction::EditActionKind::MoveDown;
            return action(act);
        }
    }

    return accept(interaction::applyKey(state_, key, snapshot_, layout_, clipboardText));
}

bool EditorSession::ime(const interaction::ImeInput& ime)
{
    return accept(interaction::applyIme(state_, ime, snapshot_, layout_));
}

bool EditorSession::action(const interaction::EditAction& action)
{
    if (!action.extendSelection) {
        if (action.kind == interaction::EditActionKind::MoveLeft) {
            if (auto crossState = interaction::tryCrossLeft(state_, snapshot_, layout_)) {
                state_ = *crossState;
                emit changed(false);
                return true;
            }
        } else if (action.kind == interaction::EditActionKind::MoveRight) {
            if (auto crossState = interaction::tryCrossRight(state_, snapshot_, layout_)) {
                state_ = *crossState;
                emit changed(false);
                return true;
            }
        } else if (action.kind == interaction::EditActionKind::MoveUp) {
            if (auto crossState = interaction::tryCrossVertical(state_, snapshot_, layout_, false)) {
                state_ = *crossState;
                emit changed(false);
                return true;
            }
        } else if (action.kind == interaction::EditActionKind::MoveDown) {
            if (auto crossState = interaction::tryCrossVertical(state_, snapshot_, layout_, true)) {
                state_ = *crossState;
                emit changed(false);
                return true;
            }
        }
    }
    return accept(interaction::applyAction(state_, action, snapshot_, layout_));
}

bool EditorSession::accept(interaction::InteractionOutput output)
{
    bool layoutChanged = false;
    if (!output.requests.empty()) {
        const auto& first = output.requests.front();
        const bool useStaging = output.requests.size() > 1 ||
            (first.paragraphFormat && (first.role || first.headingLevel));

        // 一次事件的多条请求先在临时文档执行，全部成功才替换当前文档。
        std::optional<document::Document> staged;
        if (useStaging) {
            QString error;
            staged = document::Document::fromSnapshot(snapshot_, &error);
            if (!staged) {
                emit editRejected(error);
                return false;
            }
        }
        auto& target = staged ? *staged : document_;
        for (const auto& request : output.requests) {
            if (!applyRequest(target, snapshot_, request)) {
                emit editRejected(QStringLiteral("容器 %1 的编辑请求无效").arg(request.containerId));
                return false;
            }
        }

        auto snapshot = target.toSnapshot();
        layoutChanged = snapshot.revision != snapshot_.revision;
        if (layoutChanged) {
            undoStack_.push_back({snapshot_, state_});
            redoStack_.clear();
            setLayoutSnapshot(std::move(snapshot));
            if (staged)
                document_ = std::move(*staged);
        }
    }
    state_ = layoutChanged ? interaction::refreshStateGeometry(output.state, layout_)
                           : std::move(output.state);
    emit changed(layoutChanged);
    if (output.clipboardText)
        emit clipboardRequested(*output.clipboardText);
    return true;
}

io::DocxExportResult EditorSession::exportTo(const QString& path) const
{
    const auto built = io::buildDocx(snapshot_, layout_);
    if (!built.result.ok())
        return built.result;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(built.package) != built.package.size() || !file.commit()) {
        return {io::DocxExportStatus::IoError,
                QStringLiteral("无法写入 %1（%2）").arg(path, file.errorString()), {}};
    }
    return built.result;
}

} // namespace editor::app
