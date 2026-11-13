#include "DocxExporter.h"
#include "ZipWriter.h"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QMarginsF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <vector>

namespace editor::io {
namespace {

using data::ContainerId;
using data::ContainerLayout;
using data::ContainerNode;
using data::ImageLayout;
using data::LayoutResult;
using data::ParagraphAlignment;
using data::ParagraphInfo;
using data::ParagraphRole;
using data::PlacementMode;
using data::TextFormatSpan;
using data::TextStyle;
using data::TextStyleOverride;

// ---------- 基础换算与转义 ----------

// 1 文档逻辑像素 = 15 twip = 9525 EMU；字号 pt 用半点。
int toTwips(qreal px) { return qRound(px * 15.0); }
qint64 toEmu(qreal px) { return qRound64(px * 9525.0); }
int toHalfPoints(qreal pt) { return qRound(pt * 2.0); }
qreal toPoints(qreal px) { return px * 0.75; } // VML 回退分支使用磅。

QString escapeXml(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        case u'\'': out += QStringLiteral("&apos;"); break;
        default: out += c;
        }
    }
    return out;
}

QString colorHex(const QColor& color)
{
    if (!color.isValid()) return QStringLiteral("000000");
    return color.name().mid(1).toUpper();
}

void applyOverride(TextStyle* style, const TextStyleOverride& o)
{
    if (o.fontFamily) style->fontFamily = *o.fontFamily;
    if (o.fontSizePt) style->fontSizePt = *o.fontSizePt;
    if (o.color) style->color = *o.color;
    if (o.bold) style->bold = *o.bold;
    if (o.italic) style->italic = *o.italic;
    if (o.underline) style->underline = *o.underline;
}

// ---------- 生成计划 ----------

struct MediaItem {
    QString partName; // word/media/imageN.png
    QString relId;    // document.xml.rels 中的关系 ID
    QByteArray bytes;
};

struct FloatingPlan {
    const ContainerNode* node = nullptr;
    ContainerLayout box;         // 排版结果中的实际框。
    QString relId;              // 图片浮动对象使用；文本框为空。
    int docPrId = 0;
    int relativeHeight = 0;
};

const ContainerLayout* findLayout(const LayoutResult& layout, const ContainerId& id)
{
    for (const auto& c : layout.containers)
        if (c.containerId == id) return &c;
    return nullptr;
}

const ImageLayout* findImage(const LayoutResult& layout, const ContainerId& id)
{
    for (const auto& i : layout.images)
        if (i.containerId == id) return &i;
    return nullptr;
}

// 收集资源表里实际被引用的图片，按排版结果中的出现顺序编号，同一资源只存一份。
DocxExportResult collectMedia(const data::DocumentSnapshot& doc,
                              const LayoutResult& layout,
                              std::vector<MediaItem>* media,
                              QHash<ContainerId, QString>* relIdByContainer)
{
    QHash<ContainerId, QString> relIdByResource; // 资源 ID -> 关系 ID，做去重。
    for (const auto& image : layout.images) {
        if (relIdByContainer->contains(image.containerId)) continue;
        if (relIdByResource.contains(image.resourceId)) {
            relIdByContainer->insert(image.containerId,
                                     relIdByResource.value(image.resourceId));
            continue;
        }
        const auto it = doc.resources.constFind(image.resourceId);
        if (it == doc.resources.constEnd() || it->isNull())
            return {DocxExportStatus::IoError,
                    QStringLiteral("图片资源缺失或无法解码：%1").arg(image.resourceId),
                    QString()};
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!it->save(&buffer, "PNG"))
            return {DocxExportStatus::IoError,
                    QStringLiteral("图片编码失败：%1").arg(image.resourceId),
                    QString()};
        MediaItem item;
        item.partName = QStringLiteral("word/media/image%1.png").arg(media->size() + 1);
        item.relId = QStringLiteral("rId%1").arg(media->size() + 2);
        item.bytes = png;
        media->push_back(item);
        relIdByResource.insert(image.resourceId, item.relId);
        relIdByContainer->insert(image.containerId, item.relId);
    }
    return {};
}

// ---------- 段落与文字 ----------

struct ParaOut {
    int before = 0;     // twip
    int after = 0;
    int line = 0;       // 0 表示不写行距。
    int left = 0;
    int right = 0;
    int firstLine = 0;
    QString jc;         // 空 表示左对齐省略。
    QString pStyle;     // 空 表示普通正文。
};

QString runXml(const QString& text, const TextStyle& style)
{
    QString rpr;
    if (!style.fontFamily.isEmpty()) {
        const QString f = escapeXml(style.fontFamily);
        rpr += QStringLiteral("<w:rFonts w:ascii=\"%1\" w:hAnsi=\"%1\" w:eastAsia=\"%1\" w:cs=\"%1\"/>").arg(f);
    }
    if (style.bold) rpr += QStringLiteral("<w:b/>");
    if (style.italic) rpr += QStringLiteral("<w:i/>");
    if (style.underline) rpr += QStringLiteral("<w:u w:val=\"single\"/>");
    rpr += QStringLiteral("<w:color w:val=\"%1\"/>").arg(colorHex(style.color));
    const int sz = toHalfPoints(style.fontSizePt);
    rpr += QStringLiteral("<w:sz w:val=\"%1\"/><w:szCs w:val=\"%1\"/>").arg(sz);
    return QStringLiteral("<w:r><w:rPr>%1</w:rPr><w:t xml:space=\"preserve\">%2</w:t></w:r>")
        .arg(rpr, escapeXml(text));
}

QString jcValue(ParagraphAlignment alignment)
{
    switch (alignment) {
    case ParagraphAlignment::Center: return QStringLiteral("center");
    case ParagraphAlignment::Right: return QStringLiteral("right");
    case ParagraphAlignment::Justify: return QStringLiteral("both");
    default: return QString();
    }
}

QString pStyleValue(const ParagraphInfo& info)
{
    if (info.role == ParagraphRole::Title) return QStringLiteral("Title");
    if (info.role == ParagraphRole::Heading)
        return QStringLiteral("Heading%1").arg(info.headingLevel);
    return QString();
}

// 生成一个 w:p。info 为空表示空段落（锚点承载或空文档兜底）。
// leadRuns 会插在文字 run 之前，用于承载浮动对象的 w:drawing。
QString paragraphXml(const ContainerNode& node, const ParagraphInfo* info,
                     const ParaOut& fmt, const QString& leadRuns = QString())
{
    const QString& text = node.text.text;
    const int pStart = info ? info->range.start : 0;
    const int pEnd = info ? info->range.end : 0;

    QString pPr;
    if (!fmt.pStyle.isEmpty())
        pPr += QStringLiteral("<w:pStyle w:val=\"%1\"/>").arg(fmt.pStyle);
    if (fmt.before || fmt.after || fmt.line)
        pPr += QStringLiteral("<w:spacing w:before=\"%1\" w:after=\"%2\" w:line=\"%3\" w:lineRule=\"exact\"/>")
                   .arg(fmt.before).arg(fmt.after).arg(fmt.line);
    if (fmt.left || fmt.right || fmt.firstLine)
        pPr += QStringLiteral("<w:ind w:left=\"%1\" w:right=\"%2\" w:firstLine=\"%3\"/>")
                   .arg(fmt.left).arg(fmt.right).arg(fmt.firstLine);
    if (!fmt.jc.isEmpty())
        pPr += QStringLiteral("<w:jc w:val=\"%1\"/>").arg(fmt.jc);

    // 以段落范围和格式区间边界切分 run，避免同一 run 内格式变化。
    std::vector<int> bounds{pStart, pEnd};
    for (const auto& span : node.text.formatSpans) {
        const int s = std::max(span.range.start, pStart);
        const int e = std::min(span.range.end, pEnd);
        if (s < e) { bounds.push_back(s); bounds.push_back(e); }
    }
    // 不让边界落在 UTF-16 代理对中间。
    for (size_t i = 1; i + 1 < bounds.size(); ++i) {
        if (bounds[i] > 0 && bounds[i] < text.size()
            && text.at(bounds[i] - 1).isHighSurrogate()
            && text.at(bounds[i]).isLowSurrogate()) {
            bounds[i] += 1;
        }
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    QString runs = leadRuns;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        const int a = bounds[i];
        const int b = bounds[i + 1];
        if (a >= b) continue;
        TextStyle style = node.text.defaultStyle;
        if (info) applyOverride(&style, info->defaultStyle);
        for (const auto& span : node.text.formatSpans)
            if (span.range.start <= a && b <= span.range.end)
                applyOverride(&style, span.style);
        const QString segment = text.mid(a, b - a);
        if (!segment.isEmpty()) runs += runXml(segment, style);
    }
    return QStringLiteral("<w:p>%1%2</w:p>")
        .arg(pPr.isEmpty() ? QString() : QStringLiteral("<w:pPr>%1</w:pPr>").arg(pPr),
             runs);
}

// ---------- 浮动对象锚点 ----------

struct AnchorContext {
    bool shapetypeEmitted = false; // VML 回退分支的 shapetype 只写一次。
};

// 文本框内部的段落：不携带容器边距（边距映射到形状内边距）。
QString textBoxParagraphsXml(const ContainerNode& node)
{
    const auto& paras = node.text.paragraphs;
    if (paras.empty()) return QStringLiteral("<w:p></w:p>");
    QString xml;
    for (const auto& p : paras) {
        ParaOut fmt;
        fmt.line = toTwips(p.format.lineHeight);
        fmt.before = toTwips(p.format.spaceBefore);
        fmt.after = toTwips(p.format.spaceAfter);
        fmt.left = toTwips(p.format.leftIndent);
        fmt.right = toTwips(p.format.rightIndent);
        fmt.firstLine = toTwips(p.format.firstLineIndent);
        fmt.jc = jcValue(p.format.alignment);
        fmt.pStyle = pStyleValue(p);
        xml += paragraphXml(node, &p, fmt);
    }
    return xml;
}

QString anchorOpenXml(const FloatingPlan& plan, const QMarginsF& margin)
{
    return QStringLiteral(
               "<wp:anchor distT=\"%1\" distB=\"%2\" distL=\"%3\" distR=\"%4\" "
               "simplePos=\"0\" relativeHeight=\"%5\" behindDoc=\"0\" locked=\"0\" "
               "layoutInCell=\"1\" allowOverlap=\"1\">"
               "<wp:simplePos x=\"0\" y=\"0\"/>")
        .arg(toEmu(margin.top()))
        .arg(toEmu(margin.bottom()))
        .arg(toEmu(margin.left()))
        .arg(toEmu(margin.right()))
        .arg(plan.relativeHeight)
        + QStringLiteral(
               "<wp:positionH relativeFrom=\"page\"><wp:posOffset>%1</wp:posOffset></wp:positionH>"
               "<wp:positionV relativeFrom=\"page\"><wp:posOffset>%2</wp:posOffset></wp:positionV>"
               "<wp:extent cx=\"%3\" cy=\"%4\"/>"
               "<wp:effectExtent l=\"%5\" t=\"%6\" r=\"%7\" b=\"%8\"/>"
               "<wp:wrapSquare wrapText=\"bothSides\"/>"
               "<wp:docPr id=\"%9\" name=\"%10\"/>")
              .arg(toEmu(plan.box.rect.left()))
              .arg(toEmu(plan.box.rect.top()))
              .arg(toEmu(plan.box.rect.width()))
              .arg(toEmu(plan.box.rect.height()))
              .arg(toEmu(margin.left()))
              .arg(toEmu(margin.top()))
              .arg(toEmu(margin.right()))
              .arg(toEmu(margin.bottom()))
              .arg(plan.docPrId)
              .arg(escapeXml(plan.node->id));
}

QString imageAnchorRunXml(const FloatingPlan& plan)
{
    const QMarginsF margin = plan.node->geometry.margin;
    return QStringLiteral("<w:r><w:drawing>") + anchorOpenXml(plan, margin)
         + QStringLiteral("<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">")
         + QStringLiteral("<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%1\" name=\"%2\"/><pic:cNvPicPr/></pic:nvPicPr>")
               .arg(plan.docPrId)
               .arg(escapeXml(plan.node->id))
         + QStringLiteral("<pic:blipFill><a:blip r:embed=\"%1\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>")
               .arg(plan.relId)
         + QStringLiteral("<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm>"
                          "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>")
               .arg(toEmu(plan.box.rect.width()))
               .arg(toEmu(plan.box.rect.height()))
         + QStringLiteral("</pic:pic></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>");
}

QString textBoxAnchorRunXml(const FloatingPlan& plan, AnchorContext* ctx)
{
    const QMarginsF margin = plan.node->geometry.margin;
    const QMarginsF padding = plan.node->geometry.padding;
    const QString paragraphs = textBoxParagraphsXml(*plan.node);

    // Word 2010+ 读取 wps 分支；旧版本读取 VML 回退分支，两边内容一致。
    QString choice = QStringLiteral("<w:drawing>") + anchorOpenXml(plan, margin)
        + QStringLiteral("<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\">")
        + QStringLiteral("<wps:wsp><wps:cNvSpPr txBox=\"1\"/>")
        + QStringLiteral("<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm>"
                         "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></wps:spPr>")
              .arg(toEmu(plan.box.rect.width()))
              .arg(toEmu(plan.box.rect.height()))
        + QStringLiteral("<wps:txbx><w:txbxContent>%1</w:txbxContent></wps:txbx>").arg(paragraphs)
        + QStringLiteral("<wps:bodyPr rot=\"0\" vert=\"horz\" wrap=\"square\" "
                         "lIns=\"%1\" tIns=\"%2\" rIns=\"%3\" bIns=\"%4\" "
                         "numCol=\"1\" anchor=\"t\" anchorCtr=\"0\"><a:noAutofit/></wps:bodyPr>")
              .arg(toEmu(padding.left()))
              .arg(toEmu(padding.top()))
              .arg(toEmu(padding.right()))
              .arg(toEmu(padding.bottom()))
        + QStringLiteral("</wps:wsp></a:graphicData></a:graphic></wp:anchor></w:drawing>");

    QString shapetype;
    if (!ctx->shapetypeEmitted) {
        ctx->shapetypeEmitted = true;
        shapetype = QStringLiteral(
            "<v:shapetype id=\"_x0000_t202\" coordsize=\"21600,21600\" o:spt=\"202\" "
            "path=\"m,l,21600r21600,l21600,xe\">"
            "<v:stroke joinstyle=\"miter\"/><v:path gradientshapeok=\"t\" o:connecttype=\"rect\"/>"
            "</v:shapetype>");
    }
    const QString shapeStyle = QStringLiteral(
        "position:absolute;margin-left:%1pt;margin-top:%2pt;width:%3pt;height:%4pt;"
        "z-index:%5;mso-position-horizontal-relative:page;mso-position-vertical-relative:page")
        .arg(toPoints(plan.box.rect.left()), 0, 'f', 2)
        .arg(toPoints(plan.box.rect.top()), 0, 'f', 2)
        .arg(toPoints(plan.box.rect.width()), 0, 'f', 2)
        .arg(toPoints(plan.box.rect.height()), 0, 'f', 2)
        .arg(plan.relativeHeight);
    const QString inset = QStringLiteral("%1pt,%2pt,%3pt,%4pt")
                              .arg(toPoints(padding.left()), 0, 'f', 2)
                              .arg(toPoints(padding.top()), 0, 'f', 2)
                              .arg(toPoints(padding.right()), 0, 'f', 2)
                              .arg(toPoints(padding.bottom()), 0, 'f', 2);
    QString fallback = QStringLiteral("<w:pict>") + shapetype
        + QStringLiteral("<v:shape id=\"vml_%1\" type=\"#_x0000_t202\" style=\"%2\" o:allowincell=\"f\">")
              .arg(escapeXml(plan.node->id), shapeStyle)
        + QStringLiteral("<v:textbox inset=\"%1\"><w:txbxContent>%2</w:txbxContent></v:textbox>")
              .arg(inset, paragraphs)
        + QStringLiteral("<w10:wrap type=\"square\"/></v:shape></w:pict>");

    return QStringLiteral("<w:r><mc:AlternateContent>"
                          "<mc:Choice Requires=\"wps\">%1</mc:Choice>"
                          "<mc:Fallback>%2</mc:Fallback>"
                          "</mc:AlternateContent></w:r>")
        .arg(choice, fallback);
}

QString inlineImageRunXml(const ContainerNode& node, const ImageLayout& image,
                          const QString& relId, int docPrId)
{
    return QStringLiteral("<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">")
         + QStringLiteral("<wp:extent cx=\"%1\" cy=\"%2\"/>")
               .arg(toEmu(image.rect.width()))
               .arg(toEmu(image.rect.height()))
         + QStringLiteral("<wp:docPr id=\"%1\" name=\"%2\"/>").arg(docPrId).arg(escapeXml(node.id))
         + QStringLiteral("<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">")
         + QStringLiteral("<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%1\" name=\"%2\"/><pic:cNvPicPr/></pic:nvPicPr>")
               .arg(docPrId).arg(escapeXml(node.id))
         + QStringLiteral("<pic:blipFill><a:blip r:embed=\"%1\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>")
               .arg(relId)
         + QStringLiteral("<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm>"
                          "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>")
               .arg(toEmu(image.rect.width()))
               .arg(toEmu(image.rect.height()))
         + QStringLiteral("</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>");
}

// ---------- 顺序流写块 ----------

struct FlowCtx {
    int pendingBefore = 0; // 累加到本块第一段 before 的块间距。
    int extraLeft = 0;     // 祖先（分组等）累计的左右缩进。
    int extraRight = 0;
};

struct BlockResult {
    QString xml;        // 本块的段落序列。
    int tailAfter = 0;  // 本块尾部间距（自身边距合计），交给下一块开头。
};

struct WriterState {
    const LayoutResult* layout = nullptr;
    const QHash<ContainerId, FloatingPlan>* floats = nullptr;
    const QHash<ContainerId, QString>* relIdByContainer = nullptr;
    AnchorContext anchorCtx;
    int nextDocPrId = 1;
};

BlockResult writeTextContainer(const ContainerNode& node, const FlowCtx& ctx,
                               WriterState* state)
{
    // 本容器的浮动子节点锚定到第一个段落。
    QString anchorRuns;
    for (const auto& child : node.children) {
        const auto it = state->floats->constFind(child.id);
        if (it == state->floats->constEnd()) continue;
        if (child.type == data::ContainerType::Image)
            anchorRuns += imageAnchorRunXml(it.value());
        else
            anchorRuns += textBoxAnchorRunXml(it.value(), &state->anchorCtx);
    }

    const int lead = ctx.pendingBefore
                   + toTwips(node.geometry.margin.top() + node.geometry.padding.top());
    const int tail = toTwips(node.geometry.margin.bottom() + node.geometry.padding.bottom());
    const int baseLeft = ctx.extraLeft
                       + toTwips(node.geometry.padding.left() + node.geometry.margin.left());
    const int baseRight = ctx.extraRight
                        + toTwips(node.geometry.padding.right() + node.geometry.margin.right());

    const auto& paras = node.text.paragraphs;
    QString xml;
    if (paras.empty()) {
        ParaOut fmt;
        fmt.before = lead;
        fmt.after = tail;
        fmt.left = baseLeft;
        fmt.right = baseRight;
        xml = paragraphXml(node, nullptr, fmt, anchorRuns);
        return {xml, tail};
    }
    for (size_t i = 0; i < paras.size(); ++i) {
        const auto& p = paras[i];
        const bool first = (i == 0);
        const bool last = (i + 1 == paras.size());
        ParaOut fmt;
        fmt.before = toTwips(p.format.spaceBefore) + (first ? lead : 0);
        fmt.after = toTwips(p.format.spaceAfter) + (last ? tail : 0);
        fmt.line = toTwips(p.format.lineHeight);
        fmt.left = baseLeft + toTwips(p.format.leftIndent);
        fmt.right = baseRight + toTwips(p.format.rightIndent);
        fmt.firstLine = toTwips(p.format.firstLineIndent);
        fmt.jc = jcValue(p.format.alignment);
        fmt.pStyle = pStyleValue(p);
        xml += paragraphXml(node, &p, fmt, first ? anchorRuns : QString());
    }
    return {xml, tail};
}

BlockResult writeImageBlock(const ContainerNode& node, const FlowCtx& ctx,
                            WriterState* state)
{
    const ImageLayout* image = findImage(*state->layout, node.id);
    if (!image)
        return {QStringLiteral("<w:p></w:p>"), 0}; // 校验阶段已拦截，这里不发生。
    const auto relIt = state->relIdByContainer->constFind(node.id);
    const QString relId = relIt != state->relIdByContainer->constEnd()
                              ? relIt.value() : QString();
    const int docPrId = state->nextDocPrId++;
    const int lead = ctx.pendingBefore
                   + toTwips(node.geometry.margin.top() + node.geometry.padding.top());
    const int tail = toTwips(node.geometry.margin.bottom() + node.geometry.padding.bottom());
    QString pPr;
    if (lead || tail)
        pPr = QStringLiteral("<w:pPr><w:spacing w:before=\"%1\" w:after=\"%2\"/></w:pPr>")
                  .arg(lead).arg(tail);
    const QString xml = QStringLiteral("<w:p>%1%2</w:p>")
                            .arg(pPr, inlineImageRunXml(node, *image, relId, docPrId));
    return {xml, tail};
}

// 分组内允许文本、图片和嵌套分组（嵌套分组整体按顺序块处理）。
BlockResult writeFlowBlock(const ContainerNode& node, const FlowCtx& ctx,
                           WriterState* state);

BlockResult writeGroupBlock(const ContainerNode& node, const FlowCtx& ctx,
                            WriterState* state)
{
    FlowCtx inner = ctx;
    inner.extraLeft += toTwips(node.geometry.padding.left() + node.geometry.margin.left());
    inner.extraRight += toTwips(node.geometry.padding.right() + node.geometry.margin.right());
    int carry = ctx.pendingBefore
              + toTwips(node.geometry.margin.top() + node.geometry.padding.top());
    QString xml;
    for (const auto& child : node.children) {
        FlowCtx childCtx = inner;
        childCtx.pendingBefore = carry;
        const BlockResult r = writeFlowBlock(child, childCtx, state);
        xml += r.xml;
        carry = r.tailAfter;
    }
    const int tail = carry
                   + toTwips(node.geometry.padding.bottom() + node.geometry.margin.bottom());
    return {xml, tail};
}

// 分组内允许文本、图片和嵌套分组（嵌套分组整体按顺序块处理）。
BlockResult writeFlowBlock(const ContainerNode& node, const FlowCtx& ctx,
                           WriterState* state)
{
    switch (node.type) {
    case data::ContainerType::Text:
        return writeTextContainer(node, ctx, state);
    case data::ContainerType::Image:
        return writeImageBlock(node, ctx, state);
    case data::ContainerType::Group:
    default:
        return writeGroupBlock(node, ctx, state);
    }
}

// ---------- 包部件 ----------

QString contentTypesXml(bool hasMedia)
{
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>");
    if (hasMedia)
        xml += QStringLiteral("<Default Extension=\"png\" ContentType=\"image/png\"/>");
    xml += QStringLiteral(
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "</Types>");
    return xml;
}

QString rootRelsXml()
{
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>");
}

QString documentRelsXml(const std::vector<MediaItem>& media)
{
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
        "Target=\"styles.xml\"/>");
    for (const auto& item : media) {
        xml += QStringLiteral(
                   "<Relationship Id=\"%1\" "
                   "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
                   "Target=\"media/%2\"/>")
                   .arg(item.relId)
                   .arg(item.partName.section(u'/', -1));
    }
    xml += QStringLiteral("</Relationships>");
    return xml;
}

QString stylesXml()
{
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Microsoft YaHei\" w:hAnsi=\"Microsoft YaHei\" "
        "w:eastAsia=\"Microsoft YaHei\" w:cs=\"Microsoft YaHei\"/>"
        "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
        "<w:lang w:val=\"en-US\" w:eastAsia=\"zh-CN\" w:bidi=\"ar-SA\"/>"
        "</w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:line=\"390\" w:lineRule=\"exact\"/></w:pPr></w:pPrDefault>"
        "</w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Title\">"
        "<w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
        "</w:style>");
    for (int level = 1; level <= 9; ++level) {
        xml += QStringLiteral(
                   "<w:style w:type=\"paragraph\" w:styleId=\"Heading%1\">"
                   "<w:name w:val=\"heading %1\"/><w:basedOn w:val=\"Normal\"/>"
                   "<w:next w:val=\"Normal\"/><w:qFormat/><w:outlineLvl w:val=\"%2\"/>"
                   "</w:style>")
                   .arg(level)
                   .arg(level - 1);
    }
    xml += QStringLiteral("</w:styles>");
    return xml;
}

QString sectPrXml(const data::PageSettings& page)
{
    return QStringLiteral(
        "<w:sectPr><w:pgSz w:w=\"%1\" w:h=\"%2\"/>"
        "<w:pgMar w:top=\"%3\" w:right=\"%4\" w:bottom=\"%3\" w:left=\"%4\" "
        "w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr>")
        .arg(toTwips(page.size.width()))
        .arg(toTwips(page.size.height()))
        .arg(toTwips(page.margins.top()))
        .arg(toTwips(page.margins.left()));
}

QString documentOpenXml()
{
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\" "
        "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
        "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
        "xmlns:v=\"urn:schemas-microsoft-com:vml\" "
        "xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
        "xmlns:w10=\"urn:schemas-microsoft-com:office:word\">");
}

// ---------- 校验 ----------

DocxExportResult validateInput(const data::DocumentSnapshot& doc,
                               const LayoutResult& layout,
                               const QHash<ContainerId, QString>& relIdByContainer,
                               QHash<ContainerId, FloatingPlan>* floats)
{
    if (layout.documentRevision != doc.revision)
        return {DocxExportStatus::IoError,
                QStringLiteral("排版结果版本 %1 与文档版本 %2 不一致")
                    .arg(layout.documentRevision)
                    .arg(doc.revision),
                QString()};

    // 递归检查容器树：深度、摆放模式和排版结果覆盖。
    std::function<DocxExportResult(const ContainerNode&, const ContainerNode*, int)> walk =
        [&](const ContainerNode& node, const ContainerNode* parent, int depth) -> DocxExportResult {
        if (depth > 2)
            return {DocxExportStatus::CompatibilityError,
                    QStringLiteral("容器嵌套超过 v0.1 的一层限制"),
                    node.id};
        if (parent && parent->type == data::ContainerType::Image)
            return {DocxExportStatus::IoError,
                    QStringLiteral("图片容器不能带有子节点"),
                    node.id};
        if (depth == 1 && node.placement == PlacementMode::Floating)
            return {DocxExportStatus::CompatibilityError,
                    QStringLiteral("顶层容器不支持浮动摆放"),
                    node.id};
        if (depth == 2) {
            const bool needFloating = parent->type == data::ContainerType::Text;
            if (needFloating && node.placement != PlacementMode::Floating)
                return {DocxExportStatus::CompatibilityError,
                        QStringLiteral("文本容器的子节点必须使用浮动摆放"),
                        node.id};
            if (!needFloating && node.placement == PlacementMode::Floating)
                return {DocxExportStatus::CompatibilityError,
                        QStringLiteral("分组容器的子节点必须参与顺序排版"),
                        node.id};
        }

        // 浮动子节点：检查排版覆盖与是否越出固定纸面。
        if (parent && parent->type == data::ContainerType::Text
            && node.placement == PlacementMode::Floating) {
            const ContainerLayout* box = findLayout(layout, node.id);
            if (!box)
                return {DocxExportStatus::IoError,
                        QStringLiteral("排版结果缺少容器几何：%1").arg(node.id),
                        node.id};
            const QRectF paper = layout.pageRect;
            const QRectF& outer = box->outerRect;
            constexpr qreal tol = 0.5;
            if (outer.left() < paper.left() - tol || outer.top() < paper.top() - tol
                || outer.right() > paper.right() + tol || outer.bottom() > paper.bottom() + tol)
                return {DocxExportStatus::CompatibilityError,
                        QStringLiteral("浮动容器越出 v0.1 的固定单页，分页映射尚未定义"),
                        node.id};
            FloatingPlan plan;
            plan.node = &node;
            plan.box = *box;
            plan.relId = relIdByContainer.value(node.id);
            plan.docPrId = int(floats->size()) + 1;
            plan.relativeHeight = 251659264 + int(floats->size());
            floats->insert(node.id, plan);
        }
        for (const auto& child : node.children) {
            const DocxExportResult r = walk(child, &node, depth + 1);
            if (r.status != DocxExportStatus::Ok) return r;
        }
        return {};
    };

    for (const auto& child : doc.root.children) {
        const DocxExportResult r = walk(child, &doc.root, 1);
        if (r.status != DocxExportStatus::Ok) return r;
    }

    // 排版结果里的容器必须都能回指快照，防止拿旧几何导出新文档。
    QSet<ContainerId> snapshotIds;
    std::function<void(const ContainerNode&)> collectIds = [&](const ContainerNode& n) {
        snapshotIds.insert(n.id);
        for (const auto& c : n.children) collectIds(c);
    };
    for (const auto& child : doc.root.children) collectIds(child);
    for (const auto& c : layout.containers) {
        if (!snapshotIds.contains(c.containerId))
            return {DocxExportStatus::IoError,
                    QStringLiteral("排版结果引用了快照中不存在的容器：%1").arg(c.containerId),
                    c.containerId};
    }
    return {};
}

} // namespace

DocxBuildResult buildDocx(const data::DocumentSnapshot& doc, const LayoutResult& layout)
{
    DocxBuildResult out;

    // 1. 收集媒体资源（按排版结果中的出现顺序，去重）。
    std::vector<MediaItem> media;
    QHash<ContainerId, QString> relIdByContainer;
    const DocxExportResult mediaResult = collectMedia(doc, layout, &media, &relIdByContainer);
    if (mediaResult.status != DocxExportStatus::Ok) {
        out.result = mediaResult;
        return out;
    }

    // 2. 校验容器树与排版结果，收集浮动对象计划。
    QHash<ContainerId, FloatingPlan> floats;
    const DocxExportResult validation = validateInput(doc, layout, relIdByContainer, &floats);
    if (validation.status != DocxExportStatus::Ok) {
        out.result = validation;
        return out;
    }

    // 3. 顶层顺序块：前块尾部间距写进下一块第一段的 before。
    //    行内图片的 docPr 编号接在浮动对象之后，保证全文档唯一。
    WriterState state;
    state.layout = &layout;
    state.floats = &floats;
    state.relIdByContainer = &relIdByContainer;
    state.nextDocPrId = int(floats.size()) + 1;
    QString body;
    int carry = 0;
    for (const auto& child : doc.root.children) {
        FlowCtx ctx;
        ctx.pendingBefore = carry;
        const BlockResult block = writeFlowBlock(child, ctx, &state);
        body += block.xml;
        carry = block.tailAfter;
    }
    if (body.isEmpty())
        body = QStringLiteral("<w:p></w:p>"); // 空文档也保留一个可编辑段落。
    body += sectPrXml(doc.page);

    const QString documentXml = documentOpenXml()
                              + QStringLiteral("<w:body>%1</w:body></w:document>").arg(body);

    // 4. 装配 ZIP 包，条目顺序固定。
    std::vector<zip::ZipEntry> entries;
    entries.push_back({QStringLiteral("[Content_Types].xml"), contentTypesXml(!media.empty()).toUtf8()});
    entries.push_back({QStringLiteral("_rels/.rels"), rootRelsXml().toUtf8()});
    entries.push_back({QStringLiteral("word/document.xml"), documentXml.toUtf8()});
    entries.push_back({QStringLiteral("word/_rels/document.xml.rels"), documentRelsXml(media).toUtf8()});
    entries.push_back({QStringLiteral("word/styles.xml"), stylesXml().toUtf8()});
    for (const auto& item : media)
        entries.push_back({item.partName, item.bytes});

    out.package = zip::buildStoreArchive(entries);
    out.result.status = DocxExportStatus::Ok;
    return out;
}

DocxExportResult exportDocx(const data::DocumentSnapshot& doc, const LayoutResult& layout,
                            const QString& filePath)
{
    const DocxBuildResult build = buildDocx(doc, layout);
    if (build.result.status != DocxExportStatus::Ok)
        return build.result;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return {DocxExportStatus::IoError,
                QStringLiteral("无法打开导出文件：%1").arg(filePath),
                QString()};
    if (file.write(build.package) != build.package.size())
        return {DocxExportStatus::IoError,
                QStringLiteral("写入导出文件不完整：%1").arg(filePath),
                QString()};
    return {};
}

} // namespace editor::io
