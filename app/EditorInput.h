#pragma once

#include <QWidget>

class CanvasWidget;

namespace editor::app {

class EditorSession;

// 覆盖在原画布上，接收输入并绘制光标、选区和输入法预编辑。
class EditorInput : public QWidget {
public:
    EditorInput(CanvasWidget& canvas, EditorSession& session);

    QRectF cursorRectangle() const;
    void refreshInputMethod();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QRectF toWidget(const QRectF& paperRect) const;
    QRectF caretOnPaper() const;
    QFont preeditFont() const;

    CanvasWidget& canvas_;
    EditorSession& session_;
};

} // namespace editor::app
