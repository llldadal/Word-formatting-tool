#pragma once

#include "SnapEngine.h"
#include "ContainerDragEngine.h"
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

    enum class DragMode {
        None,
        MoveContainer,
        MoveImage,
        ResizeContainer,
        ResizeImage
    };

    struct DragState {
        DragMode mode = DragMode::None;
        bool dragStarted = false;
        interaction::ContainerHitType activeHandle = interaction::ContainerHitType::None;
        data::ContainerId containerId;
        data::ContainerType containerType = data::ContainerType::Text;
        data::ContainerId initialParentId;
        std::size_t initialIndex = 0;
        QPointF pressPaperPos;
        QRectF initialRect;
        QSizeF initialDisplaySize;
        data::ContainerGeometry initialGeometry;
        QRectF parentContentRect;
        data::LayoutResult baseLayout;
        interaction::DragDropPlan plan;
        data::ContainerGeometry candidateGeometry;
        QSizeF candidateDisplaySize;
    };

    DragState dragState_;
    data::ContainerId hoveredBorderId_;
};

} // namespace editor::app
