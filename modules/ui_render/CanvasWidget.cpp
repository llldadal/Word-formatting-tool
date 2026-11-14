#include "CanvasWidget.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QTextLine>
#include <cmath>

using namespace editor::data;

CanvasWidget::CanvasWidget(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);

    rebuildGeometry();

    m_blinkTimer.setInterval(530);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_caretVisible = !m_caretVisible;
        update();
    });
    m_blinkTimer.start();
}

void CanvasWidget::setDocumentData(const DocumentSnapshot& snapshot, const LayoutResult& result)
{
    m_snapshot = snapshot;
    m_result = result;
    rebuildGeometry();
    update();
}

void CanvasWidget::setInteraction(editor::interaction::EditorInteraction* interaction)
{
    m_interaction = interaction;
    // updateBlinkTimer(); // 暂时注释，等真实交互模块接入后再打开
    update();
}

void CanvasWidget::setZoom(qreal zoom)
{
    zoom = qBound(0.25, zoom, 4.0);
    if (qFuzzyCompare(zoom, m_zoom)) {
        return;
    }
    m_zoom = zoom;
    rebuildGeometry();
    update();
}

void CanvasWidget::setShowLayoutBoxes(bool show)
{
    m_showBoxes = show;
    update();
}

QPointF CanvasWidget::paperToWidget(const QPointF& paperPoint) const
{
    return m_origin + paperPoint * m_zoom;
}

QPointF CanvasWidget::widgetToPaper(const QPointF& widgetPoint) const
{
    return (widgetPoint - m_origin) / m_zoom;
}

QRectF CanvasWidget::caretRectInWidget() const
{
    // 暂时返回空，等真实交互模块接入后，换成 m_interaction->caretRectangle()
    return QRectF();
}

QRectF CanvasWidget::contentAreaInPaper() const
{
    const QRectF page = m_result.pageRect.isValid()
        ? m_result.pageRect
        : QRectF(0, 0, kFallbackPaperWidth, kFallbackPaperHeight);

    QRectF area = m_result.pageRect;
    if (m_result.layoutBounds.isValid()) {
        area = area.united(m_result.layoutBounds);
    }
    if (!area.isValid()) {
        area = page;
    }

    const qreal sanity = 4.0 * qMax(page.width(), page.height());
    if (qAbs(area.left()) > sanity || qAbs(area.top()) > sanity
        || area.right() > 3.0 * sanity || area.bottom() > 3.0 * sanity) {
        area = page;
    }
    return area;
}

qreal CanvasWidget::zoomToFitWidth(qreal viewportWidthInWidget) const
{
    const qreal available = viewportWidthInWidget - 2 * kCanvasMargin;
    const qreal areaWidth = contentAreaInPaper().width();
    if (available <= 0.0 || areaWidth <= 0.0) {
        return m_zoom;
    }
    return available / areaWidth;
}

void CanvasWidget::rebuildGeometry()
{
    const QRectF area = contentAreaInPaper();

    m_origin = QPointF(-area.left() * m_zoom + kCanvasMargin,
                       -area.top() * m_zoom + kCanvasMargin);

    const int contentWidth = qMax(static_cast<int>(
        qMin<qreal>(std::ceil(area.width() * m_zoom), kMaxCanvasExtent)), 120);
    const int contentHeight = qMax(static_cast<int>(
        qMin<qreal>(std::ceil(area.height() * m_zoom), kMaxCanvasExtent)), 120);
    setFixedSize(contentWidth + 2 * kCanvasMargin, contentHeight + 2 * kCanvasMargin);
}

void CanvasWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    painter.fillRect(rect(), QColor(0xd9, 0xdc, 0xe0));

    const QRectF page = m_result.pageRect;
    const QRectF pageWidget(paperToWidget(page.topLeft()),
                            QSizeF(page.width() * m_zoom, page.height() * m_zoom));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 38));
    painter.drawRoundedRect(pageWidget.translated(4, 5), 2, 2);
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(pageWidget, 2, 2);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0xbf, 0xc5, 0xcb), 1));
    painter.drawRoundedRect(pageWidget, 2, 2);

    painter.save();
    painter.translate(m_origin);
    painter.scale(m_zoom, m_zoom);
    paintContent(painter);
    paintSelectionAndCaret(painter);
    if (m_showBoxes) {
        paintLayoutBoxes(painter);
    }
    paintOverflowMark(painter);
    painter.restore();
}

void CanvasWidget::paintContent(QPainter& painter) const
{
    Q_ASSERT(m_result.documentRevision == m_snapshot.revision);
    if (m_result.documentRevision != m_snapshot.revision) {
        return;
    }

    const QRectF visibleInPaper = QRectF(widgetToPaper(QPointF(0, 0)),
                                         widgetToPaper(QPointF(width(), height()))).normalized();

    painter.setPen(QColor(0x1a, 0x1a, 0x1a));
    for (const TextFragment& fragment : m_result.textFragments) {
        if (fragment.layoutIndex < 0
            || static_cast<size_t>(fragment.layoutIndex) >= m_result.textLayouts.size()) {
            continue;
        }
        const QTextLayout* layout = m_result.textLayouts[static_cast<size_t>(fragment.layoutIndex)].get();
        if (!layout) {
            continue;
        }
        if (fragment.lineIndex < 0 || fragment.lineIndex >= layout->lineCount()) {
            continue;
        }
        const QTextLine line = layout->lineAt(fragment.lineIndex);
        if (!line.isValid()) {
            continue;
        }
        if (!fragment.rect.isValid() || !fragment.rect.intersects(visibleInPaper)) {
            continue;
        }

        painter.save();
        painter.setClipRect(fragment.rect);
        line.draw(&painter, fragment.rect.topLeft() - layout->position() - line.position());
        painter.restore();
    }

    for (const ImageLayout& imageLayout : m_result.images) {
        if (!imageLayout.rect.isValid() || !imageLayout.rect.intersects(visibleInPaper)) {
            continue;
        }
        const auto resource = m_snapshot.resources.constFind(imageLayout.resourceId);
        if (resource == m_snapshot.resources.constEnd() || resource->isNull()) {
            continue;
        }
        const QRectF source = imageLayout.sourceRect.intersected(QRectF(0.0, 0.0, resource->width(), resource->height()));
        if (source.isEmpty()) {
            continue;
        }
        painter.drawImage(imageLayout.rect, *resource, source);
    }
}

void CanvasWidget::paintSelectionAndCaret(QPainter& painter) const
{
    if (!m_interaction) {
        return;
    }

    // 光标、选区、输入法的绘制
    /*
    const QVector<QRectF> selection = m_interaction->selectionRectangles();
    if (!selection.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2f, 0x80, 0xed, 64));
        for (const QRectF& rect : selection) {
            painter.drawRect(rect);
        }
    }

    if (!m_interaction->hasFocus()) {
        return;
    }

    const QRectF caret = m_interaction->caretRectangle();
    if (!caret.isValid()) {
        return;
    }

    const QString preedit = m_interaction->preedit();
    const TextStyle style = m_interaction->currentStyle();
    QFont font(style.fontFamily);
    font.setPointSizeF(style.fontSizePt);
    font.setBold(style.bold);
    font.setItalic(style.italic);

    if (preedit.isEmpty()) {
        if (!m_caretVisible) {
            return;
        }
        painter.setPen(QPen(QColor(0x1a, 0x1a, 0x1a), qMax(1.0 / m_zoom, 0.8)));
        painter.drawLine(QPointF(caret.left(), caret.top()),
                         QPointF(caret.left(), caret.bottom()));
        return;
    }

    // 输入法预编辑内容
    const QFontMetricsF metrics(font);
    const qreal baseline = caret.top() + (caret.height() - metrics.height()) / 2.0 + metrics.ascent();
    painter.setFont(font);
    painter.setPen(QColor(0x1a, 0x1a, 0x1a));
    painter.drawText(QPointF(caret.left(), baseline), preedit);
    painter.drawLine(QPointF(caret.left(), baseline + 2.0),
                     QPointF(caret.left() + metrics.horizontalAdvance(preedit), baseline + 2.0));

    const qreal caretX = caret.left()
        + metrics.horizontalAdvance(preedit.left(m_interaction->preeditCaret()));
    painter.setPen(QPen(QColor(0x1a, 0x1a, 0x1a), qMax(1.0 / m_zoom, 0.8)));
    painter.drawLine(QPointF(caretX, caret.top()), QPointF(caretX, caret.top() + caret.height()));
    */
    if (!hasFocus()) {
        return;
    }
    painter.setPen(QPen(QColor(0x1a, 0x1a, 0x1a), 1.0));
    painter.drawLine(QPointF(100, 100), QPointF(100, 120));
    Q_UNUSED(painter);
}

void CanvasWidget::paintLayoutBoxes(QPainter& painter) const
{
    painter.save();
    painter.setBrush(Qt::NoBrush);

    painter.setPen(QPen(QColor(0x2f, 0x80, 0xed, 190), qMax(0.9 / m_zoom, 0.5), Qt::DashLine));
    for (const ContainerLayout& layout : m_result.containers) {
        painter.drawRect(layout.rect);
    }

    painter.setPen(QPen(QColor(0xf2, 0x99, 0x4a, 200), qMax(0.8 / m_zoom, 0.4), Qt::DotLine));
    for (const TextFragment& fragment : m_result.textFragments) {
        painter.drawRect(fragment.rect);
    }

    painter.setPen(QPen(QColor(0x27, 0xae, 0x60, 200), qMax(0.9 / m_zoom, 0.5), Qt::DashLine));
    for (const ImageLayout& imageLayout : m_result.images) {
        painter.drawRect(imageLayout.rect);
    }

    painter.setPen(QPen(QColor(0x9b, 0x51, 0xe0, 220), qMax(1.0 / m_zoom, 0.6), Qt::DashDotLine));
    painter.drawRect(m_result.layoutBounds);

    painter.restore();
}

void CanvasWidget::paintOverflowMark(QPainter& painter) const
{
    if (!m_result.layoutBounds.isValid() || !m_result.pageRect.isValid()) {
        return;
    }

    constexpr qreal kEpsilon = 0.5;
    const QRectF& page = m_result.pageRect;
    const QRectF& bounds = m_result.layoutBounds;

    const bool overflows = bounds.bottom() > page.bottom() + kEpsilon
        || bounds.right() > page.right() + kEpsilon
        || bounds.left() < page.left() - kEpsilon
        || bounds.top() < page.top() - kEpsilon;
    if (!overflows) {
        return;
    }

    painter.save();
    painter.setPen(QPen(QColor(0xe0, 0x4b, 0x3d, 220), qMax(1.2 / m_zoom, 0.7), Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(page);
    painter.restore();
}

void CanvasWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (m_interaction && event->button() == Qt::LeftButton) {
        // m_interaction->mousePress(widgetToPaper(event->position()), event->modifiers());
        update();
    }
    QWidget::mousePressEvent(event);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF paperPoint = widgetToPaper(event->position());
    emit pointerMoved(paperPoint);

    if (m_interaction && (event->buttons() & Qt::LeftButton)) {
        // m_interaction->mouseMove(paperPoint);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_interaction) {
        // m_interaction->mouseRelease();
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void CanvasWidget::keyPressEvent(QKeyEvent* event)
{
    if(event->text().length()>0){
        qDebug()<<"按下键盘"<<event->text();
    }

    QWidget::keyPressEvent(event);
}

void CanvasWidget::focusInEvent(QFocusEvent* event)
{
    if (m_interaction) {
        // m_interaction->setFocus(true);
    }
    m_caretVisible = true;
    // updateBlinkTimer();
    update();
    QWidget::focusInEvent(event);
}

void CanvasWidget::focusOutEvent(QFocusEvent* event)
{
    if (m_interaction) {
        // m_interaction->setFocus(false);
        // m_interaction->clearPreedit();
    }
    m_blinkTimer.stop();
    update();
    QWidget::focusOutEvent(event);
}

void CanvasWidget::inputMethodEvent(QInputMethodEvent* event)
{
    if (!m_interaction) {
        QWidget::inputMethodEvent(event);
        return;
    }
    // 暂时交给父类，等接入后取消注释
    QWidget::inputMethodEvent(event);
}

QVariant CanvasWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    // 暂时返回父类默认值
    return QWidget::inputMethodQuery(query);
}

void CanvasWidget::updateBlinkTimer()
{
    if (m_interaction /* && m_interaction->hasFocus() */) {
        m_caretVisible = true;
        m_blinkTimer.start();
    } else {
        m_blinkTimer.stop();
    }
}