#include "EditorIntegration.h"

#include "CanvasWidget.h"
#include "EditorInput.h"
#include "MainWindow.h"

#include <QAction>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputMethod>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMetaMethod>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>

namespace editor::app {
namespace {

template<class T>
T* requiredChild(const QObject& owner, const QString& name = {})
{
    auto* child = owner.findChild<T*>(name);
    if (!child)
        qFatal("Missing %s %s in MainWindow", T::staticMetaObject.className(), qPrintable(name));
    return child;
}

template<class Sender, class Signal>
void disconnectPlaceholder(Sender* sender, Signal signal, MainWindow& window, const char* slot)
{
    const auto& metaObject = MainWindow::staticMetaObject;
    const int index = metaObject.indexOfSlot(slot);
    if (index >= 0)
        QObject::disconnect(sender, QMetaMethod::fromSignal(signal), &window, metaObject.method(index));
}

} // namespace

EditorIntegration::EditorIntegration(MainWindow& window) : QObject(&window), window_(window)
{
    canvas_ = requiredChild<CanvasWidget>(window_, QStringLiteral("canvas"));
    scroll_ = requiredChild<QScrollArea>(window_, QStringLiteral("scrollArea"));
    input_ = new EditorInput(*canvas_, session_);

    auto* toolbar = requiredChild<QToolBar>(window_, QStringLiteral("mainToolBar"));
    font_ = requiredChild<QFontComboBox>(*toolbar);
    size_ = requiredChild<QDoubleSpinBox>(*toolbar);
    const auto combos = toolbar->findChildren<QComboBox*>();
    const auto paragraph = std::find_if(combos.begin(), combos.end(),
        [this](QComboBox* combo) { return combo != font_; });
    if (paragraph == combos.end())
        qFatal("Missing the paragraph selector in MainWindow");
    paragraph_ = *paragraph;
    size_->setKeyboardTracking(false);
    paragraph_->setItemData(0, 0);
    paragraph_->setItemData(1, -1);
    paragraph_->setItemData(2, 1);
    for (int level = 2; level <= 9; ++level)
        paragraph_->addItem(QStringLiteral("标题 %1").arg(level), level);

    for (auto* label : window_.statusBar()->findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("光标：")))
            caret_ = label;
        if (label->text().startsWith(QStringLiteral("缩放：")))
            zoom_ = label;
    }

    bindActions();
    connect(&session_, &EditorSession::changed, this, &EditorIntegration::refresh);
    connect(&session_, &EditorSession::editRejected, this, [this](const QString& reason) {
        window_.statusBar()->showMessage(reason, 8000);
    });
    connect(&session_, &EditorSession::clipboardRequested, this, [](const QString& text) {
        QGuiApplication::clipboard()->setText(text);
    });
    connect(scroll_->horizontalScrollBar(), &QScrollBar::valueChanged, input_, &EditorInput::refreshInputMethod);
    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, input_, &EditorInput::refreshInputMethod);
    refresh(true);
    input_->setFocus();
}

EditorIntegration::~EditorIntegration()
{
    disconnect(&session_, nullptr, this, nullptr);
    delete input_;
}

void EditorIntegration::bindActions()
{
    const auto action = [this](const QString& name) { return requiredChild<QAction>(window_, name); };
    const auto bind = [this](auto* sender, auto signal, const char* slot, auto handler) {
        disconnectPlaceholder(sender, signal, window_, slot);
        connect(sender, signal, this, handler);
    };

    bind(action(QStringLiteral("actionNew")), &QAction::triggered, "newDocument()", [this] {
        QGuiApplication::inputMethod()->reset();
        if (session_.newDocument())
            window_.statusBar()->showMessage(QStringLiteral("已新建文档"), 3000);
        input_->setFocus();
    });
    bind(action(QStringLiteral("actionExportWord")), &QAction::triggered, "exportWord()", &EditorIntegration::exportWord);
    auto* selectAll = action(QStringLiteral("actionSelectAll"));
    input_->addAction(selectAll);
    connect(selectAll, &QAction::triggered, this, [this] {
        interaction::EditAction request;
        request.kind = interaction::EditActionKind::SelectAll;
        session_.action(request);
        input_->setFocus();
    });
    auto* editMenu = requiredChild<QMenu>(window_, QStringLiteral("menuEdit"));
    const auto addClipboardAction = [this, editMenu, selectAll](const QString& name, const QString& title,
        QKeySequence::StandardKey shortcut, interaction::EditActionKind kind) {
        auto* item = new QAction(title, editMenu);
        item->setObjectName(name);
        item->setShortcuts(shortcut);
        editMenu->insertAction(selectAll, item);
        input_->addAction(item);
        connect(item, &QAction::triggered, this, [this, kind] {
            interaction::EditAction request;
            request.kind = kind;
            if (kind == interaction::EditActionKind::Paste)
                request.text = QGuiApplication::clipboard()->text();
            session_.action(request);
            input_->setFocus();
        });
    };
    addClipboardAction(QStringLiteral("actionCut"), QStringLiteral("剪切"), QKeySequence::Cut,
                       interaction::EditActionKind::Cut);
    addClipboardAction(QStringLiteral("actionCopy"), QStringLiteral("复制"), QKeySequence::Copy,
                       interaction::EditActionKind::Copy);
    addClipboardAction(QStringLiteral("actionPaste"), QStringLiteral("粘贴"), QKeySequence::Paste,
                       interaction::EditActionKind::Paste);
    editMenu->insertSeparator(selectAll);

    font_->setInsertPolicy(QComboBox::NoInsert);
    disconnectPlaceholder(font_, &QFontComboBox::currentFontChanged, window_, "applyFontFamily(QFont)");
    connect(font_, &QComboBox::activated, this, [this](int index) {
        const auto family = font_->itemText(index);
        if (family.isEmpty())
            return;

        data::TextStyleOverride style;
        style.fontFamily = family;
        setCharStyle(style);
        input_->setFocus();
    });
    connect(size_, &QDoubleSpinBox::valueChanged, this, [this](double points) {
        data::TextStyleOverride style;
        style.fontSizePt = points;
        setCharStyle(style);
    });
    disconnectPlaceholder(paragraph_, &QComboBox::currentIndexChanged, window_, "applyParagraphStyle(int)");
    connect(paragraph_, &QComboBox::activated, this, [this](int index) {
        setParagraphRole(paragraph_->itemData(index).toInt());
        input_->setFocus();
    });

    bold_ = action(QStringLiteral("actionBold"));
    italic_ = action(QStringLiteral("actionItalic"));
    underline_ = action(QStringLiteral("actionUnderline"));
    const auto bindToggle = [&](QAction* toggle, const char* slot, std::optional<bool> data::TextStyleOverride::* field) {
        input_->addAction(toggle);
        bind(toggle, &QAction::triggered, slot, [this, field](bool checked) {
            data::TextStyleOverride style;
            style.*field = checked;
            setCharStyle(style);
            input_->setFocus();
        });
    };
    bindToggle(bold_, "applyBold(bool)", &data::TextStyleOverride::bold);
    bindToggle(italic_, "applyItalic(bool)", &data::TextStyleOverride::italic);
    bindToggle(underline_, "applyUnderline(bool)", &data::TextStyleOverride::underline);

    auto* formatMenu = requiredChild<QMenu>(window_, QStringLiteral("menuFormat"));
    auto* insertMenu = new QMenu(QStringLiteral("插入(&I)"), &window_);
    window_.menuBar()->insertMenu(formatMenu->menuAction(), insertMenu);
    auto* picture = insertMenu->addAction(QStringLiteral("图片（顺序排列）…"));
    picture->setObjectName(QStringLiteral("actionInsertImage"));
    connect(picture, &QAction::triggered, this, [this] { insertImage(data::PlacementMode::Flow); });
    auto* floating = insertMenu->addAction(QStringLiteral("图片（文字绕排）…"));
    floating->setObjectName(QStringLiteral("actionInsertFloatingImage"));
    connect(floating, &QAction::triggered, this, [this] { insertImage(data::PlacementMode::Floating); });

    auto* color = formatMenu->addAction(QStringLiteral("文字颜色…"));
    color->setObjectName(QStringLiteral("actionTextColor"));
    connect(color, &QAction::triggered, this, [this] {
        const auto chosen = QColorDialog::getColor(session_.currentStyle().color, &window_, QStringLiteral("文字颜色"));
        if (chosen.isValid()) {
            data::TextStyleOverride style;
            style.color = chosen;
            setCharStyle(style);
        }
        input_->setFocus();
    });

    // MainWindow 用 lambda 连接缩放动作。接管后才能按当前滚动值恢复缩放锚点。
    const auto bindZoom = [this, &action](const QString& name, auto handler) {
        auto* item = action(name);
        disconnect(item, QMetaMethod::fromSignal(&QAction::triggered), &window_, QMetaMethod{});
        connect(item, &QAction::triggered, this, handler);
    };
    bindZoom(QStringLiteral("actionZoomIn"), [this] { setZoom(canvas_->zoom() * 1.1); });
    bindZoom(QStringLiteral("actionZoomOut"), [this] { setZoom(canvas_->zoom() / 1.1); });
    bindZoom(QStringLiteral("actionZoomActual"), [this] { setZoom(1); });
    bindZoom(QStringLiteral("actionFitWidth"), [this] {
        setZoom(canvas_->zoomToFitWidth(scroll_->viewport()->width()));
    });
}

void EditorIntegration::refresh(bool layoutChanged)
{
    if (layoutChanged)
        window_.updateLayoutData(session_.snapshot(), session_.layout());
    if (caret_)
        caret_->setText(QStringLiteral("光标：%1").arg(session_.state().caret.offset));
    refreshToolbar();
    if (const auto rect = input_->cursorRectangle(); input_->hasFocus() && rect.isValid()) {
        const auto point = rect.center().toPoint();
        scroll_->ensureVisible(point.x(), point.y(), 16, 16);
    }
    input_->update();
    input_->refreshInputMethod();
}

void EditorIntegration::refreshToolbar()
{
    const auto style = session_.currentStyle();
    const QSignalBlocker fontBlocker(font_);
    const QSignalBlocker sizeBlocker(size_);
    const QSignalBlocker paragraphBlocker(paragraph_);
    font_->setCurrentFont(QFont(style.fontFamily));
    size_->setValue(style.fontSizePt);
    bold_->setChecked(style.bold);
    italic_->setChecked(style.italic);
    underline_->setChecked(style.underline);

    int level = 0;
    if (const auto* paragraph = session_.currentParagraph()) {
        if (paragraph->role == data::ParagraphRole::Title)
            level = -1;
        if (paragraph->role == data::ParagraphRole::Heading)
            level = paragraph->headingLevel;
    }
    paragraph_->setCurrentIndex(paragraph_->findData(level));
}

void EditorIntegration::setCharStyle(const data::TextStyleOverride& style)
{
    interaction::EditAction action;
    action.kind = interaction::EditActionKind::SetCharFormat;
    action.charStyle = style;
    session_.action(action);
}

void EditorIntegration::setParagraphRole(int level)
{
    interaction::EditAction action;
    action.kind = interaction::EditActionKind::SetParagraphFormat;
    if (level > 0) {
        action.role = data::ParagraphRole::Heading;
        action.headingLevel = level;
    } else {
        action.role = level == 0 ? data::ParagraphRole::Body : data::ParagraphRole::Title;
    }
    session_.action(action);
}

void EditorIntegration::setZoom(qreal zoom)
{
    auto* viewport = scroll_->viewport();
    const auto anchor = canvas_->widgetToPaper(canvas_->mapFrom(viewport, QPointF(viewport->rect().center())));
    canvas_->setZoom(zoom);
    const auto target = canvas_->mapTo(viewport, canvas_->paperToWidget(anchor)).toPoint();
    const auto offset = target - viewport->rect().center();
    auto* horizontal = scroll_->horizontalScrollBar();
    auto* vertical = scroll_->verticalScrollBar();
    horizontal->setValue(horizontal->value() + offset.x());
    vertical->setValue(vertical->value() + offset.y());
    if (zoom_)
        zoom_->setText(QStringLiteral("缩放：%1%").arg(qRound(canvas_->zoom() * 100)));
    input_->update();
    input_->refreshInputMethod();
}

void EditorIntegration::insertImage(data::PlacementMode placement)
{
    QGuiApplication::inputMethod()->commit();
    QStringList patterns;
    for (const auto& format : QImageReader::supportedImageFormats())
        patterns.push_back(QStringLiteral("*.%1").arg(QString::fromLatin1(format)));
    const auto path = QFileDialog::getOpenFileName(&window_, QStringLiteral("插入图片"), {},
        QStringLiteral("图片 (%1)").arg(patterns.join(QLatin1Char(' '))));
    input_->setFocus();
    if (path.isEmpty())
        return;

    const auto id = session_.insertImage(path, placement);
    if (!id)
        return;
    const auto& images = session_.layout().images;
    const auto image = std::find_if(images.begin(), images.end(),
        [&id](const auto& item) { return item.containerId == *id; });
    if (image != images.end()) {
        const auto point = canvas_->paperToWidget(image->rect.topLeft()).toPoint();
        scroll_->ensureVisible(point.x(), point.y(), 24, 24);
    }
    window_.statusBar()->showMessage(QStringLiteral("已插入图片"), 3000);
}

void EditorIntegration::exportWord()
{
    QGuiApplication::inputMethod()->commit();
    QFileDialog dialog(&window_, QStringLiteral("导出 Word"), QStringLiteral("文档.docx"),
                       QStringLiteral("Word 文档 (*.docx)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("docx"));
    if (dialog.exec() != QDialog::Accepted)
        return;

    const auto path = dialog.selectedFiles().value(0);
    const auto result = session_.exportTo(path);
    if (result.ok()) {
        window_.statusBar()->showMessage(QStringLiteral("已导出 %1").arg(path), 5000);
    } else {
        auto message = QStringLiteral("导出失败　%1").arg(result.errorMessage);
        if (!result.containerId.isEmpty())
            message += QStringLiteral("（容器 %1）").arg(result.containerId);
        window_.statusBar()->showMessage(message, 10000);
    }
    input_->setFocus();
}

} // namespace editor::app
