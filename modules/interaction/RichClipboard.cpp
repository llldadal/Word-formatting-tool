#include "RichClipboard.h"
#include "TextBoundary.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

namespace editor::interaction {
namespace {

data::TextStyle mergeStyle(data::TextStyle base, const data::TextStyleOverride& change)
{
    if (change.fontFamily.has_value())
        base.fontFamily = *change.fontFamily;
    if (change.fontSizePt.has_value())
        base.fontSizePt = *change.fontSizePt;
    if (change.color.has_value())
        base.color = *change.color;
    if (change.bold.has_value())
        base.bold = *change.bold;
    if (change.italic.has_value())
        base.italic = *change.italic;
    if (change.underline.has_value())
        base.underline = *change.underline;
    return base;
}

// 与 LayoutModule::resolveParagraphStyle 保持完全一致的角色默认字号与粗体规则，
// 避免 interaction 模块反向依赖 EditorLayout。
data::TextStyle resolveEffectiveParagraphStyle(data::TextStyle base, const data::ParagraphInfo& paragraph)
{
    switch (paragraph.role) {
    case data::ParagraphRole::Body:
        break;
    case data::ParagraphRole::Title:
        base.fontSizePt = 19.5;
        base.bold = true;
        break;
    case data::ParagraphRole::Heading: {
        static constexpr qreal kHeadingSizes[] = {18, 16.5, 15, 14, 13, 12, 12, 12, 12};
        base.fontSizePt = kHeadingSizes[qBound(1, paragraph.headingLevel, 9) - 1];
        base.bold = true;
        break;
    }
    }
    return mergeStyle(base, paragraph.defaultStyle);
}

const data::ParagraphInfo* findParagraphContaining(const data::TextContent& content, int offset)
{
    const auto it = std::upper_bound(content.paragraphs.begin(), content.paragraphs.end(), offset,
        [](int pos, const data::ParagraphInfo& info) { return pos < info.range.start; });
    if (it == content.paragraphs.begin())
        return nullptr;
    const auto& cand = *(it - 1);
    return offset <= cand.range.end ? &cand : nullptr;
}

data::TextStyle resolveStyleAtOffset(const data::TextContent& content,
                                     const data::ParagraphInfo* paragraph,
                                     int offset)
{
    data::TextStyle style = content.defaultStyle;
    if (paragraph) {
        style = resolveEffectiveParagraphStyle(style, *paragraph);
    }
    const auto span = std::lower_bound(content.formatSpans.begin(), content.formatSpans.end(), offset,
        [](const data::TextFormatSpan& entry, int pos) { return entry.range.end <= pos; });
    if (span != content.formatSpans.end() && span->range.start <= offset) {
        style = mergeStyle(style, span->style);
    }
    return style;
}

bool sameResolvedStyle(const data::TextStyleOverride& a, const data::TextStyleOverride& b)
{
    return a.fontFamily == b.fontFamily &&
           a.fontSizePt.has_value() == b.fontSizePt.has_value() &&
           (!a.fontSizePt || std::abs(*a.fontSizePt - *b.fontSizePt) < 1e-4) &&
           a.color == b.color &&
           a.bold == b.bold &&
           a.italic == b.italic &&
           a.underline == b.underline;
}

data::TextStyleOverride toFullOverride(const data::TextStyle& s)
{
    data::TextStyleOverride ov;
    ov.fontFamily = s.fontFamily;
    ov.fontSizePt = s.fontSizePt;
    ov.color = s.color;
    ov.bold = s.bold;
    ov.italic = s.italic;
    ov.underline = s.underline;
    return ov;
}

bool findImageNode(const data::ContainerNode& node,
                   const data::ContainerId& id,
                   const data::ContainerId& parentId,
                   const data::ContainerNode*& outNode,
                   data::ContainerId& outParentId)
{
    if (node.id == id) {
        outNode = &node;
        outParentId = parentId;
        return true;
    }
    for (const auto& child : node.children) {
        if (findImageNode(child, id, node.id, outNode, outParentId))
            return true;
    }
    return false;
}

std::vector<const data::ParagraphInfo*> overlappedParagraphs(const data::TextContent& content,
                                                             data::TextRange focusRange)
{
    const int len = static_cast<int>(content.text.size());
    const int rStart = qBound(0, focusRange.start, len);
    const int rEnd = qBound(rStart, focusRange.end, len);

    std::vector<const data::ParagraphInfo*> result;
    if (rStart >= rEnd) {
        if (const auto* para = findParagraphContaining(content, rStart)) {
            result.push_back(para);
        }
        return result;
    }

    for (const auto& para : content.paragraphs) {
        const int pStart = qBound(0, para.range.start, len);
        const int pEnd = qBound(pStart, para.range.end, len);
        const bool overlaps = (pStart == pEnd)
            ? (rStart <= pStart && pStart <= rEnd)
            : (pStart < rEnd && pEnd > rStart);
        if (overlaps) {
            result.push_back(&para);
        }
    }
    return result;
}

QJsonObject encodeStyleOverride(const data::TextStyleOverride& style)
{
    QJsonObject obj;
    if (style.fontFamily)
        obj.insert(QStringLiteral("fontFamily"), *style.fontFamily);
    if (style.fontSizePt)
        obj.insert(QStringLiteral("fontSizePt"), *style.fontSizePt);
    if (style.color)
        obj.insert(QStringLiteral("color"), style.color->name(QColor::HexArgb));
    if (style.bold)
        obj.insert(QStringLiteral("bold"), *style.bold);
    if (style.italic)
        obj.insert(QStringLiteral("italic"), *style.italic);
    if (style.underline)
        obj.insert(QStringLiteral("underline"), *style.underline);
    return obj;
}

std::optional<data::TextStyleOverride> decodeStyleOverride(const QJsonObject& obj)
{
    data::TextStyleOverride style;
    if (obj.contains(QStringLiteral("fontFamily"))) {
        if (!obj.value(QStringLiteral("fontFamily")).isString())
            return std::nullopt;
        style.fontFamily = obj.value(QStringLiteral("fontFamily")).toString();
    }
    if (obj.contains(QStringLiteral("fontSizePt"))) {
        if (!obj.value(QStringLiteral("fontSizePt")).isDouble())
            return std::nullopt;
        const qreal pt = obj.value(QStringLiteral("fontSizePt")).toDouble();
        if (!std::isfinite(pt) || pt <= 0)
            return std::nullopt;
        style.fontSizePt = pt;
    }
    if (obj.contains(QStringLiteral("color"))) {
        const QColor c(obj.value(QStringLiteral("color")).toString());
        if (!c.isValid())
            return std::nullopt;
        style.color = c;
    }
    if (obj.contains(QStringLiteral("bold")))
        style.bold = obj.value(QStringLiteral("bold")).toBool();
    if (obj.contains(QStringLiteral("italic")))
        style.italic = obj.value(QStringLiteral("italic")).toBool();
    if (obj.contains(QStringLiteral("underline")))
        style.underline = obj.value(QStringLiteral("underline")).toBool();
    return style;
}

QString roleToString(data::ParagraphRole role)
{
    switch (role) {
    case data::ParagraphRole::Title:
        return QStringLiteral("Title");
    case data::ParagraphRole::Heading:
        return QStringLiteral("Heading");
    case data::ParagraphRole::Body:
    default:
        return QStringLiteral("Body");
    }
}

data::ParagraphRole roleFromString(const QString& str)
{
    if (str == QStringLiteral("Title"))
        return data::ParagraphRole::Title;
    if (str == QStringLiteral("Heading"))
        return data::ParagraphRole::Heading;
    return data::ParagraphRole::Body;
}

QString alignmentToString(data::ParagraphAlignment align)
{
    switch (align) {
    case data::ParagraphAlignment::Center:
        return QStringLiteral("Center");
    case data::ParagraphAlignment::Right:
        return QStringLiteral("Right");
    case data::ParagraphAlignment::Justify:
        return QStringLiteral("Justify");
    case data::ParagraphAlignment::Left:
    default:
        return QStringLiteral("Left");
    }
}

data::ParagraphAlignment alignmentFromString(const QString& str)
{
    if (str == QStringLiteral("Center"))
        return data::ParagraphAlignment::Center;
    if (str == QStringLiteral("Right"))
        return data::ParagraphAlignment::Right;
    if (str == QStringLiteral("Justify"))
        return data::ParagraphAlignment::Justify;
    return data::ParagraphAlignment::Left;
}

QJsonObject encodeMargins(const QMarginsF& m)
{
    QJsonObject o;
    o.insert(QStringLiteral("l"), m.left());
    o.insert(QStringLiteral("t"), m.top());
    o.insert(QStringLiteral("r"), m.right());
    o.insert(QStringLiteral("b"), m.bottom());
    return o;
}

QMarginsF decodeMargins(const QJsonObject& o)
{
    return QMarginsF(
        qMax<qreal>(0, o.value(QStringLiteral("l")).toDouble()),
        qMax<qreal>(0, o.value(QStringLiteral("t")).toDouble()),
        qMax<qreal>(0, o.value(QStringLiteral("r")).toDouble()),
        qMax<qreal>(0, o.value(QStringLiteral("b")).toDouble()));
}

} // namespace

SelectionStyleSummary inspectSelectionStyle(const data::DocumentSnapshot& doc,
                                            const data::ContainerId& id,
                                            data::TextRange range)
{
    SelectionStyleSummary summary;
    const auto* content = findTextContent(doc, id);
    if (!content)
        return summary;

    const int len = static_cast<int>(content->text.size());
    const int start = qBound(0, range.start, len);
    const int end = qBound(0, range.end, len);

    if (start >= end) {
        const auto* para = findParagraphContaining(*content, start);
        int charPos = start;
        if (len == 0) {
            const auto style = resolveStyleAtOffset(*content, para, 0);
            summary.valid = true;
            summary.fontFamily = style.fontFamily;
            summary.fontSizePt = style.fontSizePt;
            summary.color = style.color;
            summary.bold = style.bold;
            summary.italic = style.italic;
            summary.underline = style.underline;
            return summary;
        }
        if (para && charPos == para->range.end && charPos > para->range.start) {
            charPos = para->range.end - 1;
        } else if (charPos >= len) {
            charPos = len - 1;
        }
        if (!para)
            para = findParagraphContaining(*content, charPos);
        const auto style = resolveStyleAtOffset(*content, para, charPos);
        summary.valid = true;
        summary.fontFamily = style.fontFamily;
        summary.fontSizePt = style.fontSizePt;
        summary.color = style.color;
        summary.bold = style.bold;
        summary.italic = style.italic;
        summary.underline = style.underline;
        return summary;
    }

    const int rStart = start;
    const int rEnd = end;
    bool initialized = false;
    auto accumulate = [&](const data::TextStyle& s) {
        if (!initialized) {
            initialized = true;
            summary.valid = true;
            summary.fontFamily = s.fontFamily;
            summary.fontSizePt = s.fontSizePt;
            summary.color = s.color;
            summary.bold = s.bold;
            summary.italic = s.italic;
            summary.underline = s.underline;
            return;
        }
        if (summary.fontFamily && *summary.fontFamily != s.fontFamily)
            summary.fontFamily.reset();
        if (summary.fontSizePt && std::abs(*summary.fontSizePt - s.fontSizePt) > 1e-4)
            summary.fontSizePt.reset();
        if (summary.color && *summary.color != s.color)
            summary.color.reset();
        if (summary.bold && *summary.bold != s.bold)
            summary.bold.reset();
        if (summary.italic && *summary.italic != s.italic)
            summary.italic.reset();
        if (summary.underline && *summary.underline != s.underline)
            summary.underline.reset();
    };

    for (const auto& para : content->paragraphs) {
        const int segStart = qMax(rStart, para.range.start);
        const int segEnd = qMin(rEnd, para.range.end);
        if (segStart >= segEnd)
            continue;

        const data::TextStyle paraBase = resolveEffectiveParagraphStyle(content->defaultStyle, para);
        int pos = segStart;
        auto spanIt = std::lower_bound(content->formatSpans.begin(), content->formatSpans.end(), pos,
            [](const data::TextFormatSpan& entry, int p) { return entry.range.end <= p; });
        for (; spanIt != content->formatSpans.end() && spanIt->range.start < segEnd; ++spanIt) {
            if (pos < spanIt->range.start) {
                accumulate(paraBase);
            }
            const int runEnd = qMin(segEnd, spanIt->range.end);
            if (qMax(pos, spanIt->range.start) < runEnd) {
                accumulate(mergeStyle(paraBase, spanIt->style));
            }
            pos = runEnd;
        }
        if (pos < segEnd) {
            accumulate(paraBase);
        }
    }

    if (!initialized) {
        // 仅选中换行符时的兜底样式
        const auto* para = findParagraphContaining(*content, rStart);
        accumulate(resolveStyleAtOffset(*content, para, rStart));
    }

    return summary;
}

ParagraphSelectionSummary inspectParagraphSelection(const data::DocumentSnapshot& doc,
                                                    const data::ContainerId& id,
                                                    data::TextRange focusRange)
{
    ParagraphSelectionSummary summary;
    const auto* content = findTextContent(doc, id);
    if (!content)
        return summary;

    const auto paras = overlappedParagraphs(*content, focusRange);
    if (paras.empty())
        return summary;

    summary.valid = true;
    summary.alignment = paras.front()->format.alignment;
    summary.role = paras.front()->role;
    if (*summary.role == data::ParagraphRole::Heading) {
        summary.headingLevel = paras.front()->headingLevel;
    }

    for (std::size_t i = 1; i < paras.size(); ++i) {
        const auto* p = paras[i];
        if (summary.alignment && *summary.alignment != p->format.alignment)
            summary.alignment.reset();
        if (summary.role && *summary.role != p->role) {
            summary.role.reset();
            summary.headingLevel.reset();
        }
        if (summary.headingLevel && p->role == data::ParagraphRole::Heading &&
            *summary.headingLevel != p->headingLevel) {
            summary.headingLevel.reset();
        }
    }

    return summary;
}

std::optional<RichTextClipboardPayload> extractRichTextPayload(const data::DocumentSnapshot& doc,
                                                               const data::ContainerId& id,
                                                               data::TextRange range)
{
    const auto* content = findTextContent(doc, id);
    if (!content)
        return std::nullopt;

    const int len = static_cast<int>(content->text.size());
    const int rStart = qBound(0, range.start, len);
    const int rEnd = qBound(0, range.end, len);
    if (rStart >= rEnd)
        return std::nullopt;

    RichTextClipboardPayload payload;
    payload.plainText = content->text.mid(rStart, rEnd - rStart);

    auto appendSpan = [&](int docStart, int docEnd, const data::TextStyle& resolved) {
        if (docStart >= docEnd)
            return;
        const int relStart = docStart - rStart;
        const int relEnd = docEnd - rStart;
        const auto ov = toFullOverride(resolved);
        if (!payload.formatSpans.empty() &&
            payload.formatSpans.back().range.end == relStart &&
            sameResolvedStyle(payload.formatSpans.back().style, ov)) {
            payload.formatSpans.back().range.end = relEnd;
        } else {
            payload.formatSpans.push_back({{relStart, relEnd}, ov});
        }
    };

    const int textLen = static_cast<int>(content->text.size());
    int segStart = rStart;
    while (segStart <= rEnd) {
        int nlPos = content->text.indexOf(QLatin1Char('\n'), segStart);
        const int segEnd = (nlPos < 0 || nlPos > rEnd) ? rEnd : nlPos;
        const int queryOffset = qMin(segStart, textLen);
        const auto* para = findParagraphContaining(*content, queryOffset);

        CopiedParagraphMeta meta;
        data::TextStyle paraBase = content->defaultStyle;
        if (para) {
            meta.role = para->role;
            meta.headingLevel = para->headingLevel;
            meta.format = para->format;
            meta.defaultStyle = para->defaultStyle;
            meta.fullParagraph = (rStart <= para->range.start && rEnd >= para->range.end);
            paraBase = resolveEffectiveParagraphStyle(content->defaultStyle, *para);
        }
        payload.paragraphs.push_back(std::move(meta));

        if (segStart < segEnd) {
            int pos = segStart;
            auto spanIt = std::lower_bound(content->formatSpans.begin(), content->formatSpans.end(), pos,
                [](const data::TextFormatSpan& entry, int p) { return entry.range.end <= p; });
            for (; spanIt != content->formatSpans.end() && spanIt->range.start < segEnd; ++spanIt) {
                if (pos < spanIt->range.start) {
                    appendSpan(pos, qMin(segEnd, spanIt->range.start), paraBase);
                }
                const int runStart = qMax(pos, spanIt->range.start);
                const int runEnd = qMin(segEnd, spanIt->range.end);
                if (runStart < runEnd) {
                    appendSpan(runStart, runEnd, mergeStyle(paraBase, spanIt->style));
                }
                pos = runEnd;
            }
            if (pos < segEnd) {
                appendSpan(pos, segEnd, paraBase);
            }
        }

        if (nlPos < 0 || nlPos >= rEnd) {
            break;
        }
        segStart = nlPos + 1;
    }

    return payload;
}

QByteArray encodeRichTextPayload(const RichTextClipboardPayload& payload)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("plainText"), payload.plainText);

    QJsonArray spansArr;
    for (const auto& span : payload.formatSpans) {
        QJsonObject sObj;
        sObj.insert(QStringLiteral("start"), span.range.start);
        sObj.insert(QStringLiteral("end"), span.range.end);
        sObj.insert(QStringLiteral("style"), encodeStyleOverride(span.style));
        spansArr.append(sObj);
    }
    root.insert(QStringLiteral("formatSpans"), spansArr);

    QJsonArray paraArr;
    for (const auto& meta : payload.paragraphs) {
        QJsonObject pObj;
        pObj.insert(QStringLiteral("role"), roleToString(meta.role));
        pObj.insert(QStringLiteral("headingLevel"), meta.headingLevel);
        pObj.insert(QStringLiteral("fullParagraph"), meta.fullParagraph);
        pObj.insert(QStringLiteral("defaultStyle"), encodeStyleOverride(meta.defaultStyle));

        QJsonObject fmtObj;
        fmtObj.insert(QStringLiteral("alignment"), alignmentToString(meta.format.alignment));
        fmtObj.insert(QStringLiteral("leftIndent"), meta.format.leftIndent);
        fmtObj.insert(QStringLiteral("rightIndent"), meta.format.rightIndent);
        fmtObj.insert(QStringLiteral("firstLineIndent"), meta.format.firstLineIndent);
        fmtObj.insert(QStringLiteral("lineHeight"), meta.format.lineHeight);
        fmtObj.insert(QStringLiteral("spaceBefore"), meta.format.spaceBefore);
        fmtObj.insert(QStringLiteral("spaceAfter"), meta.format.spaceAfter);
        pObj.insert(QStringLiteral("format"), fmtObj);
        paraArr.append(pObj);
    }
    root.insert(QStringLiteral("paragraphs"), paraArr);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<RichTextClipboardPayload> decodeRichTextPayload(const QByteArray& bytes)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("plainText")).isString())
        return std::nullopt;

    RichTextClipboardPayload payload;
    payload.plainText = root.value(QStringLiteral("plainText")).toString();
    const int textLen = static_cast<int>(payload.plainText.size());

    const QJsonArray spansArr = root.value(QStringLiteral("formatSpans")).toArray();
    int prevEnd = 0;
    for (const auto& val : spansArr) {
        if (!val.isObject())
            return std::nullopt;
        const QJsonObject sObj = val.toObject();
        const int start = sObj.value(QStringLiteral("start")).toInt(-1);
        const int end = sObj.value(QStringLiteral("end")).toInt(-1);
        if (start < prevEnd || start >= end || end > textLen)
            return std::nullopt;
        const auto style = decodeStyleOverride(sObj.value(QStringLiteral("style")).toObject());
        if (!style)
            return std::nullopt;
        payload.formatSpans.push_back({{start, end}, *style});
        prevEnd = end;
    }

    const QJsonArray paraArr = root.value(QStringLiteral("paragraphs")).toArray();
    for (const auto& val : paraArr) {
        if (!val.isObject())
            return std::nullopt;
        const QJsonObject pObj = val.toObject();
        CopiedParagraphMeta meta;
        meta.role = roleFromString(pObj.value(QStringLiteral("role")).toString());
        meta.headingLevel = qBound(1, pObj.value(QStringLiteral("headingLevel")).toInt(1), 9);
        meta.fullParagraph = pObj.value(QStringLiteral("fullParagraph")).toBool(false);
        if (const auto defStyle = decodeStyleOverride(pObj.value(QStringLiteral("defaultStyle")).toObject())) {
            meta.defaultStyle = *defStyle;
        }
        const QJsonObject fmtObj = pObj.value(QStringLiteral("format")).toObject();
        meta.format.alignment = alignmentFromString(fmtObj.value(QStringLiteral("alignment")).toString());
        meta.format.leftIndent = qMax<qreal>(0, fmtObj.value(QStringLiteral("leftIndent")).toDouble(0));
        meta.format.rightIndent = qMax<qreal>(0, fmtObj.value(QStringLiteral("rightIndent")).toDouble(0));
        meta.format.firstLineIndent = fmtObj.value(QStringLiteral("firstLineIndent")).toDouble(0);
        const qreal lh = fmtObj.value(QStringLiteral("lineHeight")).toDouble(26.0);
        meta.format.lineHeight = (std::isfinite(lh) && lh > 0) ? lh : 26.0;
        meta.format.spaceBefore = qMax<qreal>(0, fmtObj.value(QStringLiteral("spaceBefore")).toDouble(0));
        meta.format.spaceAfter = qMax<qreal>(0, fmtObj.value(QStringLiteral("spaceAfter")).toDouble(0));
        payload.paragraphs.push_back(std::move(meta));
    }

    return payload;
}

std::unique_ptr<QMimeData> buildRichTextMimeData(const RichTextClipboardPayload& payload)
{
    auto mime = std::make_unique<QMimeData>();
    mime->setText(payload.plainText);
    mime->setData(QString::fromLatin1(kRichTextMimeType), encodeRichTextPayload(payload));
    return mime;
}

std::optional<ImageClipboardPayload> extractImagePayload(const data::DocumentSnapshot& doc,
                                                         const data::ContainerId& id)
{
    const data::ContainerNode* node = nullptr;
    data::ContainerId parentId;
    if (!findImageNode(doc.root, id, {}, node, parentId) ||
        !node ||
        node->type != data::ContainerType::Image) {
        return std::nullopt;
    }

    const auto imgIt = doc.resources.constFind(node->image.resourceId);
    if (imgIt == doc.resources.constEnd() || imgIt->isNull())
        return std::nullopt;

    ImageClipboardPayload payload;
    payload.sourceResourceId = node->image.resourceId;
    payload.image = imgIt->copy();
    payload.displaySize = node->image.displaySize;
    payload.placement = node->placement;
    payload.geometry = node->geometry;
    payload.sourceParentId = parentId;
    payload.sourceContainerId = node->id;
    return payload;
}

QByteArray encodeImagePayload(const ImageClipboardPayload& payload)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("sourceResourceId"), payload.sourceResourceId);
    root.insert(QStringLiteral("sourceParentId"), payload.sourceParentId);
    root.insert(QStringLiteral("sourceContainerId"), payload.sourceContainerId);
    root.insert(QStringLiteral("placement"),
                payload.placement == data::PlacementMode::Floating ? QStringLiteral("Floating") : QStringLiteral("Flow"));
    root.insert(QStringLiteral("displayW"), payload.displaySize.width());
    root.insert(QStringLiteral("displayH"), payload.displaySize.height());

    QJsonObject geo;
    geo.insert(QStringLiteral("x"), payload.geometry.position.x());
    geo.insert(QStringLiteral("y"), payload.geometry.position.y());
    geo.insert(QStringLiteral("width"), payload.geometry.width);
    geo.insert(QStringLiteral("minH"), payload.geometry.minimumHeight);
    geo.insert(QStringLiteral("padding"), encodeMargins(payload.geometry.padding));
    geo.insert(QStringLiteral("margin"), encodeMargins(payload.geometry.margin));
    root.insert(QStringLiteral("geometry"), geo);

    if (!payload.image.isNull()) {
        QByteArray pngBytes;
        QBuffer buf(&pngBytes);
        buf.open(QIODevice::WriteOnly);
        if (payload.image.save(&buf, "PNG")) {
            root.insert(QStringLiteral("pngBase64"), QString::fromLatin1(pngBytes.toBase64()));
        }
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<ImageClipboardPayload> decodeImagePayload(const QByteArray& bytes,
                                                        const QImage& fallbackImage)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject root = doc.object();
    ImageClipboardPayload payload;
    payload.sourceResourceId = root.value(QStringLiteral("sourceResourceId")).toString();
    payload.sourceParentId = root.value(QStringLiteral("sourceParentId")).toString();
    payload.sourceContainerId = root.value(QStringLiteral("sourceContainerId")).toString();
    payload.placement = (root.value(QStringLiteral("placement")).toString() == QStringLiteral("Flow"))
        ? data::PlacementMode::Flow
        : data::PlacementMode::Floating;

    const qreal w = root.value(QStringLiteral("displayW")).toDouble();
    const qreal h = root.value(QStringLiteral("displayH")).toDouble();
    if (!std::isfinite(w) || w <= 0 || !std::isfinite(h) || h <= 0)
        return std::nullopt;
    payload.displaySize = QSizeF(w, h);

    const QJsonObject geo = root.value(QStringLiteral("geometry")).toObject();
    payload.geometry.position = QPointF(geo.value(QStringLiteral("x")).toDouble(),
                                        geo.value(QStringLiteral("y")).toDouble());
    payload.geometry.width = qMax<qreal>(0, geo.value(QStringLiteral("width")).toDouble());
    payload.geometry.minimumHeight = qMax<qreal>(0, geo.value(QStringLiteral("minH")).toDouble());
    payload.geometry.padding = decodeMargins(geo.value(QStringLiteral("padding")).toObject());
    payload.geometry.margin = decodeMargins(geo.value(QStringLiteral("margin")).toObject());

    const QString b64 = root.value(QStringLiteral("pngBase64")).toString();
    if (!b64.isEmpty()) {
        payload.image = QImage::fromData(QByteArray::fromBase64(b64.toLatin1()), "PNG");
    }
    if (payload.image.isNull() && !fallbackImage.isNull()) {
        payload.image = fallbackImage.copy();
    }
    if (payload.image.isNull())
        return std::nullopt;

    return payload;
}

std::unique_ptr<QMimeData> buildImageMimeData(const ImageClipboardPayload& payload)
{
    auto mime = std::make_unique<QMimeData>();
    mime->setData(QString::fromLatin1(kImageMimeType), encodeImagePayload(payload));
    mime->setImageData(payload.image);
    return mime;
}

} // namespace editor::interaction
