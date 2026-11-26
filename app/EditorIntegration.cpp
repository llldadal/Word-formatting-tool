#include "EditorIntegration.h"

#include "CanvasWidget.h"
#include "EditorInput.h"
#include "MainWindow.h"

#include <QAction>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputMethod>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaMethod>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>

#include <algorithm>
#include <cmath>

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

std::unique_ptr<QMimeData> cloneMimeData(const QMimeData& src)
{
    auto copy = std::make_unique<QMimeData>();
    for (const QString& fmt : src.formats()) {
        copy->setData(fmt, src.data(fmt));
    }
    if (src.hasText()) {
        copy->setText(src.text());
    }
    if (src.hasImage()) {
        copy->setImageData(src.imageData());
    }
    return copy;
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
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
        if (!writingClipboard_)
            fallbackClipboard_.reset();
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
            if (kind == interaction::EditActionKind::Copy) {
                writeClipboard(session_.copySelection());
            } else if (kind == interaction::EditActionKind::Cut) {
                writeClipboard(session_.cutSelection());
            } else if (kind == interaction::EditActionKind::Paste) {
                const QMimeData* sysMime = QGuiApplication::clipboard()->mimeData();
                const QString richMime = QString::fromLatin1(interaction::kRichTextMimeType);
                const QString imgMime = QString::fromLatin1(interaction::kImageMimeType);
                const bool hasSystemPayload = sysMime &&
                    (sysMime->hasFormat(richMime) || sysMime->hasFormat(imgMime) ||
                     sysMime->hasImage() || (sysMime->hasText() && !sysMime->text().isEmpty()));
                if (hasSystemPayload) {
                    session_.pasteFromMimeData(sysMime);
                } else if (fallbackClipboard_) {
                    session_.pasteFromMimeData(fallbackClipboard_.get());
                } else {
                    session_.pasteFromMimeData(sysMime);
                }
            }
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
        if (points < 6.0)
            return;
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

    alignLeft_ = action(QStringLiteral("actionAlignLeft"));
    alignCenter_ = action(QStringLiteral("actionAlignCenter"));
    alignRight_ = action(QStringLiteral("actionAlignRight"));
    alignJustify_ = action(QStringLiteral("actionAlignJustify"));
    const auto bindAlign = [&](QAction* act, data::ParagraphAlignment align) {
        input_->addAction(act);
        bind(act, &QAction::triggered, "applyAlignment()", [this, align](bool) {
            session_.setParagraphAlignment(align);
            input_->setFocus();
        });
    };
    bindAlign(alignLeft_, data::ParagraphAlignment::Left);
    bindAlign(alignCenter_, data::ParagraphAlignment::Center);
    bindAlign(alignRight_, data::ParagraphAlignment::Right);
    bindAlign(alignJustify_, data::ParagraphAlignment::Justify);

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
    disconnectPlaceholder(canvas_, &CanvasWidget::zoomWheelRequested, window_, "onZoomWheel(int,QPointF)");
    connect(canvas_, &CanvasWidget::zoomWheelRequested, this, [this](int angleDeltaY, const QPointF& globalPos) {
        const qreal factor = std::pow(1.1, static_cast<qreal>(angleDeltaY) / 120.0);
        setZoomAt(canvas_->zoom() * factor, globalPos);
    });
}

void EditorIntegration::refresh(bool layoutChanged)
{
    if (layoutChanged)
        window_.updateLayoutData(session_.snapshot(), session_.layout());
    const bool dragging = session_.isDragging();
    if (!dragging) {
        if (caret_)
            caret_->setText(QStringLiteral("光标：%1").arg(session_.state().caret.offset));
        refreshToolbar();
        if (const auto rect = input_->cursorRectangle(); input_->hasFocus() && rect.isValid()) {
            const auto point = rect.center().toPoint();
            scroll_->ensureVisible(point.x(), point.y(), 16, 16);
        }
    }
    input_->update();
    if (!dragging) {
        input_->refreshInputMethod();
    }
}

void EditorIntegration::refreshToolbar()
{
    const auto style = session_.currentSelectionStyle();
    if (style.fontFamily.has_value()) {
        const bool needUpdate = (font_->currentIndex() < 0) ||
            (font_->currentFont().family() != *style.fontFamily) ||
            (font_->lineEdit() && font_->lineEdit()->text() != *style.fontFamily);
        if (needUpdate) {
            const QSignalBlocker fontBlocker(font_);
            font_->setCurrentFont(QFont(*style.fontFamily));
            if (font_->lineEdit() && font_->lineEdit()->text().isEmpty()) {
                font_->setEditText(*style.fontFamily);
            }
        }
    } else if (font_->currentIndex() >= 0 || (font_->lineEdit() && !font_->lineEdit()->text().isEmpty())) {
        const QSignalBlocker fontBlocker(font_);
        font_->setCurrentIndex(-1);
        if (font_->lineEdit()) {
            font_->lineEdit()->clear();
        }
    }

    const qreal targetSize = style.fontSizePt.value_or(size_->minimum());
    if (!qFuzzyCompare(size_->value(), targetSize)) {
        const QSignalBlocker sizeBlocker(size_);
        size_->setValue(targetSize);
    }

    const bool isBold = style.bold.value_or(false);
    if (bold_->isChecked() != isBold) {
        const QSignalBlocker boldBlocker(bold_);
        bold_->setChecked(isBold);
    }
    const bool isItalic = style.italic.value_or(false);
    if (italic_->isChecked() != isItalic) {
        const QSignalBlocker italicBlocker(italic_);
        italic_->setChecked(isItalic);
    }
    const bool isUnderline = style.underline.value_or(false);
    if (underline_->isChecked() != isUnderline) {
        const QSignalBlocker underlineBlocker(underline_);
        underline_->setChecked(isUnderline);
    }

    const auto para = session_.currentParagraphSummary();
    int targetIndex = -1;
    if (para.role.has_value() && (*para.role != data::ParagraphRole::Heading || para.headingLevel.has_value())) {
        int level = 0;
        if (*para.role == data::ParagraphRole::Title)
            level = -1;
        else if (*para.role == data::ParagraphRole::Heading)
            level = *para.headingLevel;
        targetIndex = paragraph_->findData(level);
    }
    if (paragraph_->currentIndex() != targetIndex) {
        const QSignalBlocker paragraphBlocker(paragraph_);
        paragraph_->setCurrentIndex(targetIndex);
    }

    const auto align = para.alignment;
    const auto syncAlign = [](QAction* act, bool checked) {
        if (act && act->isChecked() != checked) {
            const QSignalBlocker blocker(act);
            act->setChecked(checked);
        }
    };
    const bool wantLeft = align && *align == data::ParagraphAlignment::Left;
    const bool wantCenter = align && *align == data::ParagraphAlignment::Center;
    const bool wantRight = align && *align == data::ParagraphAlignment::Right;
    const bool wantJustify = align && *align == data::ParagraphAlignment::Justify;
    if (!wantLeft) syncAlign(alignLeft_, false);
    if (!wantCenter) syncAlign(alignCenter_, false);
    if (!wantRight) syncAlign(alignRight_, false);
    if (!wantJustify) syncAlign(alignJustify_, false);
    if (wantLeft) syncAlign(alignLeft_, true);
    if (wantCenter) syncAlign(alignCenter_, true);
    if (wantRight) syncAlign(alignRight_, true);
    if (wantJustify) syncAlign(alignJustify_, true);
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
    setZoomAt(zoom, viewport->mapToGlobal(viewport->rect().center()));
}

void EditorIntegration::setZoomAt(qreal zoom, const QPointF& globalAnchorPos)
{
    auto* viewport = scroll_->viewport();
    const QPointF localPos = viewport->mapFromGlobal(globalAnchorPos);
    const QPointF anchorViewport = viewport->rect().contains(localPos.toPoint())
        ? localPos
        : QPointF(viewport->rect().center());
    const auto anchor = canvas_->widgetToPaper(canvas_->mapFrom(viewport, anchorViewport));
    canvas_->setZoom(zoom);
    const auto target = canvas_->mapTo(viewport, canvas_->paperToWidget(anchor));
    const auto offset = target - anchorViewport;
    auto* horizontal = scroll_->horizontalScrollBar();
    auto* vertical = scroll_->verticalScrollBar();
    horizontal->setValue(horizontal->value() + qRound(offset.x()));
    vertical->setValue(vertical->value() + qRound(offset.y()));
    if (zoom_)
        zoom_->setText(QStringLiteral("缩放：%1%").arg(qRound(canvas_->zoom() * 100)));
    input_->update();
    input_->refreshInputMethod();
}

void EditorIntegration::writeClipboard(std::unique_ptr<QMimeData> mime)
{
    if (!mime)
        return;

    auto backup = cloneMimeData(*mime);
    auto* clipboard = QGuiApplication::clipboard();
    const QString richMime = QString::fromLatin1(interaction::kRichTextMimeType);
    const QString imgMime = QString::fromLatin1(interaction::kImageMimeType);

    writingClipboard_ = true;
    bool ok = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto chunk = (attempt == 0) ? std::move(mime) : cloneMimeData(*backup);
        clipboard->setMimeData(chunk.release());
        const QMimeData* current = clipboard->mimeData();
        ok = clipboard->ownsClipboard() ||
             (current && (current->hasFormat(richMime) || current->hasFormat(imgMime)));
        if (ok)
            break;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 30);
        QThread::msleep(15);
    }
    writingClipboard_ = false;

    if (ok) {
        fallbackClipboard_.reset();
    } else {
        fallbackClipboard_ = std::move(backup);
    }
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
