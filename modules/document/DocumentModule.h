#ifndef DOCUMENTMODULE_H
#define DOCUMENTMODULE_H

#include "DocumentSnapshot.h"

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

private:
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
