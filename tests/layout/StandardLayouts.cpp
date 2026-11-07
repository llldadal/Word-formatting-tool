#include "TestFixtures.h"
#include "FixtureSupport.h"
#include <QFontDatabase>
#include <initializer_list>
#include <stdexcept>

using namespace editor::data;

namespace editor::testing {
namespace {

struct LineRecord {
    int start;   // 相对整个文本容器的 UTF-16 起点。
    int end;     // 相对整个文本容器的 UTF-16 终点，不包含此位置。
    QRectF rect; // 已冻结的纸张坐标和行高。
};

// 按数值恢复一段的 Qt 行对象，同时检查环境是否仍能重现固定的断行。
void restoreParagraph(LayoutResult& result, const DocumentSnapshot& document,
                      const QString& id, int paragraphIndex, std::initializer_list<LineRecord> records)
{
    const auto& node = detail::findNode(document.root, id);
    const auto& paragraph = node.text.paragraphs.at(paragraphIndex);
    auto layout = detail::makeParagraphLayout(node, paragraphIndex);
    const int poolIndex = int(result.textLayouts.size());
    layout->beginLayout();
    int next = paragraph.range.start;
    int lineIndex = 0;
    for (const auto& record : records) {
        auto line = layout->createLine();
        if (!line.isValid()) throw std::runtime_error("Standard line missing; check Qt and font environment");
        line.setLineWidth(record.rect.width());
        line.setPosition(record.rect.topLeft());
        if (record.start != next || paragraph.range.start + line.textStart() != record.start ||
            line.textLength() != record.end - record.start) {
            throw std::runtime_error("Frozen standard differs from Qt shaping; use Qt 6.12 and Microsoft YaHei");
        }
        result.textFragments.push_back({id, {record.start, record.end}, record.rect, poolIndex, lineIndex++});
        next = record.end;
    }
    if (layout->createLine().isValid()) throw std::runtime_error("Standard paragraph has omitted lines");
    layout->endLayout();
    if (next != paragraph.range.end) throw std::runtime_error("Standard paragraph does not cover its source range");
    result.textLayouts.push_back(std::move(layout));
}

} // namespace

LayoutResult makeStandardLayout(const QString& caseId)
{
    if (!QFontDatabase::families().contains("Microsoft YaHei"))
        throw std::runtime_error("Microsoft YaHei is unavailable; on Windows use QT_QPA_PLATFORM=windows");
    const auto fixture = makeLayoutCase(caseId);
    const auto& document = fixture.document;
    LayoutResult result;
    result.documentRevision = document.revision;
    // 本文件不计算容器位置、可用区域或新断行，只恢复已经固定的记录。
#include "StandardRecords.inc"
    throw std::invalid_argument("Unknown standard layout case id");
}

} // namespace editor::testing
