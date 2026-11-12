#include "LayoutModule.h"

#include <QImage>
#include <QGuiApplication>
#include <QDebug>

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);

    // 创建一份文档，使用结构体里的默认页面设置。
    editor::data::DocumentSnapshot document;

    document.revision = 2;
    document.page.size = QSizeF(1000, 800);
    document.page.margins = QMarginsF(20, 30, 40, 50);

    editor::data::ContainerNode box;
    box.id = QStringLiteral("box1");
    box.type = editor::data::ContainerType::Group;
    box.geometry.position = QPointF(0, 0);
    box.geometry.width = 0;
    box.geometry.minimumHeight = 80;
    box.geometry.padding = QMarginsF(10, 8, 20, 12);
    box.geometry.margin = QMarginsF(5, 6, 7, 8);

    // 创建一张纯色示例图片，放入文档资源表。
    QImage sampleImage(400, 240, QImage::Format_ARGB32);
    sampleImage.fill(Qt::cyan);
    document.resources.insert(QStringLiteral("sample-image"), sampleImage);

    editor::data::ContainerNode child;
    child.id = QStringLiteral("child1");
    child.type = editor::data::ContainerType::Image;
    child.geometry.position = QPointF(10, 10);
    child.geometry.width = 999;
    child.geometry.minimumHeight = 120;
    child.geometry.padding = QMarginsF(4, 5, 6, 7);
    child.geometry.margin = QMarginsF(2, 3, 4, 5);
    child.image.resourceId = QStringLiteral("sample-image");
    child.image.displaySize = QSizeF(200, 120);
    box.children.push_back(child);

    // 将它加入文档最外层的容器列表。
    document.root.children.push_back(box);

    auto secondBox = box;
    secondBox.children.clear();
    secondBox.id = QStringLiteral("box2");
    secondBox.geometry.position = QPointF(0, 0);
    secondBox.geometry.width = 116;
    secondBox.geometry.minimumHeight = 60;
    secondBox.type = editor::data::ContainerType::Text;
    secondBox.text.text = QStringLiteral("甲乙丙丁戊己庚辛壬癸甲乙");

    editor::data::ParagraphInfo paragraph;
    paragraph.range = {0, static_cast<int>(secondBox.text.text.size())};
    paragraph.format.alignment = editor::data::ParagraphAlignment::Center;
    paragraph.format.leftIndent = 8;
    paragraph.format.rightIndent = 10;
    paragraph.format.firstLineIndent = 16;
    paragraph.format.spaceBefore = 6;
    paragraph.format.spaceAfter = 4;
    paragraph.defaultStyle.italic = true;
    secondBox.text.paragraphs.push_back(paragraph);

    editor::data::TextFormatSpan span;
    span.range = {3, 7};
    span.style.bold = true;
    span.style.italic = false;
    span.style.color = QColor(Qt::blue);
    secondBox.text.formatSpans.push_back(span);

    auto floatingImage = child;
    floatingImage.id = QStringLiteral("floating-image");
    floatingImage.placement = editor::data::PlacementMode::Floating;
    floatingImage.geometry.position = QPointF(26, 0);
    floatingImage.geometry.minimumHeight = 0;
    floatingImage.image.displaySize = QSizeF(18, 116);
    secondBox.children.push_back(floatingImage);
    document.root.children.push_back(secondBox);

    // 调用我们刚写的排版函数。
    auto result = editor::layout::composeLayout(document);

    qInfo() << "containers:" << result.containers.size()
            << "images:" << result.images.size()
            << "text fragments:" << result.textFragments.size();

    for (const auto& item : result.containers)
    {
        if (item.containerId == QStringLiteral("box2")
            || item.containerId == QStringLiteral("floating-image"))
        {
            qInfo() << item.containerId << "parent:" << item.parentId
                    << "frame:" << item.rect << "outer:" << item.outerRect;
        }
    }
    return 0;
}