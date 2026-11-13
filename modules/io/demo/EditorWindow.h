#pragma once

#include "DocumentCanvas.h"

#include <QMainWindow>

class QAction;
class QComboBox;
class QLabel;
class QScrollArea;

namespace editor::demo {

class EditorWindow : public QMainWindow
{
public:
    explicit EditorWindow(QWidget* parent = nullptr);

    DocumentCanvas* canvas() const { return canvas_; }
    void loadCase(const QString& caseId);

    // 供自动自测绕开文件对话框直接导出。
    bool exportToFile(const QString& path, QString* error = nullptr);

    // 按 objectName 取工具栏动作，自测可以真的 trigger 一次。
    QAction* action(const QString& objectName) const;

private:
    void buildUi();
    void onExport();

    DocumentCanvas* canvas_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* caseBox_ = nullptr;
    QComboBox* headingBox_ = nullptr;
    QAction* bold_ = nullptr;
    QAction* italic_ = nullptr;
    QAction* underline_ = nullptr;
};

} // namespace editor::demo
