#ifndef DOCUMENTMODULE_H
#define DOCUMENTMODULE_H

#include "DocumentSnapshot.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace editor::document {

class DocumentNode {
public:
    DocumentNode(data::ContainerNode data, DocumentNode* parent);

private:
    friend class Document;

    data::ContainerNode data_;
    // 编辑树由 children_ 持有子节点，data_.children 在编辑期间为空。
    // vector 扩容只移动 unique_ptr，parent_ 指向的包装对象地址保持稳定。
    std::vector<std::unique_ptr<DocumentNode>> children_;
    DocumentNode* parent_;
};

class Document {
public:
    explicit Document(QHash<data::ResourceId, QImage> resources = {});

    // 创建成功时返回新节点的 ID。空正文也可以创建文本节点。
    std::optional<data::ContainerId> addText(const data::ContainerId& parentId,
                                             QString initialText = {});
    std::optional<data::ContainerId> addImage(const data::ContainerId& parentId,
                                              const data::ResourceId& resourceId,
                                              const QSizeF& displaySize);
    std::optional<data::ContainerId> addGroup(const data::ContainerId& parentId);

    // index 是移动完成后从 0 开始的下标。换父时可插到目标列表末尾。
    // 移入正文时需给出相对新父内容区的位置。
    // 省略 position 时，同级移动沿用原位置，换到顺序排版的父节点则使用 (0, 0) 作为期望位置。
    bool moveNode(const data::ContainerId& id, const data::ContainerId& targetParentId,
                  std::size_t index, std::optional<QPointF> position = std::nullopt);

private:
    // 未找到时返回 nullptr。节点指针仅在当前操作中使用。
    DocumentNode* findNode(const data::ContainerId& id);
    std::optional<data::PlacementMode> placementForChild(const DocumentNode* parent) const;
    std::optional<data::ContainerId> addNode(const data::ContainerId& parentId,
                                             data::ContainerNode data);
    std::optional<data::ContainerId> allocateId();

    std::unique_ptr<DocumentNode> root_;
    // 节点由编辑树持有，索引只用于按 ID 定位节点。
    QHash<data::ContainerId, DocumentNode*> nodesById_;
    QHash<data::ResourceId, QImage> resources_;
    quint64 revision_ = 1;
};

}

#endif // DOCUMENTMODULE_H
