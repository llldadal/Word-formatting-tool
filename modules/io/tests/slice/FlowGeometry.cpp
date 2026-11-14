#include "FlowGeometry.h"

#include <QFont>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTextLayout>
#include <QTextLine>

#include <algorithm>
#include <memory>
#include <vector>

namespace editor::slice {
namespace {

using data::ContainerId;
using data::ContainerLayout;
using data::ContainerNode;
using data::ImageLayout;
using data::LayoutResult;
using data::TextFragment;

// 顺序流的一个块：计算框、填几何结果，并把游标 y 推到下一块起点。
struct Projector {
    const data::DocumentSnapshot& doc;
    LayoutResult out;

    // 用 QTextLayout 排出真实的行对象，供 hitTest / caretRect / selectionRects 使用。
    // 返回文字占用的高度；content 是扣除 padding 后的内容区（纸张坐标）。
    qreal layoutText(const ContainerNode& node, const QRectF& content)
    {
        const auto& style = node.text.defaultStyle;
        QFont font(style.fontFamily, qMax(1, qRound(style.fontSizePt)));
        font.setBold(style.bold);
        font.setItalic(style.italic);

        auto textLayout = std::make_shared<QTextLayout>(node.text.text, font);
        const int layoutIndex = static_cast<int>(out.textLayouts.size());
        out.textLayouts.push_back(std::shared_ptr<const QTextLayout>(textLayout));

        textLayout->beginLayout();
        qreal y = content.top();
        int lineIndex = 0;
        while (lineIndex < 4096) {
            QTextLine line = textLayout->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(qMax<qreal>(1.0, content.width()));
            line.setPosition(QPointF(content.left(), y));

            TextFragment fragment;
            fragment.containerId = node.id;
            fragment.sourceRange = {line.textStart(),
                                    line.textStart() + line.textLength()};
            fragment.rect = QRectF(line.position().x(), line.position().y(),
                                   line.width(), qMax<qreal>(1.0, line.height()));
            fragment.layoutIndex = layoutIndex;
            fragment.lineIndex = lineIndex;
            out.textFragments.push_back(fragment);

            y += qMax<qreal>(1.0, line.height());
            ++lineIndex;
        }
        textLayout->endLayout();
        return y - content.top();
    }

    // 浮动子容器：位置相对父内容区左上角，纸张坐标直接写入结果。
    void projectFloatingChild(const ContainerNode& child, const ContainerId& parentId,
                              const QRectF& parentContent, qreal* requiredBottom)
    {
        const auto& g = child.geometry;
        const QPointF origin(parentContent.left() + g.position.x(),
                            parentContent.top() + g.position.y());

        qreal width = g.width > 0 ? g.width : 220.0;
        qreal height = 0.0;
        QRectF imageContent;

        if (child.type == data::ContainerType::Image) {
            width = child.image.displaySize.width()
                  + g.padding.left() + g.padding.right();
            height = child.image.displaySize.height()
                   + g.padding.top() + g.padding.bottom();
            imageContent = QRectF(origin.x() + g.padding.left(),
                                  origin.y() + g.padding.top(),
                                  child.image.displaySize.width(),
                                  child.image.displaySize.height());
        } else {
            const QRectF inner(origin.x() + g.padding.left(),
                               origin.y() + g.padding.top(),
                               qMax<qreal>(1.0,
                                           width - g.padding.left() - g.padding.right()),
                               100000.0);
            height = layoutText(child, inner)
                   + g.padding.top() + g.padding.bottom();
        }
        height = qMax(height, g.minimumHeight);
        const QRectF rect(origin, QSizeF(width, height));

        ContainerLayout box;
        box.containerId = child.id;
        box.parentId = parentId;
        box.rect = rect;
        box.contentRect = QRectF(rect.left() + g.padding.left(),
                                 rect.top() + g.padding.top(),
                                 qMax<qreal>(0.0,
                                             rect.width() - g.padding.left()
                                                 - g.padding.right()),
                                 qMax<qreal>(0.0,
                                             rect.height() - g.padding.top()
                                                 - g.padding.bottom()));
        box.outerRect = rect.adjusted(-g.margin.left(), -g.margin.top(),
                                      g.margin.right(), g.margin.bottom());
        out.containers.push_back(box);

        if (child.type == data::ContainerType::Image) {
            ImageLayout image;
            image.containerId = child.id;
            image.resourceId = child.image.resourceId;
            const auto it = doc.resources.constFind(child.image.resourceId);
            if (it != doc.resources.constEnd())
                image.sourceRect = QRectF(0, 0, it->width(), it->height());
            image.rect = imageContent;
            out.images.push_back(image);
        }

        *requiredBottom = qMax(*requiredBottom,
                               g.position.y() + height + g.margin.bottom());
    }

    // 顺序流节点。返回本块高度，并把结果写入 out。
    qreal projectBlock(const ContainerNode& node, const ContainerId& parentId,
                       qreal left, qreal available, qreal top)
    {
        const auto& g = node.geometry;
        qreal width = g.width > 0 ? g.width : available;
        qreal textHeight = 0.0;
        qreal childrenBottom = 0.0;

        if (node.type == data::ContainerType::Group) {
            // 分组内的子节点按纵向顺序排，组内左右内边距作为子块额外缩进。
            const qreal innerLeft = left + g.padding.left();
            const qreal innerWidth = qMax<qreal>(1.0,
                width - g.padding.left() - g.padding.right());
            qreal y = top + g.padding.top();
            for (const auto& child : node.children) {
                y += child.geometry.margin.top();
                const qreal h = projectBlock(child, node.id, innerLeft,
                                             innerWidth, y);
                y += h + child.geometry.margin.bottom();
            }
            textHeight = y - (top + g.padding.top());
        } else {
            // 先建容器框，再用其内容区排文字；浮动子容器相对该内容区定位。
            const QRectF content(left + g.padding.left(), top + g.padding.top(),
                                 qMax<qreal>(1.0,
                                             width - g.padding.left()
                                                 - g.padding.right()),
                                 100000.0);
            if (node.type == data::ContainerType::Text)
                textHeight = layoutText(node, content);
            else
                textHeight = node.image.displaySize.height();

            for (const auto& child : node.children)
                projectFloatingChild(child, node.id, content, &childrenBottom);
        }

        qreal height = g.padding.top() + textHeight + g.padding.bottom();
        height = qMax(height, g.minimumHeight);
        height = qMax(height, g.padding.top() + childrenBottom + g.padding.bottom());
        if (node.type == data::ContainerType::Image)
            width = node.image.displaySize.width()
                  + g.padding.left() + g.padding.right();

        const QRectF rect(left, top, width, height);
        ContainerLayout box;
        box.containerId = node.id;
        box.parentId = parentId;
        box.rect = rect;
        box.contentRect = QRectF(rect.left() + g.padding.left(),
                                 rect.top() + g.padding.top(),
                                 qMax<qreal>(0.0,
                                             rect.width() - g.padding.left()
                                                 - g.padding.right()),
                                 qMax<qreal>(0.0,
                                             rect.height() - g.padding.top()
                                                 - g.padding.bottom()));
        box.outerRect = rect.adjusted(-g.margin.left(), -g.margin.top(),
                                      g.margin.right(), g.margin.bottom());
        out.containers.push_back(box);

        if (node.type == data::ContainerType::Image) {
            ImageLayout image;
            image.containerId = node.id;
            image.resourceId = node.image.resourceId;
            const auto it = doc.resources.constFind(node.image.resourceId);
            if (it != doc.resources.constEnd())
                image.sourceRect = QRectF(0, 0, it->width(), it->height());
            image.rect = box.contentRect;
            out.images.push_back(image);
        }
        return height;
    }
};

} // 匿名命名空间结束

data::LayoutResult projectLayout(const data::DocumentSnapshot& doc)
{
    Projector projector{doc, {}};
    LayoutResult& out = projector.out;
    out.documentRevision = doc.revision;

    const auto& page = doc.page;
    out.pageRect = QRectF(0, 0, page.size.width(), page.size.height());
    out.contentRect = QRectF(page.margins.left(), page.margins.top(),
                             qMax<qreal>(1.0,
                                         page.size.width() - page.margins.left()
                                             - page.margins.right()),
                             qMax<qreal>(1.0,
                                         page.size.height() - page.margins.top()
                                             - page.margins.bottom()));

    qreal y = out.contentRect.top();
    for (const auto& child : doc.root.children) {
        y += child.geometry.margin.top();
        const qreal h = projector.projectBlock(child, ContainerId{},
                                               out.contentRect.left(),
                                               out.contentRect.width(), y);
        y += h + child.geometry.margin.bottom();
    }

    out.layoutBounds = out.pageRect;
    for (const auto& c : out.containers) out.layoutBounds |= c.outerRect;
    for (const auto& i : out.images) out.layoutBounds |= i.rect;
    for (const auto& f : out.textFragments) out.layoutBounds |= f.rect;
    return out;
}

} // 命名空间 editor::slice 结束
