#pragma once

// 第六刀：真实交互画布。
// 把 QWidget 的鼠标 / 键盘 / 输入法事件翻译成 InteractionController 的输出，
// 用本地替身落到新快照、重新投影版面，然后把新几何画回屏幕。
// 本类不声明 Q_OBJECT，事件接线全部用普通 C++ 回调和 lambda，省掉 moc 依赖。

#include "DocumentSnapshot.h"
#include "HitTest.h"
#include "InteractionController.h"
#include "LayoutResult.h"

#include <QSize>
#include <QString>
#include <QVariant>
#include <QWidget>
#include <QtGui/qevent.h>

#include <functional>

class QPainter;

namespace editor::demo {

class DocumentCanvas : public QWidget
{
public:
    explicit DocumentCanvas(QWidget* parent = nullptr);

    void loadCase(const QString& caseId);
    void reload();

    bool exportToFile(const QString& path, QString* error = nullptr);

    // 供工具栏和自动自测共用的动作入口。
    void pasteText(const QString& text);
    void setCharStyle(const data::TextStyleOverride& style);
    void setHeading(int level); // 0 正文，-1 文档标题，1..9 章节标题
    void copySelection();
    void cutSelection();
    void selectAll();
    void setPreedit(const QString& text);
    void commitIme(const QString& text);

    const data::DocumentSnapshot& document() const { return doc_; }
    const data::LayoutResult& layout() const { return layout_; }
    const interaction::InteractionState& interactionState() const { return controller_.state(); }
    int appliedRequests() const { return appliedRequests_; }
    const QString& clipboardText() const { return clipboardText_; }
    const QString& lastError() const { return lastError_; }
    const QString& caseId() const { return caseId_; }

    QSize paperSize() const;

    std::function<void(const QString&)> onStatus;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    bool commit(const interaction::InteractionOutput& output);
    void publishStatus(const QString& message);
    void drawText(QPainter* painter) const;
    void drawImages(QPainter* painter) const;

    data::DocumentSnapshot doc_;
    data::LayoutResult layout_;
    interaction::InteractionController controller_;
    interaction::Hit anchor_;
    bool dragging_ = false;
    int appliedRequests_ = 0;
    QString caseId_ = QStringLiteral("01");
    QString clipboardText_;
    QString lastError_;
};

} // namespace editor::demo
