#include "DocumentCanvas.h"

#include "DocxExporter.h"
#include "FlowGeometry.h"
#include "HitTest.h"
#include "LocalDocumentExecutor.h"
#include "Selection.h"
#include "TestFixtures.h"

#include <QBrush>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QStringList>
#include <QtMath>

#include <algorithm>

namespace data = editor::data;
namespace ei = editor::interaction;

namespace {

const data::ContainerNode* findTextNode(const data::ContainerNode& node,
                                        const data::ContainerId& id)
{
    if (node.type == data::ContainerType::Text && node.id == id)
        return &node;
    for (const auto& child : node.children) {
        if (const data::ContainerNode* found = findTextNode(child, id))
            return found;
    }
    return nullptr;
}

data::ContainerId firstTextId(const data::ContainerNode& node)
{
    if (node.type == data::ContainerType::Text)
        return node.id;
    for (const auto& child : node.children) {
        const data::ContainerId id = firstTextId(child);
        if (!id.isEmpty())
            return id;
    }
    return {};
}

QTextCharFormat formatFrom(const data::TextStyleOverride& style)
{
    QTextCharFormat format;
    if (style.fontFamily)
        format.setFontFamilies(QStringList{ *style.fontFamily });
    if (style.fontSizePt)
        format.setFontPointSize(*style.fontSizePt);
    if (style.color)
        format.setForeground(QBrush(*style.color));
    if (style.bold)
        format.setFontWeight(*style.bold ? QFont::Bold : QFont::Normal);
    if (style.italic)
        format.setFontItalic(*style.italic);
    if (style.underline)
        format.setFontUnderline(*style.underline);
    return format;
}

} // 匿名命名空间结束

namespace editor::demo {

DocumentCanvas::DocumentCanvas(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    setAutoFillBackground(false);
}

QSize DocumentCanvas::paperSize() const
{
    const QRectF bounds = layout_.layoutBounds.united(layout_.pageRect);
    return QSize(qCeil(qMax<qreal>(bounds.width(), 1.0)) + 24,
                 qCeil(qMax<qreal>(bounds.height(), 1.0)) + 24);
}

void DocumentCanvas::loadCase(const QString& caseId)
{
    QString chosen = caseId;
    data::DocumentSnapshot doc;
    try {
        doc = editor::testing::makeLayoutCase(chosen).document;
    } catch (...) {
        chosen = QStringLiteral("01");
        doc = editor::testing::makeLayoutCase(chosen).document;
    }

    caseId_ = chosen;
    doc_ = doc;
    layout_ = editor::slice::projectLayout(doc_);
    controller_.reset(doc_, layout_, firstTextId(doc_.root), 0);
    appliedRequests_ = 0;
    lastError_.clear();
    dragging_ = false;
    clipboardText_.clear();

    setFixedSize(paperSize());
    updateGeometry();
    update();
    publishStatus(QStringLiteral("样例已载入"));
}

void DocumentCanvas::reload()
{
    loadCase(caseId_);
}

void DocumentCanvas::publishStatus(const QString& message)
{
    const ei::InteractionState& st = controller_.state();
    const QString text = caseId_
        + QStringLiteral("  |  caret ") + st.caret.containerId
        + QStringLiteral(":") + QString::number(st.caret.offset)
        + QStringLiteral("  |  rev ") + QString::number(doc_.revision)
        + QStringLiteral("  |  requests ") + QString::number(appliedRequests_)
        + QStringLiteral("  |  ") + message;
    if (st.selection) {
        const QString selection = QStringLiteral("  |  sel [")
            + QString::number(st.selection->range.start)
            + QStringLiteral(", ")
            + QString::number(st.selection->range.end)
            + QStringLiteral(") in ") + st.selection->containerId;
        QString withSelection = text + selection;
        if (onStatus)
            onStatus(withSelection);
        return;
    }
    if (onStatus)
        onStatus(text);
}

bool DocumentCanvas::commit(const ei::InteractionOutput& output)
{
    if (output.clipboardText) {
        clipboardText_ = *output.clipboardText;
        if (QClipboard* clipboard = QGuiApplication::clipboard())
            clipboard->setText(clipboardText_);
    }

    if (!output.requests.empty()) {
        const editor::slice::ApplyResult applied =
            editor::slice::applyRequests(doc_, output.requests);
        if (!applied.ok) {
            lastError_ = applied.error;
            publishStatus(QStringLiteral("错误：") + applied.error);
            return false;
        }
        doc_ = applied.document;
        layout_ = editor::slice::projectLayout(doc_);
        appliedRequests_ += static_cast<int>(output.requests.size());
        setFixedSize(paperSize());
        updateGeometry();
    }

    // 无论有没有改动，都用当前版面重算光标 / 选区 / 预编辑矩形。
    controller_.refresh(layout_);
    update();
    publishStatus(output.requests.empty()
                      ? QStringLiteral("状态更新")
                      : QStringLiteral("编辑已应用"));
    return true;
}

bool DocumentCanvas::exportToFile(const QString& path, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const editor::io::DocxExportResult result =
        editor::io::exportDocx(doc_, layout_, path);
    if (!result.ok()) {
        lastError_ = result.errorMessage;
        if (error)
            *error = result.errorMessage;
        publishStatus(QStringLiteral("导出失败：") + result.errorMessage);
        return false;
    }
    if (error)
        error->clear();
    publishStatus(QStringLiteral("已导出 ") + path);
    return true;
}

void DocumentCanvas::pasteText(const QString& text)
{
    commit(controller_.paste(text, doc_, layout_));
}

void DocumentCanvas::setCharStyle(const data::TextStyleOverride& style)
{
    commit(controller_.charFormat(doc_, layout_, style));
}

void DocumentCanvas::setHeading(int level)
{
    if (level < 0) {
        commit(controller_.paragraphFormat(doc_, layout_, std::nullopt,
                                           data::ParagraphRole::Title,
                                           std::nullopt));
    } else if (level == 0) {
        commit(controller_.paragraphFormat(doc_, layout_, std::nullopt,
                                           data::ParagraphRole::Body,
                                           std::nullopt));
    } else {
        commit(controller_.paragraphFormat(doc_, layout_, std::nullopt,
                                           data::ParagraphRole::Heading,
                                           qBound(1, level, 9)));
    }
}

void DocumentCanvas::copySelection()
{
    commit(controller_.copy(doc_, layout_));
}

void DocumentCanvas::cutSelection()
{
    commit(controller_.cut(doc_, layout_));
}

void DocumentCanvas::selectAll()
{
    commit(controller_.selectAll(doc_, layout_));
}

void DocumentCanvas::setPreedit(const QString& text)
{
    ei::ImeInput ime;
    ime.preedit = text;
    ime.preeditCursor = text.size();
    commit(controller_.ime(ime, doc_, layout_));
}

void DocumentCanvas::commitIme(const QString& text)
{
    ei::ImeInput ime;
    ime.commit = text;
    commit(controller_.ime(ime, doc_, layout_));
}

void DocumentCanvas::drawImages(QPainter* painter) const
{
    for (const auto& image : layout_.images) {
        const auto it = doc_.resources.constFind(image.resourceId);
        if (it == doc_.resources.constEnd() || it->isNull())
            continue;
        QRectF source = image.sourceRect;
        if (source.isEmpty())
            source = QRectF(0, 0, it->width(), it->height());
        painter->drawImage(image.rect, *it, source);
        painter->setPen(QPen(QColor(0xB0, 0xB0, 0xB0), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(image.rect);
    }
}

void DocumentCanvas::drawText(QPainter* painter) const
{
    // QTextLayout::draw 直接使用 QPainter 当前的 pen。容器框会把 pen 留成
    // 浅色点线，选区会把它留成 NoPen；画字之前必须显式恢复成实心深色，
    // 否则正文变成淡蓝色，拖选时整页文字直接消失。
    painter->setPen(QPen(QColor(0x14, 0x14, 0x14)));
    painter->setBrush(Qt::NoBrush);

    // 每个 QTextLayout 属于哪一个文本容器，由片段的 layoutIndex 反查。
    QHash<int, data::ContainerId> owner;
    for (const auto& fragment : layout_.textFragments) {
        if (fragment.layoutIndex >= 0)
            owner.insert(fragment.layoutIndex, fragment.containerId);
    }

    for (int i = 0; i < static_cast<int>(layout_.textLayouts.size()); ++i) {
        const std::shared_ptr<const QTextLayout>& textLayout =
            layout_.textLayouts[static_cast<size_t>(i)];
        if (!textLayout)
            continue;

        QList<QTextLayout::FormatRange> overrides;
        const data::ContainerId id = owner.value(i);
        if (!id.isEmpty()) {
            const data::ContainerNode* node = findTextNode(doc_.root, id);
            if (node) {
                for (const auto& span : node->text.formatSpans) {
                    if (span.range.end <= span.range.start)
                        continue;
                    QTextLayout::FormatRange range;
                    range.start = span.range.start;
                    range.length = span.range.end - span.range.start;
                    range.format = formatFrom(span.style);
                    overrides.push_back(range);
                }
            }
        }
        // 行的 position 已经是纸张坐标，偏移量传 (0, 0)。
        textLayout->draw(painter, QPointF(0, 0), overrides);
    }
}

void DocumentCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0xE8, 0xEA, 0xED));

    const QRectF page = layout_.pageRect;
    painter.fillRect(page.adjusted(5, 5, 5, 5), QColor(0xC4, 0xC8, 0xCE));
    painter.fillRect(page, Qt::white);
    painter.setPen(QPen(QColor(0xD2, 0xD2, 0xD2), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page.adjusted(0, 0, -1, -1));
    painter.setPen(QPen(QColor(0xF0, 0xF0, 0xF0), 1, Qt::DashLine));
    painter.drawRect(layout_.contentRect);

    painter.setPen(QPen(QColor(0xE2, 0xE2, 0xE2), 1, Qt::DotLine));
    for (const auto& box : layout_.containers)
        painter.drawRect(box.rect);

    const ei::InteractionState& state = controller_.state();
    if (state.selection) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2E, 0x8A, 0xF5, 0x70));
        for (const auto& rect : state.selection->rects)
            painter.drawRect(rect);
    }

    drawImages(&painter);
    drawText(&painter);

    const QRectF caret = state.caret.rect;
    if (!caret.isEmpty()) {
        painter.setPen(QPen(QColor(0x18, 0x18, 0x18), 2));
        painter.drawLine(QPointF(caret.left(), caret.top()),
                         QPointF(caret.left(), caret.bottom()));
    }

    if (!state.preedit.text.isEmpty()) {
        QFont font = this->font();
        font.setPointSizeF(12.0);
        QFontMetricsF metrics(font);
        const QRectF anchor = state.preedit.rect.isEmpty()
                                 ? caret : state.preedit.rect;
        const qreal width = metrics.horizontalAdvance(state.preedit.text);
        const QRectF box(anchor.left(), anchor.top(),
                         width + 10.0, qMax<qreal>(16.0, anchor.height()));
        painter.save();
        painter.setPen(QPen(QColor(0xBF, 0xBF, 0xBF), 1));
        painter.setBrush(QColor(0xFF, 0xF6, 0xD8));
        painter.drawRect(box);
        painter.setPen(QPen(QColor(0x12, 0x12, 0x12)));
        painter.setFont(font);
        painter.drawText(box.adjusted(4, 0, -4, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, state.preedit.text);
        painter.setPen(QPen(QColor(0x00, 0x66, 0xCC), 1, Qt::DashLine));
        painter.drawLine(QPointF(box.left() + 3.0, box.bottom() - 1.0),
                         QPointF(box.right() - 3.0, box.bottom() - 1.0));
        painter.restore();
    }
}

void DocumentCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    const ei::Hit hit = ei::hitTest(layout_, event->position());
    if (!hit.valid()) {
        QWidget::mousePressEvent(event);
        return;
    }

    anchor_ = hit;
    dragging_ = true;
    ei::InteractionState state = controller_.state();
    state.caret.containerId = hit.containerId;
    state.caret.offset = hit.offset;
    state.caret.rect = ei::caretRect(layout_, hit.containerId, hit.offset);
    state.selection.reset();
    state.preedit = ei::PreeditState{};
    controller_.setState(state);
    controller_.refresh(layout_);
    update();
    publishStatus(QStringLiteral("鼠标定位"));
    event->accept();
}

void DocumentCanvas::mouseMoveEvent(QMouseEvent* event)
{
    const bool leftHeld = (event->buttons() & Qt::LeftButton) != 0;
    if (!dragging_) {
        // 真实窗口里按下事件有可能先被父级滚动区吃掉；只要左键仍按着，
        // 就用上一次有效的锚点把拖选续上，不让它无声失效。
        if (!leftHeld || !anchor_.valid()) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        dragging_ = true;
    }
    const ei::Hit focus = ei::clampFocus(layout_, anchor_, event->position());
    ei::InteractionState state = controller_.state();
    state.caret.containerId = anchor_.containerId;
    state.caret.offset = focus.offset;
    state.selection = ei::makeSelection(layout_, anchor_, focus);
    controller_.setState(state);
    controller_.refresh(layout_);
    update();
    publishStatus(QStringLiteral("拖选"));
    event->accept();
}

void DocumentCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    dragging_ = false;
    publishStatus(QStringLiteral("选区完成"));
    QWidget::mouseReleaseEvent(event);
}

void DocumentCanvas::keyPressEvent(QKeyEvent* event)
{
    ei::KeyInput key;
    key.key = event->key();
    key.mods = event->modifiers();
    key.text = event->text();

    QString clipboard = clipboardText_;
    if (QClipboard* systemClipboard = QGuiApplication::clipboard()) {
        const QString systemText = systemClipboard->text();
        if (!systemText.isEmpty())
            clipboard = systemText;
    }

    commit(controller_.key(key, doc_, layout_, clipboard));
    event->accept();
}

void DocumentCanvas::inputMethodEvent(QInputMethodEvent* event)
{
    ei::ImeInput ime;
    ime.preedit = event->preeditString();
    ime.commit = event->commitString();
    ime.replacementStart = event->replacementStart();
    ime.replacementLength = event->replacementLength();
    for (const auto& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            ime.preeditCursor = attribute.start;
            break;
        }
    }
    commit(controller_.ime(ime, doc_, layout_));
    event->accept();
}

QVariant DocumentCanvas::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    if (query == Qt::ImCursorRectangle)
        return QVariant(controller_.state().caret.rect.toRect());
    return QWidget::inputMethodQuery(query);
}

} // 命名空间 editor::demo 结束
