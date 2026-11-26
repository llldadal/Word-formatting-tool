#pragma once

#include "DocumentSnapshot.h"

#include <QByteArray>
#include <QImage>
#include <QMimeData>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

namespace editor::interaction {

inline constexpr const char* kRichTextMimeType = "application/x-qtdoceditor-richtext+json";
inline constexpr const char* kImageMimeType = "application/x-qtdoceditor-image+json";

// 选区字符格式聚合结果：字段全选区一致时有值，多格式混合时为 std::nullopt。
struct SelectionStyleSummary {
    bool valid = false;
    std::optional<QString> fontFamily;
    std::optional<qreal> fontSizePt;
    std::optional<QColor> color;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
};

// 选区/光标覆盖段落的属性聚合结果：一致时有值，混合时为 std::nullopt。
struct ParagraphSelectionSummary {
    bool valid = false;
    std::optional<data::ParagraphAlignment> alignment;
    std::optional<data::ParagraphRole> role;
    std::optional<int> headingLevel;
};

// 剪贴板中每段携带的段落元信息。
struct CopiedParagraphMeta {
    data::ParagraphRole role = data::ParagraphRole::Body;
    int headingLevel = 1;
    data::ParagraphFormat format;
    data::TextStyleOverride defaultStyle;
    bool fullParagraph = false;
};

// 内部富文本剪贴板载荷（字符格式区间相对 plainText 起点）。
struct RichTextClipboardPayload {
    QString plainText;
    std::vector<data::TextFormatSpan> formatSpans;
    std::vector<CopiedParagraphMeta> paragraphs;
};

// 内部图片剪贴板载荷。
struct ImageClipboardPayload {
    data::ResourceId sourceResourceId;
    QImage image;
    QSizeF displaySize;
    data::PlacementMode placement = data::PlacementMode::Floating;
    data::ContainerGeometry geometry;
    data::ContainerId sourceParentId;
    data::ContainerId sourceContainerId;
};

// 计算并聚合指定范围的有效字符样式；range.start >= range.end 时按折叠光标位置解析。
SelectionStyleSummary inspectSelectionStyle(const data::DocumentSnapshot& doc,
                                            const data::ContainerId& id,
                                            data::TextRange range);

// 聚合并返回覆盖 focusRange 的段落对齐与角色摘要。
ParagraphSelectionSummary inspectParagraphSelection(const data::DocumentSnapshot& doc,
                                                    const data::ContainerId& id,
                                                    data::TextRange focusRange);

// 提取选区富文本与完整解析后的字符/段落样式。
std::optional<RichTextClipboardPayload> extractRichTextPayload(const data::DocumentSnapshot& doc,
                                                               const data::ContainerId& id,
                                                               data::TextRange range);

QByteArray encodeRichTextPayload(const RichTextClipboardPayload& payload);
std::optional<RichTextClipboardPayload> decodeRichTextPayload(const QByteArray& bytes);
std::unique_ptr<QMimeData> buildRichTextMimeData(const RichTextClipboardPayload& payload);

// 提取选中图片的节点元数据与像素资源。
std::optional<ImageClipboardPayload> extractImagePayload(const data::DocumentSnapshot& doc,
                                                         const data::ContainerId& id);

QByteArray encodeImagePayload(const ImageClipboardPayload& payload);
std::optional<ImageClipboardPayload> decodeImagePayload(const QByteArray& bytes,
                                                        const QImage& fallbackImage = {});
std::unique_ptr<QMimeData> buildImageMimeData(const ImageClipboardPayload& payload);

} // namespace editor::interaction
