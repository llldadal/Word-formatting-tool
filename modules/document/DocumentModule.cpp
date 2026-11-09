#include "DocumentModule.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>

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

std::optional<ContainerId> Document::addNode(const ContainerId& parentId,
                                             ContainerNode node)
{
    DocumentNode* const parent = nodesById_.value(parentId, nullptr);
    if (parent == nullptr || parent->data_.type == ContainerType::Image) {
        return std::nullopt;
    }

    // v0.1 只有逻辑根及其直接子节点可以接收新节点。
    if (parent != root_.get() && parent->parent_ != root_.get()) {
        return std::nullopt;
    }

    auto id = allocateId();
    if (!id || nodesById_.contains(*id)) {
        return std::nullopt;
    }

    node.id = *id;
    node.placement = parent->data_.type == ContainerType::Text
        ? PlacementMode::Floating
        : PlacementMode::Flow;

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
