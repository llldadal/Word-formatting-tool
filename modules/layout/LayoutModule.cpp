#include "LayoutModule.h"
#include <QTextCharFormat>
#include <QFont>
#include <QTextOption>
#include <memory>

namespace editor::layout {

static data::TextStyle mergeTextStyle(
    data::TextStyle base, const data::TextStyleOverride& change)
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

static QTextCharFormat makeCharFormat(const data::TextStyle& style)
{
    QFont font(style.fontFamily);
    font.setPixelSize(qRound(style.fontSizePt * data::DocumentUnitsPerInch / 72.0));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);

    QTextCharFormat format;
    format.setFont(font);
    format.setForeground(style.color);
    return format;
}

static std::vector<QRectF> availableTextAreas(
    const QRectF& lineArea, const std::vector<QRectF>& obstacles)
{
    std::vector<QRectF> areas{lineArea};
    for (const auto& obstacle : obstacles)
    {
        std::vector<QRectF> remaining;
        for (const auto& area : areas)
        {
            if (!area.intersects(obstacle))
            {
                remaining.push_back(area);
                continue;
            }

            if (area.left() < obstacle.left())
            {
                remaining.emplace_back(area.left(), area.top(),
                                       obstacle.left() - area.left(), area.height());
            }
            if (obstacle.right() < area.right())
            {
                remaining.emplace_back(obstacle.right(), area.top(),
                                       area.right() - obstacle.right(), area.height());
            }
        }
        areas.swap(remaining);
    }
    return areas;
}

static qreal layoutText(
    const data::ContainerNode& node,
    const QRectF& contentRect,
    const std::vector<QRectF>& obstacles,
    data::LayoutResult& result)
{

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setUseDesignMetrics(true);

    qreal lineY = contentRect.top();
    for (const auto& paragraph : node.text.paragraphs)
    {
        const auto& range = paragraph.range;
        const auto& format = paragraph.format;
        lineY += format.spaceBefore;

        const auto baseStyle = mergeTextStyle(
            node.text.defaultStyle, paragraph.defaultStyle);
        const auto baseFormat = makeCharFormat(baseStyle);
        auto layout = std::make_shared<QTextLayout>(
            node.text.text.mid(range.start, range.end - range.start),
            baseFormat.font());
        switch (format.alignment)
        {
        case data::ParagraphAlignment::Left:
            option.setAlignment(Qt::AlignLeft);
            break;
        case data::ParagraphAlignment::Center:
            option.setAlignment(Qt::AlignHCenter);
            break;
        case data::ParagraphAlignment::Right:
            option.setAlignment(Qt::AlignRight);
            break;
        case data::ParagraphAlignment::Justify:
            option.setAlignment(Qt::AlignJustify);
            break;
        }
        layout->setTextOption(option);
        layout->setCacheEnabled(true);

        QList<QTextLayout::FormatRange> formats;
        int next = range.start;

        for (const auto& span : node.text.formatSpans)
        {
            const int start = qMax(range.start, span.range.start);
            const int end = qMin(range.end, span.range.end);
            if (start >= end)
                continue;

            if (next < start)
                formats.push_back({next - range.start, start - next, baseFormat});

            const auto localFormat = makeCharFormat(mergeTextStyle(baseStyle, span.style));
            formats.push_back({start - range.start, end - start, localFormat});
            next = end;
        }

        if (next < range.end)
            formats.push_back({next - range.start, range.end - next, baseFormat});

        layout->setFormats(formats);

        const int layoutIndex = static_cast<int>(result.textLayouts.size());
        int lineIndex = 0;
        layout->beginLayout();

        QTextLine pendingLine;
        bool finished = false;
        while (!finished)
        {
            const qreal indent = format.leftIndent
                                 + (lineIndex == 0 ? format.firstLineIndent : 0);
            const qreal lineWidth = contentRect.width() - indent - format.rightIndent;
            // 缩进用尽整行时，借用未缩进的宽度，避免生成零宽片段。
            const QRectF lineArea(
                contentRect.left() + (lineWidth > 0 ? indent : 0), lineY,
                lineWidth > 0 ? lineWidth : contentRect.width(), format.lineHeight);
            const auto areas = availableTextAreas(lineArea, obstacles);
            const bool obstructed = areas.size() != 1 || areas.front() != lineArea;

            const int firstLineOnRow = lineIndex;
            for (const auto& area : areas)
            {
                if (!pendingLine.isValid())
                    pendingLine = layout->createLine();
                if (!pendingLine.isValid())
                {
                    finished = true;
                    break;
                }

                pendingLine.setLineWidth(area.width());
                // Qt 允许空行接收超宽文字。绕排时保留原文起点，换个空隙重试。
                // 整行都可用时允许单个放不下的字形组溢出，保证排版继续。
                if (obstructed && pendingLine.naturalTextWidth() > area.width())
                    continue;
                pendingLine.setPosition(area.topLeft());

                const int start = range.start + pendingLine.textStart();
                const int end = start + pendingLine.textLength();
                result.textFragments.push_back({
                    node.id, {start, end}, area, layoutIndex, lineIndex
                });
                result.layoutBounds = result.layoutBounds.united(area);
                ++lineIndex;
                pendingLine = QTextLine();

                if (end >= range.end)
                {
                    finished = true;
                    break;
                }
            }

            if (!finished || lineIndex != firstLineOnRow)
                lineY += format.lineHeight;
        }
        layout->endLayout();
        result.textLayouts.push_back(layout);
        lineY += format.spaceAfter;
    }

    return lineY;
}

static qreal layoutChildren(
    const data::DocumentSnapshot& document,
    const std::vector<data::ContainerNode>& children,
    const QRectF& parentContent,
    const data::ContainerId& parentId,
    data::LayoutResult& result)
{
    qreal previousOuterBottom = parentContent.top();

    qreal furthestOuterBottom = parentContent.top();

    for (const auto& node : children)
    {
        const auto& geometry = node.geometry;
        const auto& padding = geometry.padding;
        const auto& margin = geometry.margin;

        const bool isFloating = node.placement == data::PlacementMode::Floating;
        qreal x = parentContent.left() + geometry.position.x();
        qreal y = parentContent.top() + geometry.position.y();

        if (!isFloating)
        {
            x = qMax(x, parentContent.left() + margin.left());
            y = qMax(y, previousOuterBottom + margin.top());
        }

        qreal width = geometry.width == 0
                          ? parentContent.width() - margin.left() - margin.right()
                          : geometry.width;

        qreal initialHeight = qMax(
            geometry.minimumHeight, padding.top() + padding.bottom());

        if (node.type == data::ContainerType::Image)
        {
            width = node.image.displaySize.width()
            + padding.left() + padding.right();

            const qreal imageFrameHeight = node.image.displaySize.height()
                                           + padding.top() + padding.bottom();

            initialHeight = qMax(initialHeight, imageFrameHeight);
        }

        data::ContainerLayout boxLayout;
        boxLayout.containerId = node.id;
        boxLayout.parentId = parentId;
        boxLayout.rect = QRectF(x, y, width, initialHeight);
        boxLayout.contentRect = boxLayout.rect.adjusted(
            padding.left(), padding.top(), -padding.right(), -padding.bottom());

        // 先给父容器留一个位置，让输出里父容器排在子容器前面。
        const auto boxIndex = result.containers.size();
        result.containers.emplace_back();

        qreal contentBottom = boxLayout.contentRect.top();
        if (!node.children.empty())
        {
            contentBottom = layoutChildren(
                document, node.children, boxLayout.contentRect, node.id, result);
        }
        if (node.type == data::ContainerType::Text)
        {
            std::vector<QRectF> obstacles;
            for (auto i = boxIndex + 1; i < result.containers.size(); ++i)
            {
                const auto& childBox = result.containers[i];
                if (childBox.parentId == node.id)
                    obstacles.push_back(childBox.outerRect);
            }

            const qreal textBottom = layoutText(
                node, boxLayout.contentRect, obstacles, result);
            contentBottom = qMax(contentBottom, textBottom);
        }

        const qreal requiredHeight = contentBottom + padding.bottom() - y;
        const qreal finalHeight = qMax(initialHeight, requiredHeight);
        boxLayout.rect.setHeight(finalHeight);
        boxLayout.contentRect.setHeight(
            finalHeight - padding.top() - padding.bottom());

        boxLayout.outerRect = boxLayout.rect.adjusted(
            -margin.left(), -margin.top(), margin.right(), margin.bottom());
        if (node.type == data::ContainerType::Image)
        {
            const auto sourceImage =
                document.resources.value(node.image.resourceId);

            data::ImageLayout imageLayout;
            imageLayout.containerId = node.id;
            imageLayout.resourceId = node.image.resourceId;
            imageLayout.sourceRect = QRectF(
                QPointF(0, 0), QSizeF(sourceImage.size()));
            imageLayout.rect = QRectF(
                boxLayout.contentRect.topLeft(), node.image.displaySize);

            result.images.push_back(imageLayout);
        }
        result.containers[boxIndex] = boxLayout;
        result.layoutBounds = result.layoutBounds.united(boxLayout.outerRect);
        furthestOuterBottom = qMax(furthestOuterBottom, boxLayout.outerRect.bottom());
        if (!isFloating)
            previousOuterBottom = boxLayout.outerRect.bottom();
    }

    return furthestOuterBottom;
}

data::LayoutResult composeLayout(const data::DocumentSnapshot& document)
{
    data::LayoutResult result;

    // 输出结果对应输入文档的同一版本。
    result.documentRevision = document.revision;

    // 纸张左上角为 (0, 0)，尺寸来自文档设置。
    result.pageRect = QRectF(QPointF(0, 0), document.page.size);

    // 读取页边距，计算正文区域。
    const auto& margins = document.page.margins;

    qreal contentWidth = document.page.size.width()
                         - margins.left() - margins.right();

    qreal contentHeight = document.page.size.height()
                          - margins.top() - margins.bottom();

    result.contentRect = QRectF(
        margins.left(),
        margins.top(),
        contentWidth,
        contentHeight
        );

    layoutChildren(
        document,document.root.children, result.contentRect, {}, result);
    return result;
}

} // namespace editor::layout
// 排版模块的工作区。
// 读取 DocumentSnapshot，生成同一版本的 LayoutResult。
// 负责单栏盒模型、同级避让、固定子容器障碍、文字流动绕排与弹性高度。
// 使用字符格式和段落设置排字，输出原文范围及可供绘制、光标定位的 Qt 文字布局。
// 超页内容继续排版，layoutBounds 覆盖完整结果。
// 算法、内部类和方法由模块负责人设计。
