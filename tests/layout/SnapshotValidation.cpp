#include "TestFixtures.h"
#include <QSet>
#include <cmath>
#include <functional>

using namespace editor::data;

namespace editor::testing {

QStringList validateSnapshot(const DocumentSnapshot& source)
{
    QStringList errors;
    QSet<QString> ids;
    const auto finiteNonnegative = [](qreal n) { return std::isfinite(n) && n >= 0; };
    const auto marginsValid = [&](const QMarginsF& m) {
        return finiteNonnegative(m.left()) && finiteNonnegative(m.top()) &&
               finiteNonnegative(m.right()) && finiteNonnegative(m.bottom());
    };
    if (!finiteNonnegative(source.page.size.width()) || source.page.size.width() == 0 ||
        !finiteNonnegative(source.page.size.height()) || source.page.size.height() == 0 ||
        !marginsValid(source.page.margins) ||
        source.page.margins.left() + source.page.margins.right() >= source.page.size.width() ||
        source.page.margins.top() + source.page.margins.bottom() >= source.page.size.height())
        errors << "page: invalid page or margins";
    if (source.root.type != ContainerType::Group) errors << "root: logical root must be Group";

    std::function<void(const ContainerNode&, int, ContainerType)> visit;
    visit = [&](const ContainerNode& node, int depth, ContainerType parentType) {
        const auto path = node.id.isEmpty() ? QStringLiteral("<empty-id>") : node.id;
        if (node.id.isEmpty() || ids.contains(node.id)) errors << path + ": empty or duplicate ID";
        ids.insert(node.id);
        if (depth > 2) errors << path + ": only one level of nesting is allowed below top-level containers";
        if (node.type == ContainerType::Image && !node.children.empty()) errors << path + ": image must be a leaf";
        if (depth > 0 && node.placement != (parentType == ContainerType::Text ? PlacementMode::Floating : PlacementMode::Flow))
            errors << path + ": placement does not match its parent type";
        const auto& g = node.geometry;
        if (!std::isfinite(g.position.x()) || !std::isfinite(g.position.y()) ||
            !finiteNonnegative(g.width) || !finiteNonnegative(g.minimumHeight) ||
            !marginsValid(g.padding) || !marginsValid(g.margin)) errors << path + ": invalid geometry";
        if (node.type == ContainerType::Image) {
            if (!source.resources.contains(node.image.resourceId) || source.resources.value(node.image.resourceId).isNull())
                errors << path + ": missing image resource";
            if (!std::isfinite(node.image.displaySize.width()) || node.image.displaySize.width() <= 0 ||
                !std::isfinite(node.image.displaySize.height()) || node.image.displaySize.height() <= 0)
                errors << path + ": invalid image size";
        }
        if (node.type == ContainerType::Text) {
            const auto& text = node.text.text;
            int next = 0;
            const auto parts = text.split('\n', Qt::KeepEmptyParts);
            if (node.text.paragraphs.size() != size_t(parts.size())) errors << path + ": paragraph count mismatch";
            for (int i = 0; i < int(node.text.paragraphs.size()); ++i) {
                const auto& paragraph = node.text.paragraphs[i];
                if (i >= parts.size() || paragraph.range.start != next || paragraph.range.end != next + parts.value(i).size())
                    errors << path + ": paragraph range must exclude newline";
                next += int(parts.value(i).size()) + 1;
                const auto f = paragraph.format;
                if (!std::isfinite(f.lineHeight) || f.lineHeight <= 0 ||
                    !finiteNonnegative(f.leftIndent) || !finiteNonnegative(f.rightIndent) ||
                    !std::isfinite(f.firstLineIndent) || !finiteNonnegative(f.spaceBefore) || !finiteNonnegative(f.spaceAfter))
                    errors << path + ": invalid paragraph format";
                if (paragraph.role == ParagraphRole::Heading && (paragraph.headingLevel < 1 || paragraph.headingLevel > 9))
                    errors << path + ": invalid heading level";
            }
            int end = 0;
            for (const auto& span : node.text.formatSpans) {
                if (span.range.start < end || span.range.end <= span.range.start || span.range.end > text.size())
                    errors << path + ": overlapping or invalid format range";
                end = span.range.end;
            }
        }
        for (const auto& child : node.children) visit(child, depth + 1, node.type);
    };
    visit(source.root, 0, ContainerType::Group);
    return errors;
}

} // namespace editor::testing
