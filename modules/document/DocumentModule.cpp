#include "DocumentModule.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QScopeGuard>
#include <QStringView>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace editor::document {

using data::ContainerId;
using data::ContainerGeometry;
using data::ContainerNode;
using data::ContainerType;
using data::DocumentSnapshot;
using data::PageSettings;
using data::ParagraphAlignment;
using data::ParagraphFormat;
using data::ParagraphInfo;
using data::ParagraphRole;
using data::PlacementMode;
using data::ResourceId;
using data::TextContent;
using data::TextFormatSpan;
using data::TextRange;
using data::TextStyle;
using data::TextStyleOverride;

namespace {

bool finitePositive(qreal value)
{
    return std::isfinite(value) && value > 0;
}

bool finiteNonnegative(qreal value)
{
    return std::isfinite(value) && value >= 0;
}

bool validMargins(const QMarginsF& margins)
{
    return finiteNonnegative(margins.left()) && finiteNonnegative(margins.top()) &&
           finiteNonnegative(margins.right()) && finiteNonnegative(margins.bottom());
}

bool validSize(const QSizeF& size)
{
    return finitePositive(size.width()) && finitePositive(size.height());
}

qreal pageContentWidth(const PageSettings& page)
{
    return page.size.width() - page.margins.left() - page.margins.right();
}

bool validPage(const PageSettings& page)
{
    const auto& margins = page.margins;
    return validSize(page.size) && validMargins(margins) &&
           finitePositive(pageContentWidth(page)) &&
           margins.top() + margins.bottom() < page.size.height();
}

bool validGeometry(const ContainerGeometry& geometry)
{
    return std::isfinite(geometry.position.x()) && std::isfinite(geometry.position.y()) &&
           finiteNonnegative(geometry.width) && finiteNonnegative(geometry.minimumHeight) &&
           validMargins(geometry.padding) && validMargins(geometry.margin);
}

bool usesGeometryWidth(ContainerType type)
{
    return type == ContainerType::Text || type == ContainerType::Group;
}

qreal contentWidth(const ContainerGeometry& geometry, qreal parentWidth)
{
    // 自动框宽扣除左右外边距。内容宽度的计算顺序与排版中的 QRectF::adjusted 一致。
    const qreal frameWidth = geometry.width == 0
        ? parentWidth - geometry.margin.left() - geometry.margin.right()
        : geometry.width;
    return frameWidth - geometry.padding.right() - geometry.padding.left();
}

bool validParagraphFormat(const ParagraphFormat& format)
{
    switch (format.alignment) {
    case ParagraphAlignment::Left:
    case ParagraphAlignment::Center:
    case ParagraphAlignment::Right:
    case ParagraphAlignment::Justify:
        break;
    default:
        return false;
    }
    return std::isfinite(format.lineHeight) && format.lineHeight > 0 &&
           std::isfinite(format.firstLineIndent) && finiteNonnegative(format.leftIndent) &&
           finiteNonnegative(format.rightIndent) && finiteNonnegative(format.spaceBefore) &&
           finiteNonnegative(format.spaceAfter);
}

bool validParagraphRole(ParagraphRole role, int headingLevel)
{
    switch (role) {
    case ParagraphRole::Body:
    case ParagraphRole::Title:
        return true;
    case ParagraphRole::Heading:
        return headingLevel >= 1 && headingLevel <= 9;
    }
    return false;
}

bool rejectNode(const ContainerId& id, const QString& reason, QString* error)
{
    if (error) {
        *error = QStringLiteral("容器 %1：%2").arg(id.isEmpty() ? QStringLiteral("（空 ID）") : id, reason);
    }
    return false;
}

void appendParagraphs(std::vector<ParagraphInfo>& paragraphs, const QString& text,
                      TextRange range, ParagraphInfo paragraph = {})
{
    // 段落范围排除换行符，空正文和末尾换行都要保留空段落。
    int paragraphStart = range.start;
    for (int offset = range.start; offset < range.end; ++offset) {
        if (text.at(offset) == QLatin1Char('\n')) {
            paragraph.range = {paragraphStart, offset};
            paragraphs.push_back(paragraph);
            paragraphStart = offset + 1;
        }
    }
    paragraph.range = {paragraphStart, range.end};
    paragraphs.push_back(std::move(paragraph));
}

std::vector<ParagraphInfo>::iterator paragraphAt(std::vector<ParagraphInfo>& paragraphs, int offset)
{
    return std::lower_bound(paragraphs.begin(), paragraphs.end(), offset,
                            [](const ParagraphInfo& paragraph, int position) {
                                return paragraph.range.end < position;
                            });
}

bool splitsSurrogatePair(const QString& text, int offset)
{
    return offset > 0 && offset < text.size() &&
           text.at(offset - 1).isHighSurrogate() && text.at(offset).isLowSurrogate();
}

bool validUtf16Text(const QString& text, qsizetype maxLength = std::numeric_limits<int>::max())
{
    return text.size() <= maxLength && text.isValidUtf16();
}

bool validTextRange(const QString& text, TextRange range)
{
    return range.start >= 0 && range.end >= range.start && range.end <= text.size() &&
           !splitsSurrogatePair(text, range.start) && !splitsSurrogatePair(text, range.end);
}

bool validTextStyle(const TextStyleOverride& style)
{
    return (!style.fontSizePt || (std::isfinite(*style.fontSizePt) && *style.fontSizePt > 0)) &&
           (!style.color || style.color->isValid());
}

bool validTextStyle(const TextStyle& style)
{
    return std::isfinite(style.fontSizePt) && style.fontSizePt > 0 && style.color.isValid();
}

bool validTextContent(const TextContent& content)
{
    const auto& text = content.text;
    if (!validTextStyle(content.defaultStyle)) {
        return false;
    }

    // 段落范围必须和换行位置一一对应，包括末尾的空段落。
    qsizetype start = 0;
    bool reachedEnd = false;
    for (const auto& paragraph : content.paragraphs) {
        if (reachedEnd) {
            return false;
        }
        const auto newline = text.indexOf(QLatin1Char('\n'), start);
        const auto end = newline < 0 ? text.size() : newline;
        if (paragraph.range.start != start || paragraph.range.end != end ||
            !validParagraphRole(paragraph.role, paragraph.headingLevel) ||
            !validParagraphFormat(paragraph.format) || !validTextStyle(paragraph.defaultStyle)) {
            return false;
        }
        if (newline < 0) {
            reachedEnd = true;
        } else {
            start = newline + 1;
        }
    }
    if (!reachedEnd) {
        return false;
    }

    int previousEnd = 0;
    for (const auto& span : content.formatSpans) {
        if (span.range.start < previousEnd || span.range.start == span.range.end ||
            !validTextRange(text, span.range) || !validTextStyle(span.style)) {
            return false;
        }
        previousEnd = span.range.end;
    }
    return true;
}

bool hasTextStyle(const TextStyleOverride& style)
{
    return style.fontFamily || style.fontSizePt || style.color ||
           style.bold || style.italic || style.underline;
}

template <typename Style>
bool sameTextStyle(const Style& first, const Style& second)
{
    return first.fontFamily == second.fontFamily && first.fontSizePt == second.fontSizePt &&
           first.color == second.color && first.bold == second.bold &&
           first.italic == second.italic && first.underline == second.underline;
}

TextStyleOverride overlayTextStyle(TextStyleOverride base, const TextStyleOverride& style)
{
    if (style.fontFamily) base.fontFamily = style.fontFamily;
    if (style.fontSizePt) base.fontSizePt = style.fontSizePt;
    if (style.color) base.color = style.color;
    if (style.bold) base.bold = style.bold;
    if (style.italic) base.italic = style.italic;
    if (style.underline) base.underline = style.underline;
    return base;
}

void appendFormatSpan(std::vector<TextFormatSpan>& spans, TextRange range,
                      const TextStyleOverride& style)
{
    if (!spans.empty() && spans.back().range.end == range.start && sameTextStyle(spans.back().style, style)) {
        spans.back().range.end = range.end;
    } else {
        spans.push_back({range, style});
    }
}

void appendCharacterFormat(std::vector<TextFormatSpan>& spans, QStringView text, int start,
                           const TextStyleOverride& style, const TextStyleOverride* newlineStyle = nullptr)
{
    int spanStart = start;
    int position = start;
    for (QChar character : text) {
        if (character == QLatin1Char('\n')) {
            if (spanStart < position) {
                appendFormatSpan(spans, {spanStart, position}, style);
            }
            // 新格式跳过换行符。原格式区间已覆盖换行符时，沿用它的设置。
            if (newlineStyle) {
                appendFormatSpan(spans, {position, position + 1}, *newlineStyle);
            }
            spanStart = position + 1;
        }
        ++position;
    }
    if (spanStart < position) {
        appendFormatSpan(spans, {spanStart, position}, style);
    }
}

bool sameFormatSpans(const std::vector<TextFormatSpan>& first, const std::vector<TextFormatSpan>& second)
{
    return std::equal(first.begin(), first.end(), second.begin(), second.end(),
        [](const TextFormatSpan& a, const TextFormatSpan& b) {
            return a.range.start == b.range.start && a.range.end == b.range.end && sameTextStyle(a.style, b.style);
        });
}

template <typename T>
std::optional<T> styleDifference(const T& base, const std::optional<T>& oldOverride,
                                 const std::optional<T>& newOverride)
{
    const T& oldValue = oldOverride ? *oldOverride : base;
    const T& newValue = newOverride ? *newOverride : base;
    if (oldValue == newValue) {
        return std::nullopt;
    }
    return oldValue;
}

TextStyleOverride paragraphStyleDifference(const TextStyle& base, const TextStyleOverride& oldStyle,
                                           const TextStyleOverride& newStyle)
{
    return {
        styleDifference(base.fontFamily, oldStyle.fontFamily, newStyle.fontFamily),
        styleDifference(base.fontSizePt, oldStyle.fontSizePt, newStyle.fontSizePt),
        styleDifference(base.color, oldStyle.color, newStyle.color),
        styleDifference(base.bold, oldStyle.bold, newStyle.bold),
        styleDifference(base.italic, oldStyle.italic, newStyle.italic),
        styleDifference(base.underline, oldStyle.underline, newStyle.underline),
    };
}

enum class StylePriority { Existing, Applied };

bool updateTextStyles(std::vector<TextFormatSpan>& spans, QStringView text, TextRange range,
                      const TextStyleOverride& style, StylePriority priority)
{
    if (range.start == range.end || !hasTextStyle(style)) {
        return false;
    }

    const auto first = std::lower_bound(spans.begin(), spans.end(), range.start,
        [](const TextFormatSpan& span, int start) { return span.range.end <= start; });
    std::vector<TextFormatSpan> updated(spans.begin(), first);
    auto span = first;
    int position = range.start;
    for (; span != spans.end() && span->range.start < range.end; ++span) {
        if (span->range.start < range.start) {
            appendFormatSpan(updated, {span->range.start, range.start}, span->style);
        }
        if (position < span->range.start) {
            appendCharacterFormat(updated, text.sliced(position, span->range.start - position), position, style);
        }

        // 合段补齐继承属性时保留原局部值，主动修改选区时使用传入的值。
        const auto merged = priority == StylePriority::Existing ? overlayTextStyle(style, span->style)
                                                                : overlayTextStyle(span->style, style);
        const int start = std::max(range.start, span->range.start);
        const int end = std::min(range.end, span->range.end);
        appendCharacterFormat(updated, text.sliced(start, end - start), start, merged, &span->style);
        position = end;

        if (span->range.end > range.end) {
            appendFormatSpan(updated, {range.end, span->range.end}, span->style);
        }
    }
    if (position < range.end) {
        appendCharacterFormat(updated, text.sliced(position, range.end - position), position, style);
    }
    if (span != spans.end()) {
        appendFormatSpan(updated, span->range, span->style);
        ++span;
    }
    updated.insert(updated.end(), span, spans.end());
    if (sameFormatSpans(spans, updated)) {
        return false;
    }
    spans.swap(updated);
    return true;
}

}

DocumentNode::DocumentNode(ContainerNode data, DocumentNode* parent)
    : data_(std::move(data)), parent_(parent)
{
}

Document::ImageResources::ImageResources(QHash<ResourceId, QImage> images)
    : imagesById_(std::move(images))
{
    // 外部 QImage 可能引用调用方的像素缓冲区，文档资源保留自己的像素副本。
    for (auto& image : imagesById_) {
        if (!image.isNull()) {
            auto owned = image.copy();
            if (owned.isNull()) {
                throw std::bad_alloc();
            }
            image = std::move(owned);
        }
    }
}

const QHash<ResourceId, QImage>& Document::ImageResources::images() const
{
    return imagesById_;
}

bool Document::ImageResources::hasImage(const ResourceId& id) const
{
    const auto image = imagesById_.constFind(id);
    return image != imagesById_.cend() && !image.value().isNull();
}

std::optional<Document::ImageResources::Import> Document::ImageResources::loadOrReuse(const QString& imagePath)
{
    if (imagePath.isEmpty()) {
        return std::nullopt;
    }

    const QFileInfo file(imagePath);
    const QString canonicalPath = file.canonicalFilePath();
    const QString path = canonicalPath.isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : canonicalPath;
    const auto existing = idsByPath_.constFind(path);
    if (existing != idsByPath_.cend()) {
        return Import{existing.value(), {}};
    }

    // 资源表中没有这个路径时，读取图片并登记新的资源 ID。
    if (!file.isFile()) {
        return std::nullopt;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        return std::nullopt;
    }

    ResourceId id;
    do {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (imagesById_.contains(id));

    const auto entry = imagesById_.insert(id, std::move(image));
    try {
        idsByPath_.insert(path, id);
    } catch (...) {
        imagesById_.erase(entry);
        throw;
    }
    return Import{std::move(id), path};
}

void Document::ImageResources::rollbackImport(const Import& image)
{
    if (!image.addedPath.isEmpty()) {
        idsByPath_.remove(image.addedPath);
        imagesById_.remove(image.id);
    }
}

Document::Document(QHash<ResourceId, QImage> resources)
    : root_(std::make_unique<DocumentNode>(
          ContainerNode{QStringLiteral("root"), ContainerType::Group}, nullptr)),
      resources_(std::move(resources))
{
    nodesById_.insert(root_->data_.id, root_.get());
}

DocumentSnapshot Document::toSnapshot() const
{
    return {revision_, page_, resources_.images(), exportNode(*root_)};
}

std::optional<Document> Document::fromSnapshot(const DocumentSnapshot& snapshot, QString* error)
{
    if (error) {
        error->clear();
    }
    if (snapshot.root.type != ContainerType::Group) {
        rejectNode(snapshot.root.id, QStringLiteral("逻辑根必须是分组"), error);
        return std::nullopt;
    }
    if (!validPage(snapshot.page)) {
        if (error) {
            *error = QStringLiteral("页面尺寸或页边距无效");
        }
        return std::nullopt;
    }

    // 在新文档中重建整棵树。中途遇到非法节点时，丢弃这份文档。
    Document document(snapshot.resources);
    document.nodesById_.clear();
    if (!document.importNode(snapshot.root, nullptr, document.root_, pageContentWidth(snapshot.page), error)) {
        return std::nullopt;
    }
    document.page_ = snapshot.page;
    document.revision_ = snapshot.revision;
    return document;
}

std::optional<Document::NodeInfo> Document::queryNode(const ContainerId& id) const
{
    const DocumentNode* const node = findNode(id);
    if (node == nullptr) {
        return std::nullopt;
    }

    NodeInfo info{node->data_, std::nullopt, {}};
    if (node->parent_ != nullptr) {
        info.parentId = node->parent_->data_.id;
    }
    info.childIds.reserve(node->children_.size());
    for (const auto& child : node->children_) {
        info.childIds.push_back(child->data_.id);
    }
    return info;
}

ContainerNode Document::exportNode(const DocumentNode& node)
{
    auto result = node.data_;
    result.children.reserve(node.children_.size());
    for (const auto& child : node.children_) {
        result.children.push_back(exportNode(*child));
    }
    return result;
}

bool Document::importNode(const ContainerNode& source, DocumentNode* parent,
                          std::unique_ptr<DocumentNode>& destination, qreal parentWidth, QString* error)
{
    if (source.id.isEmpty() || nodesById_.contains(source.id)) {
        return rejectNode(source.id, QStringLiteral("ID 为空或重复"), error);
    }
    if (!validGeometry(source.geometry)) {
        return rejectNode(source.id, QStringLiteral("几何属性无效"), error);
    }
    // 逻辑根直接沿用页面正文宽度，保留快照中已有的根节点几何校验。
    qreal width = parentWidth;
    if (usesGeometryWidth(source.type) && (parent != nullptr || source.geometry.width > 0)) {
        const qreal nodeWidth = contentWidth(source.geometry, parentWidth);
        if (!finitePositive(nodeWidth)) {
            return rejectNode(source.id, QStringLiteral("内容宽度必须为有限的正数，请检查框宽和左右边距"), error);
        }
        if (parent != nullptr) {
            width = nodeWidth;
        }
    }
    if (parent != nullptr) {
        const auto placement = placementForChild(parent);
        if (!placement || source.placement != *placement) {
            return rejectNode(source.id, QStringLiteral("容器层级或排版方式无效"), error);
        }
    }

    switch (source.type) {
    case ContainerType::Text:
        if (!validUtf16Text(source.text.text)) {
            return rejectNode(source.id, QStringLiteral("正文含有无效 UTF-16 或超过长度上限"), error);
        }
        if (!validTextContent(source.text)) {
            return rejectNode(source.id, QStringLiteral("段落或字符格式无效"), error);
        }
        break;
    case ContainerType::Image:
        if (!resources_.hasImage(source.image.resourceId)) {
            return rejectNode(source.id, QStringLiteral("图片资源缺失或无效"), error);
        }
        if (!validSize(source.image.displaySize) || !source.children.empty()) {
            return rejectNode(source.id, QStringLiteral("图片显示尺寸无效或包含子节点"), error);
        }
        break;
    case ContainerType::Group:
        break;
    default:
        return rejectNode(source.id, QStringLiteral("容器类型无效"), error);
    }

    // 编辑树用 children_ 持有子节点，这里只复制单个节点的数据。
    destination = std::make_unique<DocumentNode>(
        ContainerNode{source.id, source.type, source.placement, source.geometry, source.text, source.image, {}}, parent);
    DocumentNode* const node = destination.get();
    nodesById_.insert(source.id, node);
    node->children_.reserve(source.children.size());
    for (const auto& child : source.children) {
        node->children_.emplace_back();
        if (!importNode(child, node, node->children_.back(), width, error)) {
            return false;
        }
    }
    return true;
}

std::optional<ContainerId> Document::addText(const ContainerId& parentId,
                                             QString initialText)
{
    if (!validUtf16Text(initialText)) {
        return std::nullopt;
    }

    ContainerNode node{};
    node.type = ContainerType::Text;
    node.text.text = std::move(initialText);
    appendParagraphs(node.text.paragraphs, node.text.text, {0, static_cast<int>(node.text.text.size())});

    return addNode(parentId, std::move(node));
}

std::optional<ContainerId> Document::addImage(const ContainerId& parentId,
                                             const ResourceId& resourceId, const QSizeF& displaySize)
{
    if (!validSize(displaySize) || !resources_.hasImage(resourceId)) {
        return std::nullopt;
    }

    ContainerNode node{};
    node.type = ContainerType::Image;
    node.image = {resourceId, displaySize};
    return addNode(parentId, std::move(node));
}

std::optional<ContainerId> Document::addImageFromFile(const ContainerId& parentId,
                                                     const QString& imagePath, const QSizeF& displaySize)
{
    if (!validSize(displaySize) || !placementForChild(findNode(parentId))) {
        return std::nullopt;
    }

    const auto resource = resources_.loadOrReuse(imagePath);
    if (!resource) {
        // 未登记过的路径会尝试导入，路径无效或图片读取失败才会走到这里。
        return std::nullopt;
    }
    // 节点创建失败时，只撤销本次新导入的资源。
    auto rollback = qScopeGuard([&] { resources_.rollbackImport(*resource); });

    auto id = addImage(parentId, resource->id, displaySize);
    if (id) {
        rollback.dismiss();
    }
    return id;
}

bool Document::replaceImage(const ContainerId& id, const ResourceId& resourceId)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Image || !resources_.hasImage(resourceId)) {
        return false;
    }

    setImageResource(*node, resourceId);
    return true;
}

bool Document::replaceImageFromFile(const ContainerId& id, const QString& imagePath)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Image) {
        return false;
    }

    const auto resource = resources_.loadOrReuse(imagePath);
    if (!resource) {
        return false;
    }
    setImageResource(*node, resource->id);
    return true;
}

void Document::setImageResource(DocumentNode& node, const ResourceId& resourceId)
{
    // 只更新当前节点的引用，其他容器和旧快照仍可使用原资源。
    if (node.data_.image.resourceId != resourceId) {
        node.data_.image.resourceId = resourceId;
        ++revision_;
    }
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
    if (!sameParent && usesGeometryWidth(node->data_.type) &&
        !finitePositive(contentWidth(node->data_.geometry, parentContentWidth(*targetParent)))) {
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

bool Document::removeNode(const ContainerId& id)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node == root_.get()) {
        return false;
    }

    auto& siblings = node->parent_->children_;
    const auto entry = std::find_if(siblings.begin(), siblings.end(),
                                    [node](const auto& child) { return child.get() == node; });
    // 先清掉整棵子树的索引，再由 unique_ptr 销毁节点，避免索引中留下悬空指针。
    removeSubtreeFromIndex(*node);
    siblings.erase(entry);
    ++revision_;
    return true;
}

bool Document::setContainerGeometry(const ContainerId& id, const ContainerGeometry& geometry)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node == root_.get()) {
        return false;
    }

    if (!validGeometry(geometry)) {
        return false;
    }
    if (usesGeometryWidth(node->data_.type)) {
        const qreal width = contentWidth(geometry, parentContentWidth(*node->parent_));
        if (!finitePositive(width)) {
            return false;
        }
        // v0.1 只有一层子节点。用更新后的父内容宽度检查直接子节点。
        for (const auto& child : node->children_) {
            if (usesGeometryWidth(child->data_.type) && !finitePositive(contentWidth(child->data_.geometry, width))) {
                return false;
            }
        }
    }

    // 坐标和边距逐项比较,不一致时才修改，一致时返回true但不执行修改
    const auto sameMargins = [](const QMarginsF& first, const QMarginsF& second) {
        return first.left() == second.left() && first.top() == second.top() &&
               first.right() == second.right() && first.bottom() == second.bottom();
    };
    auto& current = node->data_.geometry;
    if (current.position.x() == geometry.position.x() && current.position.y() == geometry.position.y() &&
        current.width == geometry.width && current.minimumHeight == geometry.minimumHeight &&
        sameMargins(current.padding, geometry.padding) && sameMargins(current.margin, geometry.margin)) {
        return true;
    }

    current = geometry;
    ++revision_;
    return true;
}

bool Document::replaceText(const ContainerId& id, TextRange range, const QString& replacement)
{
    return replaceTextImpl(id, range, replacement, {}, false);
}

bool Document::replaceText(const ContainerId& id, TextRange range, const QString& replacement,
                           const std::vector<TextFormatSpan>& replacementFormats)
{
    return replaceTextImpl(id, range, replacement, replacementFormats, true);
}

bool Document::replaceTextImpl(const ContainerId& id, TextRange range, const QString& replacement,
                               const std::vector<TextFormatSpan>& replacementFormats, bool replaceFormatting)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Text) {
        return false;
    }

    //检查边界合理
    auto& content = node->data_.text;
    if (!validTextRange(content.text, range)) {
        return false;
    }
    const int removedSize = range.end - range.start;
    const auto remainingSize = content.text.size() - removedSize;
    if (!validUtf16Text(replacement, std::numeric_limits<int>::max() - remainingSize)) {
        return false;
    }
    int previousEnd = 0;
    for (const auto& span : replacementFormats) {
        if (span.range.start < previousEnd || span.range.start == span.range.end ||
            !validTextRange(replacement, span.range) || !validTextStyle(span.style)) {
            return false;
        }
        previousEnd = span.range.end;
    }

    const bool sameText = QStringView(content.text).mid(range.start, removedSize) == QStringView(replacement);
    if (sameText && !replaceFormatting) {
        return true;
    }

    const int insertedSize = static_cast<int>(replacement.size());
    const int delta = insertedSize - removedSize;
    const int suffixStart = range.start + insertedSize;
    // 保留选区两侧的格式。替换文字的格式区间先换算到正文坐标，再接回右侧。
    std::vector<TextFormatSpan> formatSpans;
    formatSpans.reserve(content.formatSpans.size() + replacementFormats.size());
    auto span = content.formatSpans.begin();
    for (; span != content.formatSpans.end() && span->range.start < range.start; ++span) {
        appendFormatSpan(formatSpans, {span->range.start, std::min(span->range.end, range.start)}, span->style);
        if (span->range.end > range.end) {
            // 一段格式跨过整个选区时，后面的循环还要保留它的右半段。
            break;
        }
    }
    for (const auto& replacementSpan : replacementFormats) {
        if (hasTextStyle(replacementSpan.style)) {
            const auto& localRange = replacementSpan.range;
            const auto characters = QStringView(replacement).sliced(localRange.start, localRange.end - localRange.start);
            appendCharacterFormat(formatSpans, characters, range.start + localRange.start, replacementSpan.style);
        }
    }
    for (; span != content.formatSpans.end(); ++span) {
        if (span->range.end > range.end) {
            const int start = std::max(span->range.start, range.end) + delta;
            appendFormatSpan(formatSpans, {start, span->range.end + delta}, span->style);
        }
    }

    if (sameText && sameFormatSpans(content.formatSpans, formatSpans)) {
        return true;
    }
    if (!sameText) {
        QString updatedText = content.text;
        updatedText.replace(range.start, removedSize, replacement);

        const auto first = paragraphAt(content.paragraphs, range.start);
        const auto last = paragraphAt(content.paragraphs, range.end);
        std::vector<ParagraphInfo> paragraphs(content.paragraphs.begin(), first);
        // 拆段和合段沿用起始段的角色和格式。右段的剩余内容单独成段时，保留右段的属性。
        appendParagraphs(paragraphs, updatedText, {first->range.start, last->range.end + delta}, *first);
        const bool separateSuffix = first != last && paragraphs.back().range.start == suffixStart &&
            (range.end < last->range.end || range.end == last->range.start);
        if (separateSuffix) {
            const TextRange suffixRange = paragraphs.back().range;
            paragraphs.back() = *last;
            paragraphs.back().range = suffixRange;
        }
        for (auto it = last + 1; it != content.paragraphs.end(); ++it) {
            ParagraphInfo paragraph = *it;
            paragraph.range.start += delta;
            paragraph.range.end += delta;
            paragraphs.push_back(std::move(paragraph));
        }

        if (first != last && !separateSuffix && range.end < last->range.end) {
            // 右段文字合入左段后，把原段落的字体差异补到保留的文字范围上。
            const auto difference = paragraphStyleDifference(content.defaultStyle, last->defaultStyle,
                                                             first->defaultStyle);
            updateTextStyles(formatSpans, updatedText, {suffixStart, last->range.end + delta},
                             difference, StylePriority::Existing);
        }

        // 正文和范围都准备好后一起写入，避免留下只更新了一部分的节点。
        content.text.swap(updatedText);
        content.paragraphs.swap(paragraphs);
    }

    content.formatSpans.swap(formatSpans);
    ++revision_;
    return true;
}

bool Document::applyTextStyle(const ContainerId& id, TextRange range, const TextStyleOverride& style)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Text ||
        !validTextRange(node->data_.text.text, range) || !validTextStyle(style)) {
        return false;
    }
    auto& content = node->data_.text;
    if (updateTextStyles(content.formatSpans, content.text, range, style, StylePriority::Applied)) {
        ++revision_;
    }
    return true;
}

bool Document::setTextDefaultStyle(const ContainerId& id, const TextStyle& style)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Text || !validTextStyle(style)) {
        return false;
    }

    auto& current = node->data_.text.defaultStyle;
    if (!sameTextStyle(current, style)) {
        current = style;
        ++revision_;
    }
    return true;
}

bool Document::setParagraphFormat(const ContainerId& id, int textOffset, const ParagraphFormat& format,
                                  const TextStyleOverride& defaultStyle)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Text || textOffset < 0 ||
        !validParagraphFormat(format) || !validTextStyle(defaultStyle)) {
        return false;
    }

    auto& paragraphs = node->data_.text.paragraphs;
    const auto paragraph = paragraphAt(paragraphs, textOffset);
    if (paragraph == paragraphs.end()) {
        return false;
    }

    const auto& current = paragraph->format;
    if (current.alignment == format.alignment && current.leftIndent == format.leftIndent &&
        current.rightIndent == format.rightIndent && current.firstLineIndent == format.firstLineIndent &&
        current.lineHeight == format.lineHeight && current.spaceBefore == format.spaceBefore &&
        current.spaceAfter == format.spaceAfter && sameTextStyle(paragraph->defaultStyle, defaultStyle)) {
        return true;
    }

    paragraph->format = format;
    paragraph->defaultStyle = defaultStyle;
    ++revision_;
    return true;
}

bool Document::setParagraphRole(const ContainerId& id, int textOffset, ParagraphRole role, int headingLevel)
{
    DocumentNode* const node = findNode(id);
    if (node == nullptr || node->data_.type != ContainerType::Text || textOffset < 0) {
        return false;
    }

    auto& paragraphs = node->data_.text.paragraphs;
    const auto paragraph = paragraphAt(paragraphs, textOffset);
    if (paragraph == paragraphs.end() || !validParagraphRole(role, headingLevel)) {
        return false;
    }

    if (role != ParagraphRole::Heading) {
        headingLevel = 1;
    }

    if (paragraph->role == role && paragraph->headingLevel == headingLevel) {
        return true;
    }

    paragraph->role = role;
    paragraph->headingLevel = headingLevel;
    ++revision_;
    return true;
}

DocumentNode* Document::findNode(const ContainerId& id) const
{
    return nodesById_.value(id, nullptr);
}

qreal Document::parentContentWidth(const DocumentNode& parent) const
{
    // 允许接收子节点的父容器只有逻辑根和顶层容器。
    const qreal width = pageContentWidth(page_);
    return &parent == root_.get() ? width : contentWidth(parent.data_.geometry, width);
}

void Document::removeSubtreeFromIndex(const DocumentNode& node)
{
    nodesById_.remove(node.data_.id);
    for (const auto& child : node.children_) {
        removeSubtreeFromIndex(*child);
    }
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
    node.id = id;
    node.placement = *placement;

    auto& children = parent->children_;
    children.push_back(std::make_unique<DocumentNode>(std::move(node), parent));
    try {
        nodesById_.insert(id, children.back().get());
    } catch (...) {
        children.pop_back();
        throw;
    }
    ++revision_;
    return id;
}

ContainerId Document::allocateId() const
{
    // 新节点使用 UUID，避免依赖全局配置。导入时保留快照里的 ID。
    ContainerId id;
    do {
        id = QStringLiteral("node-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (nodesById_.contains(id));
    return id;
}

}
