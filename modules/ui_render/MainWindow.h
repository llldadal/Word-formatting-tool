#pragma once

#include "LayoutResult.h"
#include "DocumentSnapshot.h"
#include <QMainWindow>

namespace Ui {
class MainWindow;
}

class CanvasWidget;
class QActionGroup;
class QEvent;
class QLabel;
class QFontComboBox;
class QDoubleSpinBox;
class QComboBox;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 排版结果与文档快照必须成对、同版本更新，否则画布会因版本检查拒绝绘制正文。
    void updateLayoutData(const editor::data::DocumentSnapshot& snapshot,
                          const editor::data::LayoutResult& result);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void newDocument();
    void exportWord();
    void onPointerMoved(const QPointF& paperPoint);
    void applyFontFamily(const QFont& font);
    void applyBold(bool checked);
    void applyItalic(bool checked);
    void applyUnderline(bool checked);
    void applyAlignment();
    void applyParagraphStyle(int index);
    void onZoomWheel(int angleDeltaY, const QPointF& globalPos);

private:
    void buildToolBar();
    void setZoom(qreal zoom);
    void updateStatus();
    void showResult(const QString& text);

    Ui::MainWindow* ui = nullptr;

    editor::data::DocumentSnapshot m_snapshot;
    editor::data::LayoutResult m_layoutResult;

    QFontComboBox* m_fontCombo = nullptr;
    QDoubleSpinBox* m_sizeSpin = nullptr;
    QComboBox* m_styleCombo = nullptr;
    QActionGroup* m_alignGroup = nullptr;

    QLabel* m_pointerLabel = nullptr;
    QLabel* m_caretLabel = nullptr;
    QLabel* m_overflowLabel = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QLabel* m_resultLabel = nullptr;
};