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
    struct NodeInfo {
        data::ContainerNode node; // 单个节点的数据。子节点按顺序列在 childIds 中。
        std::optional<data::ContainerId> parentId;
        std::vector<data::ContainerId> childIds;
    };

    explicit Document(QHash<data::ResourceId, QImage> resources = {});

    // 按当前版本生成可独立读取的快照，包含容器树、页面设置和原图。
    data::DocumentSnapshot toSnapshot() const;
    // 从快照恢复一份可编辑文档。失败时返回空值，并通过 error 提供原因。
    static std::optional<Document> fromSnapshot(const data::DocumentSnapshot& snapshot, QString* error = nullptr);

    // 返回节点信息的副本。逻辑根没有父节点，找不到 ID 时返回空值。
    std::optional<NodeInfo> queryNode(const data::ContainerId& id) const;

    // 创建成功时返回新节点的 ID。空正文也可以创建文本节点。
    std::optional<data::ContainerId> addText(const data::ContainerId& parentId,
                                             QString initialText = {});
    // 用已登记的资源 ID 创建图片容器，displaySize 只作用于本次新增的容器。
    std::optional<data::ContainerId> addImage(const data::ContainerId& parentId,
                                             const data::ResourceId& resourceId, const QSizeF& displaySize);
    // 按路径导入原图。同一规范化路径复用本次文档会话中首次导入的图片。
    std::optional<data::ContainerId> addImageFromFile(const data::ContainerId& parentId,
                                                     const QString& imagePath, const QSizeF& displaySize);
    std::optional<data::ContainerId> addGroup(const data::ContainerId& parentId);

    // 只替换指定图片容器的资源引用，沿用原来的显示尺寸和位置。
    bool replaceImage(const data::ContainerId& id, const data::ResourceId& resourceId);
    bool replaceImageFromFile(const data::ContainerId& id, const QString& imagePath);

    // index 是移动完成后从 0 开始的下标。换父时可插到目标列表末尾。
    // 移入正文时需给出相对新父内容区的位置。
    // 省略 position 时，同级移动沿用原位置，换到顺序排版的父节点则使用 (0, 0) 作为期望位置。
    // 换父后文本或分组的内容宽度不足时，移动失败。
    bool moveNode(const data::ContainerId& id, const data::ContainerId& targetParentId,
                  std::size_t index, std::optional<QPointF> position = std::nullopt);

    // 删除容器及其所有子节点，保留图片资源。逻辑根不可删除。
    bool removeNode(const data::ContainerId& id);

    // 更新容器的期望位置、尺寸约束和边距。文本和分组的内容宽度必须为有限的正数。
    // 修改父容器时，同时检查受影响的子节点。
    bool setContainerGeometry(const data::ContainerId& id, const data::ContainerGeometry& geometry);

    // range 使用 UTF-16 的 [start, end)。空范围用于插入，空 replacement 用于删除。
    bool replaceText(const data::ContainerId& id, data::TextRange range, const QString& replacement);
    // 格式区间相对 replacement，按起点排序且互不重叠。未覆盖的文字继承段落格式。
    // 传空列表可清除替换范围里的局部格式。
    bool replaceText(const data::ContainerId& id, data::TextRange range, const QString& replacement,
                     const std::vector<data::TextFormatSpan>& replacementFormats);

    // 只修改选区内已提供的字符属性，跳过换行符。false 可显式取消粗体、斜体或下划线。
    bool applyTextStyle(const data::ContainerId& id, data::TextRange range,
                        const data::TextStyleOverride& style);
    bool setTextDefaultStyle(const data::ContainerId& id, const data::TextStyle& style);

    // 按光标位置修改段落排版和默认字符格式。空的字符格式表示继承容器默认值。
    bool setParagraphFormat(const data::ContainerId& id, int textOffset,
                            const data::ParagraphFormat& format,
                            const data::TextStyleOverride& defaultStyle);

    // textOffset 是 UTF-16 光标偏移，位于换行符时选中换行前的段落。
    // 文末位置也可定位。章节标题的级别为 1 到 9。
    bool setParagraphRole(const data::ContainerId& id, int textOffset,
                          data::ParagraphRole role, int headingLevel = 1);

private:
    class ImageResources {
    public:
        struct Import {
            data::ResourceId id;
            QString addedPath; // 本次新登记的路径，复用已有资源时为空。
        };

        explicit ImageResources(QHash<data::ResourceId, QImage> images);
        const QHash<data::ResourceId, QImage>& images() const;
        bool hasImage(const data::ResourceId& id) const;
        std::optional<Import> loadOrReuse(const QString& imagePath);
        void rollbackImport(const Import& image);

    private:
        QHash<data::ResourceId, QImage> imagesById_;
        QHash<QString, data::ResourceId> idsByPath_; // 用规范化路径查找已导入的原图。
    };

    static data::ContainerNode exportNode(const DocumentNode& node);
    bool importNode(const data::ContainerNode& source, DocumentNode* parent,
                    std::unique_ptr<DocumentNode>& destination, qreal parentWidth, QString* error);

    bool replaceTextImpl(const data::ContainerId& id, data::TextRange range, const QString& replacement,
                         const std::vector<data::TextFormatSpan>& replacementFormats, bool replaceFormatting);

    // 未找到时返回 nullptr。节点指针仅在当前操作中使用。
    DocumentNode* findNode(const data::ContainerId& id) const;
    qreal parentContentWidth(const DocumentNode& parent) const;
    void setImageResource(DocumentNode& node, const data::ResourceId& resourceId);
    void removeSubtreeFromIndex(const DocumentNode& node);
    std::optional<data::PlacementMode> placementForChild(const DocumentNode* parent) const;
    std::optional<data::ContainerId> addNode(const data::ContainerId& parentId,
                                             data::ContainerNode data);
    data::ContainerId allocateId() const;

    std::unique_ptr<DocumentNode> root_;
    // 节点由编辑树持有，索引只用于按 ID 定位节点。
    QHash<data::ContainerId, DocumentNode*> nodesById_;
    ImageResources resources_;
    data::PageSettings page_;
    quint64 revision_ = 1;
};

}

#endif // DOCUMENTMODULE_H
