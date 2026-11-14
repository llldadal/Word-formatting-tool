#include "LocalDocumentExecutor.h"

#include <algorithm>

namespace editor::slice {
namespace {

using data::ContainerId;
using data::ContainerNode;
using data::ParagraphInfo;
using data::ParagraphRole;
using data::TextContent;
using data::TextFormatSpan;
using data::TextRange;
using data::TextStyleOverride;

ContainerNode* findNode(ContainerNode& node, const ContainerId& id)
{
    if (node.id == id) return &node;
    for (auto& child : node.children)
        if (ContainerNode* found = findNode(child, id)) return found;
    return nullptr;
}

TextRange clampRange(const TextContent& content, TextRange range)
{
    const int len = static_cast<int>(content.text.size());
    const int a = qBound(0, range.start, len);
    const int b = qBound(0, range.end, len);
    return {qMin(a, b), qMax(a, b)};
}

// 段落语义在文本被改写前的存档，重建段落时按位置继承。
struct ParagraphSeed {
    TextRange range;
    ParagraphRole role = ParagraphRole::Body;
    int headingLevel = 1;
    data::ParagraphFormat format;
    TextStyleOverride defaultStyle;
};

// 一次文本替换：[start, end) 变为长度 inserted 的文本。
struct Mutation {
    int start = 0;
    int end = 0;
    int inserted = 0;

    int delta() const { return inserted - (end - start); }

    // 旧偏移端点映射到新文本。被删除区间内的端点夹到 start。
    int forward(int p) const
    {
        if (p <= start) return p;
        if (p >= end) return p + delta();
        return start;
    }

    // 新偏移反查旧文本中用于继承语义的位置。
    int backward(int p) const
    {
        if (p <= start) return p;
        if (p >= start + inserted) return p - delta();
        return start;
    }
};

// 区间两两不重叠、按起点排序，符合 DocumentSnapshot 对 formatSpans 的约定。
void normalizeSpans(std::vector<TextFormatSpan>* spans)
{
    std::sort(spans->begin(), spans->end(), [](const TextFormatSpan& a,
                                               const TextFormatSpan& b) {
        return a.range.start < b.range.start;
    });
    std::vector<TextFormatSpan> out;
    for (auto& span : *spans) {
        if (span.range.start >= span.range.end) continue;
        if (!out.empty() && out.back().range.end > span.range.start)
            out.back().range.end = span.range.start; // 后写入的覆盖区间优先。
        if (!out.empty() && out.back().range.start >= out.back().range.end)
            out.pop_back();
        out.push_back(span);
    }
    spans->swap(out);
}

// 覆盖式加入一段字符格式：先裁掉被覆盖的旧区间，再写入新区间。
void addFormatSpan(TextContent& content, TextRange range,
                   const TextStyleOverride& style)
{
    const TextRange r = clampRange(content, range);
    if (r.start >= r.end) return;
    std::vector<TextFormatSpan> next;
    for (const auto& span : content.formatSpans) {
        if (span.range.end <= r.start || span.range.start >= r.end) {
            next.push_back(span);
            continue;
        }
        if (span.range.start < r.start)
            next.push_back({{span.range.start, r.start}, span.style});
        if (span.range.end > r.end)
            next.push_back({{r.end, span.range.end}, span.style});
    }
    next.push_back({r, style});
    normalizeSpans(&next);
    content.formatSpans = std::move(next);
}

// 用新的 '\n' 分段重建段落，语义从改写前覆盖该位置的段落继承。
void rebuildParagraphs(TextContent& content, const QString& oldText,
                       const std::vector<ParagraphSeed>& seeds,
                       const Mutation& mutation)
{
    const QString& text = content.text;
    std::vector<ParagraphInfo> rebuilt;
    int start = 0;
    while (start <= text.size()) {
        const int nl = text.indexOf(QLatin1Char('\n'), start);
        const int end = nl < 0 ? text.size() : nl;

        const int oldAt = mutation.backward(start);
        const ParagraphSeed* seed = nullptr;
        for (const auto& candidate : seeds) {
            if (oldAt >= candidate.range.start && oldAt <= candidate.range.end) {
                seed = &candidate;
                break;
            }
        }
        ParagraphInfo info;
        info.range = {start, end};
        if (seed) {
            info.role = seed->role;
            info.headingLevel = seed->headingLevel;
            info.format = seed->format;
            info.defaultStyle = seed->defaultStyle;
        } else {
            info.role = ParagraphRole::Body;
            info.headingLevel = 1;
        }
        rebuilt.push_back(info);
        if (nl < 0) break;
        start = nl + 1;
    }
    content.paragraphs = std::move(rebuilt);
    Q_UNUSED(oldText)
}

// 执行 Insert / Replace / Delete：改写正文并重排段落与格式区间。
void mutateText(TextContent& content, TextRange requested, const QString& replacement)
{
    const TextRange range = clampRange(content, requested);
    const QString oldText = content.text;
    std::vector<ParagraphSeed> seeds;
    seeds.reserve(content.paragraphs.size());
    for (const auto& p : content.paragraphs)
        seeds.push_back({p.range, p.role, p.headingLevel, p.format, p.defaultStyle});

    const Mutation mutation{range.start, range.end,
                            static_cast<int>(replacement.size())};
    QString next = oldText;
    next.replace(range.start, range.end - range.start, replacement);
    content.text = next;

    rebuildParagraphs(content, oldText, seeds, mutation);

    std::vector<TextFormatSpan> spans;
    for (const auto& span : content.formatSpans) {
        const int a = mutation.forward(span.range.start);
        const int b = mutation.forward(span.range.end);
        if (a < b) spans.push_back({{a, b}, span.style});
    }
    content.formatSpans = std::move(spans);
    normalizeSpans(&content.formatSpans);
}

bool rangeTouchesParagraph(TextRange selection, TextRange paragraph)
{
    if (paragraph.start == paragraph.end)
        return selection.start <= paragraph.start && paragraph.start <= selection.end;
    return paragraph.start < selection.end && paragraph.end > selection.start;
}

void applyParagraphFormat(TextContent& content,
                          const interaction::EditRequest& request)
{
    const TextRange range = clampRange(content, request.range);
    for (auto& p : content.paragraphs) {
        if (!rangeTouchesParagraph(range, p.range)) continue;
        if (request.paragraphFormat) p.format = *request.paragraphFormat;
        if (request.role) {
            p.role = *request.role;
            p.headingLevel = (*request.role == ParagraphRole::Heading)
                                 ? qBound(1, request.headingLevel.value_or(1), 9)
                                 : 1;
        } else if (request.headingLevel && p.role == ParagraphRole::Heading) {
            p.headingLevel = qBound(1, *request.headingLevel, 9);
        }
    }
}

} // 匿名命名空间结束

ApplyResult applyRequests(const data::DocumentSnapshot& doc,
                          const std::vector<interaction::EditRequest>& requests)
{
    ApplyResult result;
    result.document = doc;
    for (const auto& request : requests) {
        ContainerNode* node = findNode(result.document.root, request.containerId);
        if (!node || node->type != data::ContainerType::Text) {
            result.ok = false;
            result.error = QStringLiteral("编辑指向了不存在的文本容器：%1")
                               .arg(request.containerId);
            return result;
        }
        TextContent& content = node->text;
        switch (request.op) {
        case interaction::EditOp::Insert:
            mutateText(content, {request.range.start, request.range.start}, request.text);
            break;
        case interaction::EditOp::Replace:
            mutateText(content, request.range, request.text);
            break;
        case interaction::EditOp::Delete:
            mutateText(content, request.range, QString());
            break;
        case interaction::EditOp::SetCharFormat:
            addFormatSpan(content, request.range, request.charStyle);
            break;
        case interaction::EditOp::SetParagraphFormat:
            applyParagraphFormat(content, request);
            break;
        }
    }
    if (!requests.empty()) ++result.document.revision;
    return result;
}

} // 命名空间 editor::slice 结束
