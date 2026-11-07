#include "TestFixtures.h"
#include <QHash>
#include <QSet>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace editor::data;

namespace editor::testing {

QStringList validateLayoutStructure(const DocumentSnapshot& source, const LayoutResult& result, qreal tolerance)
{
    QStringList errors;
    QHash<QString, const ContainerNode*> nodes;
    QHash<QString, QString> parents;
    std::function<void(const ContainerNode&, const QString&)> collect = [&](const auto& parent, const auto& parentId) {
        for (const auto& child : parent.children) {
            nodes.insert(child.id, &child);
            parents.insert(child.id, parentId);
            collect(child, child.id);
        }
    };
    collect(source.root, {});
    const auto fail = [&](const QString& path, const QString& why) {
        if (errors.size() < 80) errors << path + ": " + why;
    };
    const auto close = [&](qreal a, qreal b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; };
    const auto rectClose = [&](const QRectF& a, const QRectF& b) {
        return close(a.x(), b.x()) && close(a.y(), b.y()) && close(a.width(), b.width()) && close(a.height(), b.height());
    };
    const auto positive = [](const QRectF& r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height()) && r.width() > 0 && r.height() > 0;
    };
    const auto overlap = [&](const QRectF& a, const QRectF& b) {
        const auto intersection = a.intersected(b);
        return intersection.width() > tolerance && intersection.height() > tolerance;
    };
    const QRectF page(QPointF(), source.page.size);
    const auto m = source.page.margins;
    if (result.documentRevision != source.revision) fail("documentRevision", "does not match source snapshot");
    if (!rectClose(result.pageRect, page)) fail("pageRect", "must use paper origin (0, 0) and source size");
    if (!rectClose(result.contentRect, page.adjusted(m.left(), m.top(), -m.right(), -m.bottom()))) fail("contentRect", "page margins differ");

    QHash<QString, const ContainerLayout*> boxes;
    QRectF bounds;
    bool hasBounds = false;
    const auto includeBounds = [&](const QRectF& r) {
        bounds = hasBounds ? bounds.united(r) : r;
        hasBounds = true;
    };
    for (const auto& box : result.containers) {
        if (boxes.contains(box.containerId)) fail(box.containerId, "duplicate container box");
        boxes.insert(box.containerId, &box);
        if (!nodes.contains(box.containerId)) { fail(box.containerId, "container does not exist in source"); continue; }
        const auto& node = *nodes.value(box.containerId);
        const auto p = node.geometry.padding;
        const auto margins = node.geometry.margin;
        if (!positive(box.rect) || !positive(box.contentRect)) fail(node.id, "container rectangles must be finite and positive");
        if (box.parentId != parents.value(node.id)) fail(node.id, "wrong parent ID");
        if (!rectClose(box.contentRect, box.rect.adjusted(p.left(), p.top(), -p.right(), -p.bottom()))) fail(node.id, "padding does not match contentRect");
        if (!rectClose(box.outerRect, box.rect.adjusted(-margins.left(), -margins.top(), margins.right(), margins.bottom()))) fail(node.id, "margin does not match outerRect");
        if (box.rect.height() + tolerance < node.geometry.minimumHeight) fail(node.id, "minimumHeight was ignored");
        includeBounds(box.outerRect);
    }
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        if (!boxes.contains(it.key())) fail(it.key(), "missing container box");
    QSet<QString> images;
    for (const auto& image : result.images) {
        if (images.contains(image.containerId)) fail(image.containerId, "duplicate image");
        images.insert(image.containerId);
        if (!nodes.contains(image.containerId) || nodes.value(image.containerId)->type != ContainerType::Image) {
            fail(image.containerId, "image does not belong to an image node"); continue;
        }
        const auto& node = *nodes.value(image.containerId);
        if (image.resourceId != node.image.resourceId) fail(node.id, "wrong image resource");
        if (!positive(image.rect) || !positive(image.sourceRect)) fail(node.id, "invalid image rectangle");
        if (!close(image.rect.width(), node.image.displaySize.width()) || !close(image.rect.height(), node.image.displaySize.height())) fail(node.id, "image display size differs");
        if (boxes.contains(node.id) && !rectClose(image.rect, boxes.value(node.id)->contentRect)) fail(node.id, "image rect does not match its content box");
        includeBounds(image.rect);
    }
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
        if (it.value()->type == ContainerType::Image && !images.contains(it.key())) fail(it.key(), "missing image drawing record");

    QHash<QString, std::vector<const TextFragment*>> fragments;
    for (const auto& f : result.textFragments) {
        includeBounds(f.rect);
        if (!nodes.contains(f.containerId) || nodes.value(f.containerId)->type != ContainerType::Text) {
            fail(f.containerId, "text fragment does not belong to a text node"); continue;
        }
        fragments[f.containerId].push_back(&f);
        const auto& node = *nodes.value(f.containerId);
        if (!positive(f.rect)) fail(node.id, "invalid text region");
        const auto r = f.sourceRange;
        if (r.start < 0 || r.end < r.start || r.end > node.text.text.size()) { fail(node.id, "invalid source range"); continue; }
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, node.text.text);
        boundary.setPosition(r.start);
        if (!boundary.isAtBoundary()) fail(node.id, "fragment starts inside a grapheme");
        boundary.setPosition(r.end);
        if (!boundary.isAtBoundary()) fail(node.id, "fragment ends inside a grapheme");
        if (boxes.contains(node.id) && !boxes.value(node.id)->contentRect.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(f.rect)) fail(node.id, "text is outside its content box");
        for (const auto& child : node.children)
            if (boxes.contains(child.id) && overlap(f.rect, boxes.value(child.id)->outerRect)) fail(node.id, "text overlaps a child obstacle");
    }
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        const auto& node = *it.value();
        if (node.type != ContainerType::Text) continue;
        auto list = fragments.value(node.id);
        std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return a->sourceRange.start < b->sourceRange.start; });
        size_t index = 0;
        for (const auto& paragraph : node.text.paragraphs) {
            int next = paragraph.range.start;
            int count = 0;
            while (index < list.size() && list[index]->sourceRange.start <= paragraph.range.end) {
                const auto range = list[index++]->sourceRange;
                if (range.start != next || range.end > paragraph.range.end ||
                    (range.start == range.end && paragraph.range.start != paragraph.range.end))
                    fail(node.id, "paragraph has missing, overlapping or invalid text ranges");
                next = range.end;
                ++count;
            }
            if (next != paragraph.range.end || count == 0 || (paragraph.range.start == paragraph.range.end && count != 1)) fail(node.id, "paragraph coverage is incomplete");
        }
        if (index != list.size()) fail(node.id, "fragment includes a paragraph separator or unexpected source offset");
        for (size_t a = 0; a < list.size(); ++a)
            for (size_t b = a + 1; b < list.size(); ++b)
                if (overlap(list[a]->rect, list[b]->rect)) fail(node.id, "text regions overlap");
    }
    if (!rectClose(result.layoutBounds, hasBounds ? bounds : QRectF())) fail("layoutBounds", "does not cover the complete union of output regions");
    return errors;
}

} // namespace editor::testing
