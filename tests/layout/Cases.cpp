#include "TestFixtures.h"
#include <QDir>
#include <stdexcept>

using namespace editor::data;

namespace editor::testing {
namespace {

const QString firstParagraph = QStringLiteral(
    "雨停以后，我们沿着图书馆西侧的步道记录地面的变化。入口的碎石带仍然湿润，"
    "低处还留着一片浅水，靠近树下的土面已经露出来。每次观察都从同一个位置拍照，"
    "并把白色标记石留在画面里。记录的重点是水从哪里来、在哪里停留，以及不同位置退水的先后。"
    "为了让下一次观察能与这次对照，我们同时记下时间、天气和水深，读不清的位置保留为空。");
const QString secondParagraph = QStringLiteral(
    "第二次观察安排在午休时。我们沿原来的路线回到入口、低点和排水口，"
    "先核对照片中的参照物，再读取标尺。碎石之间的积水已经退去，低点还剩一条连续的湿痕。"
    "这些记录只说明这场雨后的现场变化。后续还需要记录雨前土壤状态，并把几场降雨的观察按时间排在一起，"
    "才能比较退水速度是否稳定。");

// 建立段落范围，每个换行划分一个段落，段落范围不包含换行符。
ContainerNode textNode(const QString& id, const QString& value, bool title = false)
{
    ContainerNode node;
    node.id = id;
    node.type = ContainerType::Text;
    node.text.text = value;
    node.text.defaultStyle.fontSizePt = title ? 19.5 : 12;
    node.text.defaultStyle.bold = title;
    node.geometry.margin.setBottom(title ? 18 : 14);
    int start = 0;
    for (const auto& part : value.split('\n', Qt::KeepEmptyParts)) {
        ParagraphInfo paragraph;
        paragraph.range = {start, start + int(part.size())};
        paragraph.role = title ? ParagraphRole::Title : ParagraphRole::Body;
        paragraph.format.lineHeight = title ? 40 : 26;
        node.text.paragraphs.push_back(paragraph);
        start += int(part.size()) + 1;
    }
    return node;
}

// 建立图片叶节点。displaySize 是内容尺寸，框尺寸另外包含 padding。
ContainerNode imageNode(const QString& id, const QString& resource,
                        QPointF position, QSizeF size, bool floating = true)
{
    ContainerNode node;
    node.id = id;
    node.type = ContainerType::Image;
    node.placement = floating ? PlacementMode::Floating : PlacementMode::Flow;
    node.geometry.position = position;
    node.geometry.margin = floating ? QMarginsF(12, 7, 12, 7) : QMarginsF(0, 8, 0, 0);
    node.image = {resource, size};
    return node;
}

LayoutCase base(const QString& id, const QString& title)
{
    LayoutCase result{id, title, {}};
    // 不同题使用不同版本号，便于发现把旧排版结果交给画布的问题。
    result.document.revision = 100 + id.toUInt();
    for (const auto& name : {QStringLiteral("figure-1"), QStringLiteral("figure-2")}) {
        const auto path = QDir(QStringLiteral(EDITOR_TEST_ASSET_DIR)).filePath(name + ".png");
        const QImage image(path);
        if (image.isNull()) throw std::runtime_error("Missing fixture image asset");
        result.document.resources.insert(name, image);
    }
    return result;
}

} // namespace

std::vector<LayoutCase> makeLayoutCases()
{
    std::vector<LayoutCase> result;
    {
        auto c = base("01", QStringLiteral("简单单列 标题和两段正文"));
        c.document.root.children = {textNode("title", QStringLiteral("雨后观察记录"), true),
                                   textNode("p1", firstParagraph), textNode("p2", secondParagraph)};
        result.push_back(std::move(c));
    }
    {
        auto c = base("02", QStringLiteral("单图嵌入正文"));
        auto body = textNode("body", firstParagraph + secondParagraph);
        body.children = {imageNode("photo1", "figure-1", {238, 52}, {196, 140})};
        c.document.root.children = {textNode("title", QStringLiteral("居中插图与连续正文"), true), body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("03", QStringLiteral("双图嵌入正文"));
        auto body = textNode("body", firstParagraph + secondParagraph + firstParagraph);
        body.children = {imageNode("photo1", "figure-1", {130, 52}, {144, 130}),
                         imageNode("photo2", "figure-2", {420, 52}, {144, 130})};
        c.document.root.children = {textNode("title", QStringLiteral("两张图片同时参与绕排"), true), body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("04", QStringLiteral("独立图片排在正文之后"));
        c.document.root.children = {textNode("title", QStringLiteral("正文之后的独立配图"), true),
            textNode("p1", firstParagraph), textNode("p2", secondParagraph),
            imageNode("photo1", "figure-1", {246, 0}, {180, 120}, false)};
        result.push_back(std::move(c));
    }
    {
        auto c = base("06", QStringLiteral("右下角嵌套小段落"));
        auto body = textNode("main", firstParagraph + secondParagraph + firstParagraph + secondParagraph);
        body.geometry.minimumHeight = 468;
        auto note = textNode("note", QStringLiteral(
            "现场补记：本次三处读数来自同一场小雨。下次观察沿用标记石和拍摄角度，"
            "并补记雨前土壤的湿润程度，便于比较。"));
        note.placement = PlacementMode::Floating;
        note.geometry.position = {452, 286}; // 固定相对坐标，不使用尚未定义的底部锚定规则。
        note.geometry.width = 220;
        note.geometry.padding = {10, 10, 10, 10};
        note.geometry.margin = {12, 7, 0, 0};
        body.children.push_back(note);
        c.document.root.children = {textNode("title", QStringLiteral("雨后街区观察与现场补记"), true), body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("07", QStringLiteral("多段落和局部格式覆盖"));
        auto body = textNode("rich", QStringLiteral(
            "观察方法\n每次观察都从同一个位置拍照，并记录天气、水深和时间。入口处的照片需要保留参照物。\n"
            "整理数据时，先核对记录，再比较变化。没有读数的位置保持为空，不用估计值代替。"));
        body.geometry.padding = {16, 12, 16, 12};
        body.text.defaultStyle.color = QColor("#253746");
        auto& heading = body.text.paragraphs[0];
        heading.role = ParagraphRole::Heading;
        heading.headingLevel = 2;
        heading.defaultStyle.fontSizePt = 16.5;
        heading.defaultStyle.bold = true;
        heading.format.lineHeight = 34;
        heading.format.spaceAfter = 10;
        auto& p1 = body.text.paragraphs[1];
        p1.format.firstLineIndent = 32;
        p1.format.spaceAfter = 12;
        p1.defaultStyle.italic = true;
        auto& p2 = body.text.paragraphs[2];
        p2.format.leftIndent = 20;
        p2.format.rightIndent = 12;
        p2.format.alignment = ParagraphAlignment::Right;
        const int start = body.text.text.indexOf(QStringLiteral("天气、水深和时间"));
        TextFormatSpan highlighted;
        highlighted.range = {start, start + 8};
        highlighted.style.color = QColor("#b63d2e");
        highlighted.style.bold = true;
        highlighted.style.italic = false; // 显式取消段落斜体，不能当成继承。
        highlighted.style.underline = true;
        body.text.formatSpans.push_back(highlighted);
        c.document.root.children = {body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("08", QStringLiteral("超出单页仍完整排版"));
        c.document.page.size = {520, 420};
        c.document.page.margins = {30, 30, 30, 30};
        auto body = textNode("overflow", firstParagraph + "\n" + secondParagraph + "\n" + firstParagraph);
        body.children = {imageNode("photo1", "figure-2", {240, 390}, {160, 110})};
        c.document.root.children = {body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("09", QStringLiteral("分组盒模型和外边距相加"));
        ContainerNode group;
        group.id = "group";
        group.type = ContainerType::Group;
        group.geometry.width = 620;
        group.geometry.padding = {20, 12, 24, 16};
        group.geometry.margin = {10, 8, 6, 10};
        auto body = textNode("inside", QStringLiteral("分组内的正文占满内容区，内边距留在框内，外边距参与顺序排版。"));
        body.geometry.padding = {8, 6, 8, 6};
        body.geometry.margin = {6, 4, 10, 9};
        auto image = imageNode("photo1", "figure-1", {70, 0}, {180, 100}, false);
        image.geometry.padding = {4, 5, 6, 7};
        image.geometry.margin = {3, 11, 5, 13};
        image.geometry.width = 999; // 图片尺寸只取 displaySize，不能采用此字段。
        group.children = {body, image};
        auto after = textNode("after", QStringLiteral("这一段排在分组之后，两侧外边距不会吞掉文字。"));
        after.geometry.margin = {2, 7, 4, 5};
        after.geometry.minimumHeight = 72;
        c.document.root.children = {group, after};
        result.push_back(std::move(c));
    }
    {
        auto c = base("10", QStringLiteral("UTF16 偏移和空段落"));
        auto body = textNode("unicode", QStringLiteral("中文与 A\U0001F600e\u0301 一起记录。\n\n最后一个段落之后还有空段落。\n"));
        body.geometry.width = 360;
        body.geometry.padding = {12, 8, 12, 8};
        c.document.root.children = {body};
        result.push_back(std::move(c));
    }
    {
        auto c = base("11", QStringLiteral("空文档"));
        c.document.resources.clear();
        result.push_back(std::move(c));
    }
    return result;
}

LayoutCase makeLayoutCase(const QString& caseId)
{
    for (auto& c : makeLayoutCases()) if (c.id == caseId) return std::move(c);
    throw std::invalid_argument("Unknown layout case id");
}

} // namespace editor::testing
