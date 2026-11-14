#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"

#include <QPointF>
#include <QTimer>
#include <QWidget>

namespace editor::interaction {
class EditorInteraction;
}

class CanvasWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);

    void setDocumentData(const editor::data::DocumentSnapshot& snapshot,
                         const editor::data::LayoutResult& result);

    void setInteraction(editor::interaction::EditorInteraction* interaction);

    void setZoom(qreal zoom);
    qreal zoom() const { return m_zoom; }

    void setShowLayoutBoxes(bool show);
    bool showLayoutBoxes() const { return m_showBoxes; }

    QPointF paperToWidget(const QPointF& paperPoint) const;
    QPointF widgetToPaper(const QPointF& widgetPoint) const;
    QRectF caretRectInWidget() const;

signals:
    void pointerMoved(const QPointF& paperPoint);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    void rebuildGeometry();
    void paintContent(QPainter& painter) const;
    void paintSelectionAndCaret(QPainter& painter) const;
    void paintLayoutBoxes(QPainter& painter) const;
    void paintOverflowMark(QPainter& painter) const;
    void updateBlinkTimer();

    editor::data::DocumentSnapshot m_snapshot;
    editor::data::LayoutResult m_result;

    editor::interaction::EditorInteraction* m_interaction = nullptr;

    qreal m_zoom = 1.0;
    QPointF m_origin;
    bool m_showBoxes = false;

    QTimer m_blinkTimer;
    bool m_caretVisible = true;

    static constexpr int kCanvasMargin = 36;
};