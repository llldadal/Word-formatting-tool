#include "EditorWindow.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>

namespace editor::demo {

EditorWindow::EditorWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Qt 富文本编辑器 v0.1 - 交互 + DOCX 导出联调窗口"));
    resize(1100, 900);

    canvas_ = new DocumentCanvas(this);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    scroll_->setWidget(canvas_);
    setCentralWidget(scroll_);

    status_ = new QLabel(this);
    statusBar()->addWidget(status_);
    canvas_->onStatus = [this](const QString& text) { status_->setText(text); };

    buildUi();

    canvas_->setFocus();
    loadCase(QStringLiteral("01"));
}

void EditorWindow::buildUi()
{
    QToolBar* toolbar = addToolBar(QStringLiteral("编辑工具"));
    toolbar->setMovable(false);

    caseBox_ = new QComboBox(this);
    caseBox_->addItem(QStringLiteral("01 纯文字"), QStringLiteral("01"));
    caseBox_->addItem(QStringLiteral("02 绕排图片"), QStringLiteral("02"));
    caseBox_->addItem(QStringLiteral("06 浮动文本框"), QStringLiteral("06"));
    toolbar->addWidget(new QLabel(QStringLiteral(" 样例: "), this));
    toolbar->addWidget(caseBox_);
    toolbar->addSeparator();
    connect(caseBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        loadCase(caseBox_->currentData().toString());
    });

    bold_ = toolbar->addAction(QStringLiteral("B"));
    bold_->setObjectName(QStringLiteral("actionBold"));
    bold_->setToolTip(QStringLiteral("加粗 (Ctrl+B)"));
    bold_->setCheckable(true);
    QFont boldFont = bold_->font();
    boldFont.setBold(true);
    bold_->setFont(boldFont);
    connect(bold_, &QAction::triggered, this, [this](bool on) {
        data::TextStyleOverride style;
        style.bold = on;
        canvas_->setCharStyle(style);
    });

    italic_ = toolbar->addAction(QStringLiteral("I"));
    italic_->setObjectName(QStringLiteral("actionItalic"));
    italic_->setToolTip(QStringLiteral("斜体 (Ctrl+I)"));
    italic_->setCheckable(true);
    QFont italicFont = italic_->font();
    italicFont.setItalic(true);
    italic_->setFont(italicFont);
    connect(italic_, &QAction::triggered, this, [this](bool on) {
        data::TextStyleOverride style;
        style.italic = on;
        canvas_->setCharStyle(style);
    });

    underline_ = toolbar->addAction(QStringLiteral("U"));
    underline_->setObjectName(QStringLiteral("actionUnderline"));
    underline_->setToolTip(QStringLiteral("下划线 (Ctrl+U)"));
    underline_->setCheckable(true);
    QFont underlineFont = underline_->font();
    underlineFont.setUnderline(true);
    underline_->setFont(underlineFont);
    connect(underline_, &QAction::triggered, this, [this](bool on) {
        data::TextStyleOverride style;
        style.underline = on;
        canvas_->setCharStyle(style);
    });

    headingBox_ = new QComboBox(this);
    headingBox_->addItem(QStringLiteral("正文"), 0);
    headingBox_->addItem(QStringLiteral("文档标题"), -1);
    for (int level = 1; level <= 3; ++level)
        headingBox_->addItem(QStringLiteral("标题 %1").arg(level), level);
    toolbar->addWidget(new QLabel(QStringLiteral(" 段落: "), this));
    toolbar->addWidget(headingBox_);
    connect(headingBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        canvas_->setHeading(headingBox_->currentData().toInt());
    });

    QAction* heading2 = toolbar->addAction(QStringLiteral("H2"));
    heading2->setObjectName(QStringLiteral("actionHeading2"));
    heading2->setToolTip(QStringLiteral("当前段落设为二级标题"));
    connect(heading2, &QAction::triggered, this, [this]() {
        const int index = headingBox_->findData(2);
        if (index >= 0) {
            const QSignalBlocker blocker(headingBox_);
            headingBox_->setCurrentIndex(index);
        }
        canvas_->setHeading(2);
    });

    toolbar->addSeparator();

    QAction* copyAction = toolbar->addAction(QStringLiteral("复制"));
    copyAction->setObjectName(QStringLiteral("actionCopy"));
    connect(copyAction, &QAction::triggered, this, [this]() {
        canvas_->copySelection();
    });

    QAction* cutAction = toolbar->addAction(QStringLiteral("剪切"));
    cutAction->setObjectName(QStringLiteral("actionCut"));
    connect(cutAction, &QAction::triggered, this, [this]() {
        canvas_->cutSelection();
    });

    QAction* pasteAction = toolbar->addAction(QStringLiteral("粘贴"));
    pasteAction->setObjectName(QStringLiteral("actionPaste"));
    connect(pasteAction, &QAction::triggered, this, [this]() {
        QString text = canvas_->clipboardText();
        if (QClipboard* clipboard = QGuiApplication::clipboard()) {
            const QString systemText = clipboard->text();
            if (!systemText.isEmpty())
                text = systemText;
        }
        canvas_->pasteText(text);
    });

    QAction* selectAllAction = toolbar->addAction(QStringLiteral("全选"));
    selectAllAction->setObjectName(QStringLiteral("actionSelectAll"));
    connect(selectAllAction, &QAction::triggered, this, [this]() {
        canvas_->selectAll();
    });

    QAction* reloadAction = toolbar->addAction(QStringLiteral("重置"));
    reloadAction->setObjectName(QStringLiteral("actionReload"));
    connect(reloadAction, &QAction::triggered, this, [this]() {
        canvas_->reload();
        const QSignalBlocker caseBlocker(caseBox_);
        const QSignalBlocker headingBlocker(headingBox_);
        caseBox_->setCurrentIndex(caseBox_->findData(canvas_->caseId()));
        headingBox_->setCurrentIndex(0);
        bold_->setChecked(false);
        italic_->setChecked(false);
        underline_->setChecked(false);
    });

    toolbar->addSeparator();

    QAction* exportAction = toolbar->addAction(QStringLiteral("导出 DOCX"));
    exportAction->setObjectName(QStringLiteral("actionExport"));
    connect(exportAction, &QAction::triggered, this, [this]() { onExport(); });
}

void EditorWindow::loadCase(const QString& caseId)
{
    canvas_->loadCase(caseId);
    canvas_->setFocus();
}

bool EditorWindow::exportToFile(const QString& path, QString* error)
{
    return canvas_->exportToFile(path, error);
}

QAction* EditorWindow::action(const QString& objectName) const
{
    return findChild<QAction*>(objectName);
}

void EditorWindow::onExport()
{
    const QString directory = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/ui-export");
    QDir().mkpath(directory);
    const QString suggested = directory + QStringLiteral("/edited-")
                            + canvas_->caseId() + QStringLiteral(".docx");

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 DOCX"), suggested,
        QStringLiteral("Word 文档 (*.docx)"));
    if (path.isEmpty())
        return;

    QString error;
    if (canvas_->exportToFile(path, &error)) {
        QMessageBox::information(this, QStringLiteral("导出完成"),
                                 QStringLiteral("文件已写出:\n%1").arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
    }
}

} // 命名空间 editor::demo 结束
