#include "EditorInput.h"

#include "CanvasWidget.h"
#include "EditorSession.h"
#include "InteractionController.h"
#include "Selection.h"
#include "TextBoundary.h"

#include <QAction>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace editor::app {

EditorInput::EditorInput(CanvasWidget& canvas, EditorSession& session)
    : QWidget(&canvas), canvas_(canvas), session_(session)
{
    setObjectName(QStringLiteral("editorInput"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setMouseTracking(true);

    auto* layout = new QVBoxLayout(&canvas);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(this);
    canvas.setFocusProxy(this);
}

QRectF EditorInput::toWidget(const QRectF& paperRect) const
{
    if (!paperRect.isValid())
        return {};
    return {canvas_.paperToWidget(paperRect.topLeft()), paperRect.size() * canvas_.zoom()};
}

QFont EditorInput::preeditFont() const
{
    const auto style = session_.currentStyle();
    QFont font(style.fontFamily);
    font.setPixelSize(qMax(1, qRound(style.fontSizePt * data::DocumentUnitsPerInch / 72)));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    return font;
}

QRectF EditorInput::caretOnPaper() const
{
    const auto& state = session_.state();
    auto rect = state.caret.rect;
    if (state.preedit.rect.isValid() && !state.preedit.text.isEmpty()) {
        rect = state.preedit.rect;
        const QFontMetricsF metrics(preeditFont());
        rect.translate(metrics.horizontalAdvance(state.preedit.text.left(state.preedit.cursorInPreedit)), 0);
        rect.setHeight(qMax(rect.height(), metrics.height()));
    }
    return rect;
}

QRectF EditorInput::cursorRectangle() const
{
    return toWidget(caretOnPaper());
}

void EditorInput::refreshInputMethod()
{
    if (hasFocus())
        QGuiApplication::inputMethod()->update(Qt::ImQueryInput | Qt::ImAnchorRectangle);
}

void EditorInput::paintEvent(QPaintEvent*)
{
    const auto& state = session_.state();
    QPainter painter(this);
    painter.translate(canvas_.paperToWidget(QPointF{}));
    painter.scale(canvas_.zoom(), canvas_.zoom());
    painter.setRenderHint(QPainter::TextAntialiasing);

    if (state.selection) {
        for (const auto& rect : state.selection->rects)
            painter.fillRect(rect, QColor(47, 128, 237, 64));
    }
    if (!hasFocus())
        return;

    if (!state.preedit.text.isEmpty() && state.preedit.rect.isValid()) {
        const auto font = preeditFont();
        const QFontMetricsF metrics(font);
        const auto top = state.preedit.rect.topLeft();
        const QRectF box(top, QSizeF(metrics.horizontalAdvance(state.preedit.text),
                                    qMax(state.preedit.rect.height(), metrics.height())));
        painter.fillRect(box, QColor(255, 249, 224));
        painter.setFont(font);
        painter.setPen(session_.currentStyle().color);
        const qreal baseline = box.center().y() + (metrics.ascent() - metrics.descent()) / 2;
        painter.drawText(QPointF(box.left(), baseline), state.preedit.text);
        painter.drawLine(box.bottomLeft(), box.bottomRight());
    }

    const auto caret = caretOnPaper();
    if (caret.isValid()) {
        painter.setPen(QPen(QColor(26, 26, 26), 1 / canvas_.zoom()));
        painter.drawLine(caret.topLeft(), caret.bottomLeft());
    }
}

void EditorInput::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    if (!session_.state().preedit.text.isEmpty())
        QGuiApplication::inputMethod()->commit();

    if (!session_.beginSelection(canvas_.widgetToPaper(event->position()),
                                 event->modifiers().testFlag(Qt::ShiftModifier))) {
        event->ignore();
        return;
    }
    event->accept();
}

void EditorInput::mouseMoveEvent(QMouseEvent* event)
{
    const auto paper = canvas_.widgetToPaper(event->position());
    if (!event->buttons().testFlag(Qt::LeftButton) || !session_.dragSelection(paper)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // 普通移动交给父画布上报。拖选消费事件，由这里上报坐标。
    emit canvas_.pointerMoved(paper);
    event->accept();
}

void EditorInput::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && session_.endSelection()) {
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void EditorInput::keyPressEvent(QKeyEvent* event)
{
    // 如果按键直接送到画布，仍调用菜单使用的 QAction。
    const QKeySequence shortcut(event->keyCombination());
    for (auto* action : actions()) {
        if (action->shortcuts().contains(shortcut)) {
            action->trigger();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && !session_.state().preedit.text.isEmpty())
        QGuiApplication::inputMethod()->reset();
    session_.key({event->key(), event->modifiers(), event->text()}, {});
    event->accept();
}

void EditorInput::inputMethodEvent(QInputMethodEvent* event)
{
    interaction::ImeInput input;
    input.preedit = event->preeditString();
    input.commit = event->commitString();
    input.replacementStart = event->replacementStart();
    input.replacementLength = event->replacementLength();
    for (const auto& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            input.preeditCursor = attribute.start;
            break;
        }
    }
    session_.ime(input);
    event->accept();
}

QVariant EditorInput::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const auto& state = session_.state();
    const auto& caret = state.caret;
    const auto* text = interaction::findTextContent(session_.snapshot(), caret.containerId);
    const int anchor = state.selectionAnchor.value_or(caret.offset);
    switch (query) {
    case Qt::ImEnabled:
        return text != nullptr;
    case Qt::ImCursorRectangle:
        return cursorRectangle();
    case Qt::ImAnchorRectangle:
        return toWidget(interaction::caretRect(session_.layout(), caret.containerId, anchor));
    case Qt::ImSurroundingText:
        return text ? text->text : QString{};
    case Qt::ImCursorPosition:
    case Qt::ImAbsolutePosition:
        return caret.offset;
    case Qt::ImAnchorPosition:
        return anchor;
    case Qt::ImCurrentSelection:
        return interaction::selectedText(state, session_.snapshot());
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void EditorInput::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    refreshInputMethod();
    update();
}

void EditorInput::focusOutEvent(QFocusEvent* event)
{
    session_.endSelection();
    if (!session_.state().preedit.text.isEmpty()) {
        interaction::EditAction cancel;
        cancel.kind = interaction::EditActionKind::CancelPreedit;
        session_.action(cancel);
    }
    QWidget::focusOutEvent(event);
    update();
}

} // namespace editor::app
