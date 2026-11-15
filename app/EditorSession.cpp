#include "EditorSession.h"

#include "DocxExporter.h"
#include "InteractionController.h"
#include "LayoutModule.h"
#include "Selection.h"
#include "TextBoundary.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <algorithm>
#include <utility>

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
    setLayoutSnapshot(document.toSnapshot());
    state_ = interaction::makeInitialState(snapshot_, layout_, textId);
    document_ = std::move(document);
    importedImages_.clear();
    emit changed(true);
}

void EditorSession::setLayoutSnapshot(data::DocumentSnapshot snapshot)
{
    auto newLayout = layout::composeLayout(snapshot);
    snapshot_ = std::move(snapshot);
    layout_ = std::move(newLayout);
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
        mergeStyle(style, paragraph->defaultStyle);
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
    auto state = interaction::makeInitialState(snapshot_, layout_, focus.containerId, end);
    if (start != end) {
        state.selection = interaction::makeSelection(layout_, focus.containerId, {start, end});
        state.selectionAnchor = start;
    }
    state_ = std::move(state);
    emit changed(false);
}

bool EditorSession::key(const interaction::KeyInput& key, const QString& clipboardText)
{
    return accept(interaction::applyKey(state_, key, snapshot_, layout_, clipboardText));
}

bool EditorSession::ime(const interaction::ImeInput& ime)
{
    return accept(interaction::applyIme(state_, ime, snapshot_, layout_));
}

bool EditorSession::action(const interaction::EditAction& action)
{
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
            setLayoutSnapshot(std::move(snapshot));
            if (staged)
                document_ = std::move(*staged);
        }
    }
    state_ = interaction::refreshStateGeometry(output.state, layout_);
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
