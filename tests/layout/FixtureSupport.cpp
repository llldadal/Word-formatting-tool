#include "FixtureSupport.h"
#include <algorithm>
#include <stdexcept>

using namespace editor::data;

namespace editor::testing::detail {

const ContainerNode& findNode(const ContainerNode& root, const QString& id)
{
    if (root.id == id) return root;
    for (const auto& child : root.children) {
        try { return findNode(child, id); } catch (const std::out_of_range&) {}
    }
    throw std::out_of_range("Unknown fixture container");
}

TextStyle mergeStyle(TextStyle base, const TextStyleOverride& over)
{
    if (over.fontFamily) base.fontFamily = *over.fontFamily;
    if (over.fontSizePt) base.fontSizePt = *over.fontSizePt;
    if (over.color) base.color = *over.color;
    if (over.bold) base.bold = *over.bold;
    if (over.italic) base.italic = *over.italic;
    if (over.underline) base.underline = *over.underline;
    return base;
}

QFont makeFont(const TextStyle& style)
{
    QFont font(style.fontFamily);
    // 标准题使用能整除为像素的字号，显式换算以摆脱操作系统屏幕 DPI 的影响。
    font.setPixelSize(qRound(style.fontSizePt * DocumentUnitsPerInch / 72.0));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    return font;
}

QTextCharFormat makeCharacterFormat(const TextStyle& style)
{
    QTextCharFormat format;
    format.setFont(makeFont(style));
    format.setForeground(style.color);
    return format;
}

std::shared_ptr<QTextLayout> makeParagraphLayout(const ContainerNode& node, int paragraphIndex)
{
    const auto& paragraph = node.text.paragraphs.at(paragraphIndex);
    const auto base = mergeStyle(node.text.defaultStyle, paragraph.defaultStyle);
    const auto range = paragraph.range;
    auto result = std::make_shared<QTextLayout>(node.text.text.mid(range.start, range.end - range.start), makeFont(base));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setUseDesignMetrics(true);
    switch (paragraph.format.alignment) {
    case ParagraphAlignment::Left: option.setAlignment(Qt::AlignLeft); break;
    case ParagraphAlignment::Center: option.setAlignment(Qt::AlignHCenter); break;
    case ParagraphAlignment::Right: option.setAlignment(Qt::AlignRight); break;
    case ParagraphAlignment::Justify: option.setAlignment(Qt::AlignJustify); break;
    }
    result->setTextOption(option);
    result->setCacheEnabled(true);
    QList<QTextLayout::FormatRange> formats;
    int next = range.start;
    for (const auto& span : node.text.formatSpans) {
        const int start = std::max(range.start, span.range.start);
        const int end = std::min(range.end, span.range.end);
        if (start >= end) continue;
        if (next < start) formats.push_back({next - range.start, start - next, makeCharacterFormat(base)});
        formats.push_back({start - range.start, end - start, makeCharacterFormat(mergeStyle(base, span.style))});
        next = end;
    }
    if (next < range.end) formats.push_back({next - range.start, range.end - next, makeCharacterFormat(base)});
    result->setFormats(formats);
    return result;
}

} // namespace editor::testing::detail
