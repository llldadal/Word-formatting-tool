#pragma once

#include "EditorSession.h"

class CanvasWidget;
class MainWindow;
class QAction;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QScrollArea;

namespace editor::app {

class EditorInput;

class EditorIntegration : public QObject {
public:
    explicit EditorIntegration(MainWindow& window);
    ~EditorIntegration() override;

    EditorSession& session() { return session_; }

private:
    void bindActions();
    void refresh(bool layoutChanged);
    void refreshToolbar();
    void setCharStyle(const data::TextStyleOverride& style);
    void setParagraphRole(int level);
    void setZoom(qreal zoom);
    void insertImage(data::PlacementMode placement);
    void exportWord();

    MainWindow& window_;
    EditorSession session_;
    CanvasWidget* canvas_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    EditorInput* input_ = nullptr;
    QFontComboBox* font_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QComboBox* paragraph_ = nullptr;
    QAction* bold_ = nullptr;
    QAction* italic_ = nullptr;
    QAction* underline_ = nullptr;
    QLabel* caret_ = nullptr;
    QLabel* zoom_ = nullptr;
};

} // namespace editor::app
