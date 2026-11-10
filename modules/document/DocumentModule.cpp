#include "DocumentModule.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace editor::document {

using data::ContainerId;
using data::ContainerNode;
using data::ContainerType;
using data::PlacementMode;
using data::ResourceId;

DocumentNode::DocumentNode(ContainerNode data, DocumentNode* parent)
    : data_(std::move(data)), parent_(parent)
{
}

Document::Document(QHash<ResourceId, QImage> resources)
    : root_(std::make_unique<DocumentNode>(
          ContainerNode{QStringLiteral("root"), ContainerType::Group}, nullptr)),
      resources_(std::move(resources))
{
    nodesById_.insert(root_->data_.id, root_.get());
}

std::optional<ContainerId> Document::addText(const ContainerId& parentId,
                                             QString initialText)
{
    if (initialText.size() > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    ContainerNode node{};
    node.type = ContainerType::Text;
    node.text.text = std::move(initialText);

    // 段落范围使用 UTF-16 偏移并排除换行符，空正文也有一个空段落。
    const int end = static_cast<int>(node.text.text.size());
    int paragraphStart = 0;
    for (int i = 0; i < end; ++i) {
        if (node.text.text.at(i) == QLatin1Char('\n')) {
            node.text.paragraphs.push_back({{paragraphStart, i}});
            paragraphStart = i + 1;
        }
    }
    node.text.paragraphs.push_back({{paragraphStart, end}});

    return addNode(parentId, std::move(node));
}

std::optional<ContainerId> Document::addImage(const ContainerId& parentId,
                                              const ResourceId& resourceId,
                                              const QSizeF& displaySize)
{
    const auto resource = resources_.constFind(resourceId);
    if (resource == resources_.cend() || resource.value().isNull()) {
        return std::nullopt;
    }

    if (!std::isfinite(displaySize.width()) || displaySize.width() <= 0 ||
        !std::isfinite(displaySize.height()) || displaySize.height() <= 0) {
        return std::nullopt;
    }

    ContainerNode node{};
    node.type = ContainerType::Image;
    node.image = {resourceId, displaySize};
    return addNode(parentId, std::move(node));
}

std::optional<ContainerId> Document::addGroup(const ContainerId& parentId)
{
    ContainerNode node{};
    node.type = ContainerType::Group;
    return addNode(parentId, std::move(node));
}

bool Document::moveNode(const ContainerId& id, const ContainerId& targetParentId,
                        std::size_t index, std::optional<QPointF> position)
{
    DocumentNode* const node = findNode(id);
    DocumentNode* const targetParent = findNode(targetParentId);
    const auto placement = placementForChild(targetParent);
    if (node == nullptr || node == root_.get() || node == targetParent || !placement) {
        return false;
    }
    // 顶层容器下面只允许一层子节点，移入的节点必须是叶节点。
    if (targetParent != root_.get() && !node->children_.empty()) {
        return false;
    }

    const bool sameParent = node->parent_ == targetParent;
    auto& targetChildren = targetParent->children_;
    // 下标按移动后的子节点数量校验，换父时目标列表会增加一个子节点。
    const auto finalChildCount = targetChildren.size() + (sameParent ? 0 : 1);
    if (index >= finalChildCount) {
        return false;
    }
    if (!sameParent && *placement == PlacementMode::Floating && !position) {
        return false;
    }
    if (position && (!std::isfinite(position->x()) || !std::isfinite(position->y()))) {
        return false;
    }

    auto& sourceChildren = node->parent_->children_;
    const auto source = std::find_if(sourceChildren.begin(), sourceChildren.end(),
                                     [node](const auto& child) { return child.get() == node; });
    const auto target = targetChildren.begin() + static_cast<std::ptrdiff_t>(index);
    auto& currentPosition = node->data_.geometry.position;
    // 同级重排省略 position 时，沿用原来的期望坐标。
    const QPointF newPosition = position.value_or(sameParent ? currentPosition : QPointF{});
    const bool samePosition = currentPosition.x() == newPosition.x() &&
                              currentPosition.y() == newPosition.y();
    if (sameParent && source == target && samePosition) {
        // 请求的位置已经满足，成功返回，版本号无需推进。
        return true;
    }

    if (!sameParent) {
        // 先插入目标列表。若扩容失败，原列表仍持有该节点。
        targetChildren.insert(target, std::move(*source));
        sourceChildren.erase(source);
        node->parent_ = targetParent;
        node->data_.placement = *placement;
    } else if (source < target) {
        std::rotate(source, source + 1, target + 1);
    } else if (source > target) {
        std::rotate(target, source, source + 1);
    }

    // 移动沿用原有的 minimumHeight，实际框高由重新排版计算。
    currentPosition = newPosition;
    ++revision_;
    return true;
}

DocumentNode* Document::findNode(const ContainerId& id)
{
    return nodesById_.value(id, nullptr);
}

std::optional<PlacementMode> Document::placementForChild(const DocumentNode* parent) const
{
    if (parent == nullptr || parent->data_.type == ContainerType::Image) {
        return std::nullopt;
    }

    // v0.1 只有逻辑根及其直接子节点可以接收新节点。
    if (parent != root_.get() && parent->parent_ != root_.get()) {
        return std::nullopt;
    }

    return parent->data_.type == ContainerType::Text ? PlacementMode::Floating : PlacementMode::Flow;
}

std::optional<ContainerId> Document::addNode(const ContainerId& parentId,
                                             ContainerNode node)
{
    DocumentNode* const parent = findNode(parentId);
    const auto placement = placementForChild(parent);
    if (!placement) {
        return std::nullopt;
    }

    auto id = allocateId();
    if (!id || nodesById_.contains(*id)) {
        return std::nullopt;
    }

    node.id = *id;
    node.placement = *placement;

    auto& children = parent->children_;
    children.push_back(std::make_unique<DocumentNode>(std::move(node), parent));
    try {
        nodesById_.insert(*id, children.back().get());
    } catch (...) {
        children.pop_back();
        throw;
    }
    ++revision_;
    return id;
}

std::optional<ContainerId> Document::allocateId()
{
    const QString fileName = QSettings(QSettings::IniFormat, QSettings::UserScope,
                                       QStringLiteral("QtDocumentEditor"),
                                       QStringLiteral("EditorDocument")).fileName();
    if (!QFileInfo(fileName).absoluteDir().mkpath(QStringLiteral("."))) {
        return std::nullopt;
    }

    // 锁覆盖读、递增和写入，避免两个进程同时领取同一个编号。
    QLockFile lock(fileName + QStringLiteral(".id.lock"));
    if (!lock.tryLock(1000)) {
        return std::nullopt;
    }

    const bool initializing = !QFileInfo::exists(fileName);
    QSettings settings(fileName, QSettings::IniFormat);
    settings.sync();

    const QString magicKey = QStringLiteral("magic");
    const QString versionKey = QStringLiteral("version");
    const QString idKey = QStringLiteral("nextNodeId");
    const QString magic = QStringLiteral("QtDocumentEditor.NodeIds");
    const QString version = QStringLiteral("1");

    // 只有首次创建配置时才从 1 发号，已有文件缺字段时直接失败。
    quint64 id = 1;
    if (!initializing) {
        if (settings.value(magicKey).toString() != magic ||
            settings.value(versionKey).toString() != version) {
            return std::nullopt;
        }

        bool ok = false;
        id = settings.value(idKey).toString().toULongLong(&ok);
        // 超过 quint64 上限会使转换失败。等于上限时，写回 id + 1 会回绕成 0。
        if (!ok || id == 0 || id == std::numeric_limits<quint64>::max()) {
            return std::nullopt;
        }
    }
    if (settings.status() != QSettings::NoError) {
        return std::nullopt;
    }

    // 先写入下一个编号，保存成功后才把当前编号交给新节点。
    if (initializing) {
        settings.setValue(magicKey, magic);
        settings.setValue(versionKey, version);
    }
    settings.setValue(idKey, id + 1);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return std::nullopt;
    }

    return QStringLiteral("node-%1").arg(id);
}

}
