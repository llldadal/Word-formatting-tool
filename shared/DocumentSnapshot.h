#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QMarginsF>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <vector>

namespace editor::data {

using ContainerId = QString; // 文档内稳定且唯一的容器标识。
using ResourceId = QString;  // 文档资源表中原始图片的标识。

// 文档几何量使用逻辑像素，96 个单位对应一英寸。屏幕缩放由 UI 处理。
inline constexpr qreal DocumentUnitsPerInch = 96.0;

// 文本位置采用 UTF-16 偏移，范围为 [start, end)，start == end 表示空范围。
struct TextRange {
    int start = 0; // 起始偏移，包含此位置。
    int end = 0;   // 结束偏移，排除此位置。
};

enum class ContainerType {
    Text,  // 持有连续正文的文本容器。
    Image, // 引用图片资源的图片容器。
    Group  // 按纵向顺序组织其他容器的分组。
};

enum class PlacementMode {
    Flow,    // 参与父级顺序排版。
    Floating // 位置相对父内容区固定，作为父正文绕排的障碍。
};

enum class ParagraphRole {
    Body,    // 普通正文。
    Title,   // 整篇文档的标题。
    Heading  // 章节标题，由 headingLevel 表示级别。
};

enum class ParagraphAlignment {
    Left,    // 左对齐。
    Center,  // 居中。
    Right,   // 右对齐。
    Justify  // 两端对齐。
};

// 文本容器的完整默认字符格式。字号使用 pt，几何量仍使用文档逻辑像素。
struct TextStyle {
    QString fontFamily = QStringLiteral("Microsoft YaHei"); // 字体名称。
    qreal fontSizePt = 12;                                  // 字号，单位为点。
    QColor color = QColor(Qt::black);                       // 字符颜色。
    bool bold = false;                                     // 是否加粗。
    bool italic = false;                                   // 是否使用斜体。
    bool underline = false;                                // 是否绘制下划线。
};

// 段落或字符区间对默认格式的局部覆盖。无值表示继承，false 表示显式取消该格式。
struct TextStyleOverride {
    std::optional<QString> fontFamily; // 覆盖字体名称。
    std::optional<qreal> fontSizePt;   // 覆盖字号。
    std::optional<QColor> color;      // 覆盖字符颜色。
    std::optional<bool> bold;         // 覆盖粗体状态。
    std::optional<bool> italic;       // 覆盖斜体状态。
    std::optional<bool> underline;    // 覆盖下划线状态。
};

// 作用于一段连续文字的格式覆盖。文档模块输出的区间按起点排序，彼此无重叠。
struct TextFormatSpan {
    TextRange range;          // 相对所属文本容器的 UTF-16 范围。
    TextStyleOverride style;  // 此范围需要覆盖的格式。
};

// 段落排版设置，长度均使用文档逻辑像素。
struct ParagraphFormat {
    ParagraphAlignment alignment = ParagraphAlignment::Left; // 水平对齐方式。
    qreal leftIndent = 0;                                    // 左侧缩进。
    qreal rightIndent = 0;                                   // 右侧缩进。
    qreal firstLineIndent = 0;                               // 在左缩进基础上的首行缩进。
    qreal lineHeight = 26;                                   // 固定行高。
    qreal spaceBefore = 0;                                  // 段落之前的间距。
    qreal spaceAfter = 0;                                   // 段落之后的间距。
};

// 文本中的一个段落，包含语义和样式。range 不包含段末的换行符。
struct ParagraphInfo {
    TextRange range;                             // 相对整个文本容器的段落范围。
    ParagraphRole role = ParagraphRole::Body;     // 正文、文档标题或章节标题。
    int headingLevel = 1;                        // Heading 的级别，v0.1 使用 1 到 9。
    ParagraphFormat format;                      // 段落对齐、缩进和间距。
    TextStyleOverride defaultStyle;              // 该段对容器默认字符格式的覆盖。
};

// 一个文本容器只有一份连续文本，段落和格式区间都引用它的 UTF-16 位置。
// 字符格式按容器默认、段落默认、局部区间的顺序覆盖。
struct TextContent {
    QString text;                          // 完整正文，段落之间使用 '\n' 分隔。
    TextStyle defaultStyle;                // 容器默认字符格式。
    std::vector<ParagraphInfo> paragraphs; // 每个段落的信息，按起点排序。
    std::vector<TextFormatSpan> formatSpans; // 局部格式区间。
};

// 图片内容。原图存放在文档资源表中，displaySize 表示图片内容区的目标大小。
struct ImageContent {
    ResourceId resourceId; // 原图在资源表中的 ID。
    QSizeF displaySize;   // 目标宽、高，单位为文档逻辑像素。
};

// 盒模型约束。边距为非负值，相邻顺序容器的外边距相加。
struct ContainerGeometry {
    QPointF position;          // 相对父容器内容区的期望左上角。
    qreal width = 0;           // 文本或分组的框宽，含 padding，0 表示填满父内容区。
    qreal minimumHeight = 0;   // 框高下限，实际高度还要容纳正文、子容器和底部 padding。
    QMarginsF padding;         // 容器框边缘到内容区的距离。
    QMarginsF margin;          // 周围内容需要避让的四边距离。
};

// 公共容器节点。type 决定使用 text、image 或分组数据。
// 图片框尺寸由 image.displaySize 加 padding 得到，图片节点的 geometry.width 不参与尺寸计算。
// 逻辑根下的节点为顶层容器，顶层可以包含一层子容器，所有子容器的 children 必须为空。
struct ContainerNode {
    ContainerId id;                                   // 文档内唯一 ID。
    ContainerType type = ContainerType::Group;         // 内容类型。
    PlacementMode placement = PlacementMode::Flow;     // 顺序排版或固定障碍。
    ContainerGeometry geometry;                       // 用户设定的位置、尺寸及边距约束。
    TextContent text;                                 // Text 节点的正文和格式。
    ImageContent image;                               // Image 节点的资源引用与尺寸。
    std::vector<ContainerNode> children;               // 按文档顺序保存的直接子节点。
};

// v0.1 使用固定大小的单页，正文单栏排版，排版结果可以延伸到纸张区域之外。
struct PageSettings {
    QSizeF size{760, 1000};             // 纸张宽、高，单位为文档逻辑像素。
    QMarginsF margins{44, 44, 44, 44}; // 纸张边缘到正文区的距离。
};

// 公共数据一。文档模块提供某一版本的容器树，排版和导出按只读快照使用。
// 顶层正文纵向排列，文本容器中的子节点使用 Floating，其位置固定于父内容区。
// 分组内的顺序节点使用 Flow，所有普通分组同样遵守一层嵌套的限制。
// 文档模块负责 ID 唯一性、深度限制、段落范围、格式区间和资源引用的有效性。
// 每个 '\n' 划分一个段落，段落范围排除分隔符。空段落可用空范围表示。
struct DocumentSnapshot {
    quint64 revision = 1;                              // 文档版本，成功修改后推进。
    PageSettings page;                                 // 纸张和页边距。
    QHash<ResourceId, QImage> resources;                // 按 ID 查找原始图片。
    ContainerNode root{QStringLiteral("root"), ContainerType::Group}; // 逻辑根，深度计数时忽略。
};

}
