#include "TestFixtures.h"
#include "FixtureSupport.h"
#include <QMap>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>

using namespace editor::data;

namespace editor::testing {
namespace {

struct Comparison {
    QStringList errors; // 带字段路径的错误列表，便于组员定位差异。
    qreal tolerance;    // 坐标比较容差，字符下标和 ID 不使用容差。
    void fail(const QString& path, const QString& reason) {
        if (errors.size() < 80) errors << path + ": " + reason;
    }
    template<class A, class B> void equal(const QString& path, const A& expected, const B& actual) {
        if (expected != actual) fail(path, QString("expected %1, got %2").arg(expected).arg(actual));
    }
    void number(const QString& path, qreal expected, qreal actual) {
        if (!std::isfinite(actual) || std::abs(expected - actual) > tolerance)
            fail(path, QString("expected %1, got %2").arg(expected).arg(actual));
    }
    void rect(const QString& path, const QRectF& a, const QRectF& b) {
        number(path + ".x", a.x(), b.x()); number(path + ".y", a.y(), b.y());
        number(path + ".width", a.width(), b.width()); number(path + ".height", a.height(), b.height());
    }
    template<class T, class Key> QMap<QString, const T*> index(const std::vector<T>& values, const QString& path, Key key) {
        QMap<QString, const T*> result;
        for (const auto& value : values) {
            const auto id = key(value);
            if (result.contains(id)) fail(path + "/" + id, "duplicate record");
            result.insert(id, &value);
        }
        return result;
    }
    template<class T, class Key, class Check> void records(const std::vector<T>& expected, const std::vector<T>& actual,
                                                          const QString& path, Key key, Check check) {
        const auto a = index(expected, path + "/expected", key);
        const auto b = index(actual, path + "/actual", key);
        equal(path + ".count", expected.size(), actual.size());
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!b.contains(it.key())) fail(path + "/" + it.key(), "missing record");
            else check(path + "/" + it.key(), *it.value(), *b.value(it.key()));
        }
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key())) fail(path + "/" + it.key(), "unexpected record");
    }
};

// 从实际 Qt 布局提取某个字符的最终格式，允许实现方采用不同的格式区间拆法。
QTextCharFormat characterFormat(const QTextLayout& layout, int position)
{
    QTextCharFormat format;
    for (const auto& range : layout.formats())
        if (position >= range.start && position < range.start + range.length) format.merge(range.format);
    return format;
}

TextStyle sourceStyle(const ContainerNode& node, int position)
{
    auto style = node.text.defaultStyle;
    for (const auto& p : node.text.paragraphs)
        if (position >= p.range.start && position < p.range.end) { style = detail::mergeStyle(style, p.defaultStyle); break; }
    for (const auto& span : node.text.formatSpans)
        if (position >= span.range.start && position < span.range.end) { style = detail::mergeStyle(style, span.style); break; }
    return style;
}

const QTextLayout* validateLine(Comparison& c, const DocumentSnapshot& source, const LayoutResult& result,
                               const TextFragment& fragment, const QString& path)
{
    if (fragment.layoutIndex < 0 || fragment.layoutIndex >= int(result.textLayouts.size()) || !result.textLayouts[fragment.layoutIndex]) {
        c.fail(path + ".layoutIndex", "invalid index or null layout"); return nullptr;
    }
    const auto& layout = *result.textLayouts[fragment.layoutIndex];
    if (fragment.lineIndex < 0 || fragment.lineIndex >= layout.lineCount()) {
        c.fail(path + ".lineIndex", "invalid line index"); return nullptr;
    }
    const auto& node = detail::findNode(source.root, fragment.containerId);
    const auto range = fragment.sourceRange;
    if (range.start < 0 || range.end < range.start || range.end > node.text.text.size()) {
        c.fail(path + ".sourceRange", "outside source text"); return nullptr;
    }
    const auto line = layout.lineAt(fragment.lineIndex);
    const int local = line.textStart();
    const int count = range.end - range.start;
    c.equal(path + ".line.length", count, line.textLength());
    c.equal(path + ".line.text", node.text.text.mid(range.start, count), layout.text().mid(local, line.textLength()));
    c.number(path + ".line.x", fragment.rect.x(), line.x());
    c.number(path + ".line.y", fragment.rect.y(), line.y());
    c.number(path + ".line.width", fragment.rect.width(), line.width());
    // Qt 对空字符串的 isValidCursorPosition(0) 返回 false；空段落仍有一个插入位置。
    if (count > 0 && (!layout.isValidCursorPosition(local) || !layout.isValidCursorPosition(local + line.textLength())))
        c.fail(path, "line splits a cursor boundary");
    for (int offset = 0; offset < count && local + offset < layout.text().size(); ++offset) {
        const auto expected = sourceStyle(node, range.start + offset);
        const auto format = characterFormat(layout, local + offset);
        const auto font = format.font().resolve(layout.font());
        const auto prefix = path + ".style[" + QString::number(range.start + offset) + "]";
        c.equal(prefix + ".family", expected.fontFamily, font.family());
        const qreal sizePt = font.pixelSize() > 0 ? font.pixelSize() * 72.0 / DocumentUnitsPerInch : font.pointSizeF();
        if (std::abs(expected.fontSizePt - sizePt) > 0.01) c.fail(prefix + ".fontSizePt", "font size differs");
        c.equal(prefix + ".bold", expected.bold, font.bold());
        c.equal(prefix + ".italic", expected.italic, font.italic());
        c.equal(prefix + ".underline", expected.underline, font.underline());
        const QColor color = format.foreground().style() == Qt::NoBrush ? QColor(Qt::black) : format.foreground().color();
        c.equal(prefix + ".color", expected.color.name(QColor::HexArgb), color.name(QColor::HexArgb));
    }
    return &layout;
}

} // namespace

QStringList compareLayout(const DocumentSnapshot& source, const LayoutResult& expected,
                          const LayoutResult& actual, qreal tolerance)
{
    Comparison c{{}, tolerance};
    if (!std::isfinite(tolerance) || tolerance < 0) { c.fail("tolerance", "must be finite and nonnegative"); return c.errors; }
    c.errors = validateLayoutStructure(source, actual, tolerance);
    c.equal("documentRevision", source.revision, actual.documentRevision);
    c.rect("pageRect", expected.pageRect, actual.pageRect);
    c.rect("contentRect", expected.contentRect, actual.contentRect);
    c.rect("layoutBounds", expected.layoutBounds, actual.layoutBounds);
    c.records(expected.containers, actual.containers, "containers", [](const auto& value) { return value.containerId; },
        [&](const auto& path, const auto& a, const auto& b) {
            c.equal(path + ".parentId", a.parentId, b.parentId);
            c.rect(path + ".rect", a.rect, b.rect);
            c.rect(path + ".contentRect", a.contentRect, b.contentRect);
            c.rect(path + ".outerRect", a.outerRect, b.outerRect);
        });
    c.records(expected.images, actual.images, "images", [](const auto& value) { return value.containerId; },
        [&](const auto& path, const auto& a, const auto& b) {
            c.equal(path + ".resourceId", a.resourceId, b.resourceId);
            c.rect(path + ".sourceRect", a.sourceRect, b.sourceRect);
            c.rect(path + ".rect", a.rect, b.rect);
        });
    c.records(expected.textFragments, actual.textFragments, "text", [](const auto& f) {
            return f.containerId + ":" + QString::number(f.sourceRange.start);
        }, [&](const auto& path, const auto& a, const auto& b) {
            c.equal(path + ".sourceRange.end", a.sourceRange.end, b.sourceRange.end);
            c.rect(path + ".rect", a.rect, b.rect);
            const auto* al = validateLine(c, source, expected, a, path + "/expected");
            const auto* bl = validateLine(c, source, actual, b, path + "/actual");
            if (!al || !bl) return;
            const auto left = al->lineAt(a.lineIndex);
            const auto right = bl->lineAt(b.lineIndex);
            const auto& text = detail::findNode(source.root, a.containerId).text.text;
            QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
            // 比较所有合法字符边界的横坐标，检查右对齐、混合格式及光标定位依据。
            for (int i = 0; i <= a.sourceRange.end - a.sourceRange.start; ++i) {
                boundaries.setPosition(a.sourceRange.start + i);
                if (boundaries.isAtBoundary() && i <= right.textLength())
                    c.number(path + ".cursorX[" + QString::number(i) + "]",
                             left.cursorToX(left.textStart() + i), right.cursorToX(right.textStart() + i));
            }
        });
    return c.errors;
}

} // namespace editor::testing
