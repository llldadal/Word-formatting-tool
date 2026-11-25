#include "EditorInput.h"

#include "CanvasWidget.h"
#include "EditorSession.h"
#include "InteractionController.h"
#include "Selection.h"
#include "TextBoundary.h"
#include "ContainerHitTest.h"
#include "ContainerOverlay.h"
#include "ContextMenuBuilder.h"

#include <QAction>
#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>

namespace editor::app {
namespace {

const data::ContainerNode* findNode(const data::ContainerNode& root, const data::ContainerId& id)
{
    if (root.id == id)
        return &root;
    for (const auto& child : root.children) {
        if (const auto* found = findNode(child, id))
            return found;
    }
    return nullptr;
}

const data::ContainerNode* findParent(const data::ContainerNode& root, const data::ContainerId& id)
{
    for (const auto& child : root.children) {
        if (child.id == id)
            return &root;
        if (const auto* found = findParent(child, id))
            return found;
    }
    return nullptr;
}

QRectF findInitialBoxRect(const data::ContainerId& id, const data::LayoutResult& layout)
{
    // 优先取 containers 外框（与 ContainerOverlay / LayoutModule 含 padding 的矩形一致）
    for (const auto& box : layout.containers) {
        if (box.containerId == id)
            return box.rect;
    }
    for (const auto& img : layout.images) {
        if (img.containerId == id)
            return img.rect;
    }
    return QRectF{};
}

Qt::CursorShape cursorForHit(const interaction::ContainerHitResult& hit)
{
    switch (hit.type) {
    case interaction::ContainerHitType::ResizeHandleLeft:
    case interaction::ContainerHitType::ResizeHandleRight:
        return Qt::SizeHorCursor;
    case interaction::ContainerHitType::ResizeHandleTop:
    case interaction::ContainerHitType::ResizeHandleBottom:
        return Qt::SizeVerCursor;
    case interaction::ContainerHitType::ResizeHandleTopLeft:
    case interaction::ContainerHitType::ResizeHandleBottomRight:
        return Qt::SizeFDiagCursor;
    case interaction::ContainerHitType::ResizeHandleTopRight:
    case interaction::ContainerHitType::ResizeHandleBottomLeft:
        return Qt::SizeBDiagCursor;
    case interaction::ContainerHitType::ContainerBorder:
    case interaction::ContainerHitType::FloatingImage:
        return Qt::SizeAllCursor;
    case interaction::ContainerHitType::TextFragment:
        return Qt::IBeamCursor;
    default:
        return Qt::ArrowCursor;
    }
}

QRectF findParentContentRect(const data::ContainerId& id, const data::DocumentSnapshot& snapshot, const data::LayoutResult& layout)
{
    const auto* parentNode = findParent(snapshot.root, id);
    if (!parentNode || parentNode->id == snapshot.root.id) {
        return layout.contentRect;
    }
    for (const auto& box : layout.containers) {
        if (box.containerId == parentNode->id)
            return box.contentRect;
    }
    return layout.contentRect;
}

static bool findNodeInfo(const data::ContainerNode& root,
                         const data::ContainerId& id,
                         data::ContainerId& outParentId,
                         std::size_t& outIndex)
{
    for (std::size_t i = 0; i < root.children.size(); ++i) {
        if (root.children[i].id == id) {
            outParentId = root.id;
            outIndex = i;
            return true;
        }
        if (findNodeInfo(root.children[i], id, outParentId, outIndex)) {
            return true;
        }
    }
    return false;
}

} // namespace

EditorInput::EditorInput(CanvasWidget& canvas, EditorSession& session)
    : QWidget(&canvas), canvas_(canvas), session_(session)
{
    setObjectName(QStringLiteral("editorInput"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setMouseTracking(true);

    auto* layout = new QVBoxLayout(&canvas);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(this);
    canvas.setFocusProxy(this);
}

QRectF EditorInput::toWidget(const QRectF& paperRect) const
{
    if (!paperRect.isValid())
        return {};
    return {canvas_.paperToWidget(paperRect.topLeft()), paperRect.size() * canvas_.zoom()};
}

QFont EditorInput::preeditFont() const
{
    const auto style = session_.currentStyle();
    QFont font(style.fontFamily);
    font.setPixelSize(qMax(1, qRound(style.fontSizePt * data::DocumentUnitsPerInch / 72)));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    return font;
}

QRectF EditorInput::caretOnPaper() const
{
    const auto& state = session_.state();
    auto rect = state.caret.rect;
    if (state.preedit.rect.isValid() && !state.preedit.text.isEmpty()) {
        rect = state.preedit.rect;
        const QFontMetricsF metrics(preeditFont());
        rect.translate(metrics.horizontalAdvance(state.preedit.text.left(state.preedit.cursorInPreedit)), 0);
        rect.setHeight(qMax(rect.height(), metrics.height()));
    }
    return rect;
}

QRectF EditorInput::cursorRectangle() const
{
    return toWidget(caretOnPaper());
}

void EditorInput::refreshInputMethod()
{
    if (hasFocus())
        QGuiApplication::inputMethod()->update(Qt::ImQueryInput | Qt::ImAnchorRectangle);
}

void EditorInput::paintEvent(QPaintEvent*)
{
    const auto& state = session_.state();
    QPainter painter(this);
    painter.translate(canvas_.paperToWidget(QPointF{}));
    painter.scale(canvas_.zoom(), canvas_.zoom());
    painter.setRenderHint(QPainter::TextAntialiasing);

    // 1. 绘制空文档提示
    ui::ContainerOverlay::paintEmptyPlaceholder(painter, session_.layout(), canvas_.zoom());

    // 2. 绘制悬停提示边框与选中的容器边框/手柄
    if (dragState_.mode == DragMode::None &&
        !hoveredBorderId_.isEmpty() &&
        !session_.containerSelection().isSelected(hoveredBorderId_)) {
        ui::ContainerOverlay::paintHoverBorder(painter, hoveredBorderId_, session_.layout(), canvas_.zoom());
    }
    ui::ContainerOverlay::paintSelection(painter, session_.containerSelection(), session_.layout(), canvas_.zoom(), &session_.snapshot());

    // 3. 绘制文字选区
    if (state.selection) {
        for (const auto& rect : state.selection->rects)
            painter.fillRect(rect, QColor(47, 128, 237, 64));
    }
    if (!hasFocus())
        return;

    // 4. 输入法预编辑
    if (!state.preedit.text.isEmpty() && state.preedit.rect.isValid()) {
        const auto font = preeditFont();
        const QFontMetricsF metrics(font);
        const auto top = state.preedit.rect.topLeft();
        const QRectF box(top, QSizeF(metrics.horizontalAdvance(state.preedit.text),
                                    qMax(state.preedit.rect.height(), metrics.height())));
        painter.fillRect(box, QColor(255, 249, 224));
        painter.setFont(font);
        painter.setPen(session_.currentStyle().color);
        const qreal baseline = box.center().y() + (metrics.ascent() - metrics.descent()) / 2;
        painter.drawText(QPointF(box.left(), baseline), state.preedit.text);
        painter.drawLine(box.bottomLeft(), box.bottomRight());
    }

    // 5. 文字光标（容器被选中时隐藏）
    if (session_.containerSelection().empty()) {
        const auto caret = caretOnPaper();
        if (caret.isValid()) {
            painter.setPen(QPen(QColor(26, 26, 26), 1 / canvas_.zoom()));
            painter.drawLine(caret.topLeft(), caret.bottomLeft());
        }
    }

    // 6. 拖拽时的跟随指示框、插槽线、分组高亮与吸附对齐线
    if (dragState_.mode != DragMode::None && dragState_.dragStarted) {
        const qreal safeZoom = qMax<qreal>(0.1, canvas_.zoom());
        const QRectF ghostRect = dragState_.plan.targetRect.isValid() ? dragState_.plan.targetRect : dragState_.initialRect;

        if (dragState_.mode == DragMode::MoveImage || dragState_.mode == DragMode::ResizeImage) {
            painter.save();
            painter.fillRect(ghostRect, QColor(47, 128, 237, 24));
            QPen ghostPen(QColor(47, 128, 237), 1.5 / safeZoom, Qt::DashLine);
            painter.setPen(ghostPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(ghostRect);

            if (dragState_.mode == DragMode::ResizeImage) {
                const QString sizeText = QStringLiteral("%1 × %2")
                    .arg(qRound(dragState_.candidateDisplaySize.width()))
                    .arg(qRound(dragState_.candidateDisplaySize.height()));
                QFont badgeFont(QStringLiteral("Microsoft YaHei"));
                badgeFont.setPixelSize(qMax(8, qRound(11.0 / safeZoom)));
                painter.setFont(badgeFont);
                const QFontMetricsF fm(badgeFont);
                const qreal padX = 5.0 / safeZoom;
                const qreal padY = 2.0 / safeZoom;
                const QRectF pill(ghostRect.left(),
                                  ghostRect.bottom() + 4.0 / safeZoom,
                                  fm.horizontalAdvance(sizeText) + padX * 2,
                                  fm.height() + padY * 2);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(26, 26, 26, 190));
                painter.drawRoundedRect(pill, 3.0 / safeZoom, 3.0 / safeZoom);
                painter.setPen(Qt::white);
                painter.drawText(pill, Qt::AlignCenter, sizeText);
            }
            painter.restore();
        } else if (dragState_.mode == DragMode::MoveContainer || dragState_.mode == DragMode::ResizeContainer) {
            painter.save();
            painter.fillRect(ghostRect, QColor(47, 128, 237, 25));
            QPen ghostPen(QColor(47, 128, 237), 1.5 / safeZoom, Qt::DashLine);
            painter.setPen(ghostPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(ghostRect);
            painter.restore();
        }

        if (dragState_.plan.targetParentHighlightRect.isValid()) {
            painter.save();
            QPen groupPen(QColor(39, 174, 96), 2.0 / safeZoom, Qt::DashLine);
            painter.setPen(groupPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(dragState_.plan.targetParentHighlightRect);
            painter.restore();
        }

        if (dragState_.plan.insertionIndicator.has_value()) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QLineF& line = *dragState_.plan.insertionIndicator;
            QPen linePen(QColor(47, 128, 237), 2.5 / safeZoom, Qt::SolidLine);
            painter.setPen(linePen);
            painter.drawLine(line);
            const qreal r = 3.5 / safeZoom;
            painter.setBrush(QColor(47, 128, 237));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(line.p1(), r, r);
            painter.drawEllipse(line.p2(), r, r);
            painter.restore();
        }

        if (!dragState_.plan.guidelines.empty()) {
            painter.save();
            QPen pen(QColor(255, 0, 127), 1.0 / safeZoom, Qt::DashLine);
            painter.setPen(pen);
            for (const auto& gl : dragState_.plan.guidelines) {
                if (gl.orientation == interaction::GuidelineOrientation::Vertical) {
                    painter.drawLine(QPointF(gl.position, gl.start), QPointF(gl.position, gl.end));
                } else {
                    painter.drawLine(QPointF(gl.start, gl.position), QPointF(gl.end, gl.position));
                }
            }
            painter.restore();
        }
    }
}

void EditorInput::mousePressEvent(QMouseEvent* event)
{
    const auto paper = canvas_.widgetToPaper(event->position());

    if (event->button() == Qt::LeftButton) {
        hoveredBorderId_.clear();
        setFocus(Qt::MouseFocusReason);
        if (!session_.state().preedit.text.isEmpty())
            QGuiApplication::inputMethod()->commit();

        const auto hit = interaction::hitTestScene(paper, session_.layout(), session_.snapshot(),
                                                   session_.containerSelection(), canvas_.zoom());

        // 优先检查调整大小手柄
        if (hit.isHandle()) {
            const auto* node = findNode(session_.snapshot().root, hit.containerId);
            if (node) {
                dragState_.mode = (node->type == data::ContainerType::Image)
                                      ? DragMode::ResizeImage
                                      : DragMode::ResizeContainer;
                dragState_.dragStarted = false;
                dragState_.activeHandle = hit.type;
                dragState_.containerId = hit.containerId;
                dragState_.containerType = node->type;
                dragState_.pressPaperPos = paper;
                dragState_.initialRect = findInitialBoxRect(hit.containerId, session_.layout());
                dragState_.initialDisplaySize = node->image.displaySize;
                dragState_.candidateDisplaySize = node->image.displaySize;
                dragState_.initialGeometry = node->geometry;
                dragState_.candidateGeometry = node->geometry;
                dragState_.parentContentRect = findParentContentRect(hit.containerId, session_.snapshot(), session_.layout());
                dragState_.baseLayout = session_.baseLayout();
                dragState_.plan = {};
                findNodeInfo(session_.snapshot().root, hit.containerId, dragState_.initialParentId, dragState_.initialIndex);
                event->accept();
                return;
            }
        }

        // 边框或图片主体：选中并准备拖拽
        if (hit.type == interaction::ContainerHitType::ContainerBorder ||
            hit.type == interaction::ContainerHitType::FloatingImage) {
            session_.selectContainer(hit.containerId, event->modifiers().testFlag(Qt::ControlModifier));

            const auto* node = findNode(session_.snapshot().root, hit.containerId);
            if (node) {
                dragState_.mode = (node->type == data::ContainerType::Image)
                                      ? DragMode::MoveImage
                                      : DragMode::MoveContainer;
                dragState_.dragStarted = false;
                dragState_.activeHandle = interaction::ContainerHitType::None;
                dragState_.containerId = hit.containerId;
                dragState_.containerType = node->type;
                dragState_.pressPaperPos = paper;
                dragState_.initialRect = findInitialBoxRect(hit.containerId, session_.layout());
                dragState_.initialDisplaySize = node->image.displaySize;
                dragState_.candidateDisplaySize = node->image.displaySize;
                dragState_.initialGeometry = node->geometry;
                dragState_.candidateGeometry = node->geometry;
                dragState_.parentContentRect = findParentContentRect(hit.containerId, session_.snapshot(), session_.layout());
                dragState_.baseLayout = session_.baseLayout();
                dragState_.plan = {};
                findNodeInfo(session_.snapshot().root, hit.containerId, dragState_.initialParentId, dragState_.initialIndex);
            }

            event->accept();
            update();
            return;
        }

        if (hit.type == interaction::ContainerHitType::PageBlankArea ||
            hit.type == interaction::ContainerHitType::GroupBlankArea) {
            // 点击空白处：取消容器选中
            session_.clearContainerSelection();
        }

        if (hit.type == interaction::ContainerHitType::TextFragment) {
            // 点击正文：清除容器选中，进入文字编辑与拖选
            session_.clearContainerSelection();
            if (!session_.beginSelection(paper, event->modifiers().testFlag(Qt::ShiftModifier))) {
                event->ignore();
                return;
            }
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        setFocus(Qt::MouseFocusReason);
        const auto hit = interaction::hitTestScene(paper, session_.layout(), session_.snapshot(),
                                                   session_.containerSelection(), canvas_.zoom());
        if (hit.type == interaction::ContainerHitType::ContainerBorder ||
            hit.type == interaction::ContainerHitType::FloatingImage) {
            if (!session_.containerSelection().isSelected(hit.containerId)) {
                session_.selectContainer(hit.containerId, false);
            }
        }

        ui::ContextMenuCallbacks callbacks;
        callbacks.onAddText = [this](const data::ContainerId& parentId) {
            session_.addTextContainer(parentId);
            setFocus(Qt::OtherFocusReason);
            update();
        };
        callbacks.onAddGroup = [this](const data::ContainerId& parentId) {
            session_.addGroupContainer(parentId);
            setFocus(Qt::OtherFocusReason);
            update();
        };
        callbacks.onDeleteSelected = [this]() {
            session_.removeSelectedContainers();
            update();
        };
        callbacks.onCopy = [this]() {
            interaction::EditAction act;
            act.kind = interaction::EditActionKind::Copy;
            session_.action(act);
        };
        callbacks.onCut = [this]() {
            interaction::EditAction act;
            act.kind = interaction::EditActionKind::Cut;
            session_.action(act);
        };
        callbacks.onPaste = [this]() {
            interaction::EditAction act;
            act.kind = interaction::EditActionKind::Paste;
            act.text = QGuiApplication::clipboard()->text();
            session_.action(act);
        };
        callbacks.onUndo = [this]() {
            session_.undo();
            update();
        };
        callbacks.onRedo = [this]() {
            session_.redo();
            update();
        };

        auto menu = ui::ContextMenuBuilder::buildMenu(hit, session_.containerSelection(), callbacks,
                                                      session_.canUndo(), session_.canRedo(), this);
        menu->exec(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void EditorInput::mouseMoveEvent(QMouseEvent* event)
{
    const auto paper = canvas_.widgetToPaper(event->position());

    // 1. 拖拽中处理（移动容器、图片、容器/图片手柄缩放）
    if (event->buttons().testFlag(Qt::LeftButton) && dragState_.mode != DragMode::None) {
        const QPointF delta = paper - dragState_.pressPaperPos;
        const qreal debouncePaper = 3.0 / qMax<qreal>(0.25, canvas_.zoom());
        if (!dragState_.dragStarted) {
            if (std::hypot(delta.x(), delta.y()) >= debouncePaper) {
                dragState_.dragStarted = true;
            } else {
                event->accept();
                return;
            }
        }

        if (dragState_.mode == DragMode::MoveImage) {
            dragState_.plan = interaction::ContainerDragEngine::planImageMove(
                dragState_.containerId, dragState_.initialParentId, dragState_.initialRect,
                delta, paper, dragState_.baseLayout, session_.baseSnapshot(), canvas_.zoom());

            auto newGeo = dragState_.initialGeometry;
            newGeo.position = dragState_.plan.localPosition;
            dragState_.candidateGeometry = newGeo;

            session_.updateLiveDragWithPlan(dragState_.containerId, dragState_.plan, newGeo);
            update();
            event->accept();
            return;
        }

        if (dragState_.mode == DragMode::MoveContainer) {
            dragState_.plan = interaction::ContainerDragEngine::planContainerMove(
                dragState_.containerId, dragState_.containerType, dragState_.initialRect,
                delta, paper, dragState_.baseLayout, session_.baseSnapshot(), canvas_.zoom());

            auto newGeo = dragState_.initialGeometry;
            newGeo.position = dragState_.plan.localPosition;
            dragState_.candidateGeometry = newGeo;

            session_.updateLiveDragWithPlan(dragState_.containerId, dragState_.plan, newGeo);
            update();
            event->accept();
            return;
        }

        if (dragState_.mode == DragMode::ResizeContainer) {
            const bool isLeft = (dragState_.activeHandle == interaction::ContainerHitType::ResizeHandleLeft);
            dragState_.plan = interaction::ContainerDragEngine::planResizeWidth(
                dragState_.containerId, isLeft, dragState_.initialRect,
                delta, dragState_.parentContentRect, dragState_.baseLayout, canvas_.zoom());

            auto newGeo = dragState_.initialGeometry;
            newGeo.width = dragState_.plan.targetRect.width();
            if (isLeft) {
                newGeo.position.setX(qMax<qreal>(0, dragState_.plan.localPosition.x()));
            }
            dragState_.candidateGeometry = newGeo;

            session_.updateLiveDragGeometry(dragState_.containerId, newGeo);
            update();
            event->accept();
            return;
        }

        if (dragState_.mode == DragMode::ResizeImage) {
            const bool isCorner = (dragState_.activeHandle == interaction::ContainerHitType::ResizeHandleTopLeft ||
                                   dragState_.activeHandle == interaction::ContainerHitType::ResizeHandleTopRight ||
                                   dragState_.activeHandle == interaction::ContainerHitType::ResizeHandleBottomLeft ||
                                   dragState_.activeHandle == interaction::ContainerHitType::ResizeHandleBottomRight);
            const bool keepAspect = isCorner
                ? !event->modifiers().testFlag(Qt::ShiftModifier)
                : event->modifiers().testFlag(Qt::ShiftModifier);

            dragState_.plan = interaction::ContainerDragEngine::planImageResize(
                dragState_.containerId, dragState_.activeHandle,
                dragState_.initialRect, dragState_.initialDisplaySize, dragState_.initialGeometry,
                delta, dragState_.parentContentRect, dragState_.baseLayout,
                keepAspect, canvas_.zoom());

            auto newGeo = dragState_.initialGeometry;
            newGeo.position = dragState_.plan.localPosition;
            dragState_.candidateGeometry = newGeo;
            dragState_.candidateDisplaySize = dragState_.plan.targetDisplaySize.value_or(dragState_.initialDisplaySize);

            session_.updateLiveImageResize(dragState_.containerId, dragState_.candidateDisplaySize, newGeo);
            update();
            event->accept();
            return;
        }
    }

    // 2. 悬停样式更新与容器边框高亮提示
    if (!event->buttons().testFlag(Qt::LeftButton)) {
        const auto hit = interaction::hitTestScene(paper, session_.layout(), session_.snapshot(),
                                                   session_.containerSelection(), canvas_.zoom());
        setCursor(cursorForHit(hit));

        const data::ContainerId newHovered = (hit.type == interaction::ContainerHitType::ContainerBorder)
            ? hit.containerId
            : data::ContainerId{};
        if (newHovered != hoveredBorderId_) {
            hoveredBorderId_ = newHovered;
            update();
        }
    }

    if (!event->buttons().testFlag(Qt::LeftButton) || !session_.dragSelection(paper)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // 普通移动交给父画布上报。拖选消费事件，由这里上报坐标。
    emit canvas_.pointerMoved(paper);
    event->accept();
}

void EditorInput::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (dragState_.mode != DragMode::None) {
            const auto id = dragState_.containerId;
            const auto mode = dragState_.mode;
            const auto plan = dragState_.plan;
            const auto finalGeo = dragState_.candidateGeometry;
            const auto finalDisplaySize = dragState_.candidateDisplaySize;
            const bool dragStarted = dragState_.dragStarted;
            dragState_ = DragState{};

            if (dragStarted) {
                if (mode == DragMode::MoveContainer || mode == DragMode::MoveImage) {
                    session_.moveContainer(id, plan.targetParentId, plan.targetIndex, finalGeo);
                } else if (mode == DragMode::ResizeContainer) {
                    session_.commitContainerGeometry(id, finalGeo);
                } else if (mode == DragMode::ResizeImage) {
                    session_.commitImageResize(id, finalDisplaySize, finalGeo);
                }
            } else {
                session_.clearLiveDrag();
            }
            update();
            event->accept();
            return;
        }

        if (session_.endSelection()) {
            event->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void EditorInput::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && dragState_.mode != DragMode::None) {
        dragState_ = DragState{};
        session_.clearLiveDrag();
        update();
        event->accept();
        return;
    }
    // 如果按键直接送到画布，仍调用菜单使用的 QAction。
    const QKeySequence shortcut(event->keyCombination());
    for (auto* action : actions()) {
        if (action->shortcuts().contains(shortcut)) {
            action->trigger();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && !session_.state().preedit.text.isEmpty())
        QGuiApplication::inputMethod()->reset();

    // 容器被选中时，按 Delete 或 Backspace 执行容器删除
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (!session_.containerSelection().empty()) {
            session_.removeSelectedContainers();
            event->accept();
            return;
        }
    }

    // 快捷键撤销 / 重做
    if (event->matches(QKeySequence::Undo) || (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Z)) {
        if (session_.undo()) {
            event->accept();
            update();
            return;
        }
    }
    if (event->matches(QKeySequence::Redo) ||
        (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_Z) ||
        (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Y)) {
        if (session_.redo()) {
            event->accept();
            update();
            return;
        }
    }

    session_.key({event->key(), event->modifiers(), event->text()}, {});
    event->accept();
}

void EditorInput::inputMethodEvent(QInputMethodEvent* event)
{
    interaction::ImeInput input;
    input.preedit = event->preeditString();
    input.commit = event->commitString();
    input.replacementStart = event->replacementStart();
    input.replacementLength = event->replacementLength();
    for (const auto& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            input.preeditCursor = attribute.start;
            break;
        }
    }
    session_.ime(input);
    event->accept();
}

QVariant EditorInput::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const auto& state = session_.state();
    const auto& caret = state.caret;
    const auto* text = interaction::findTextContent(session_.snapshot(), caret.containerId);
    const int anchor = state.selectionAnchor.value_or(caret.offset);
    switch (query) {
    case Qt::ImEnabled:
        return text != nullptr;
    case Qt::ImCursorRectangle:
        return cursorRectangle();
    case Qt::ImAnchorRectangle:
        return toWidget(interaction::caretRect(session_.layout(), caret.containerId, anchor));
    case Qt::ImSurroundingText:
        return text ? text->text : QString{};
    case Qt::ImCursorPosition:
    case Qt::ImAbsolutePosition:
        return caret.offset;
    case Qt::ImAnchorPosition:
        return anchor;
    case Qt::ImCurrentSelection:
        return interaction::selectedText(state, session_.snapshot());
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void EditorInput::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    refreshInputMethod();
    update();
}

void EditorInput::focusOutEvent(QFocusEvent* event)
{
    session_.endSelection();
    if (!session_.state().preedit.text.isEmpty()) {
        interaction::EditAction cancel;
        cancel.kind = interaction::EditActionKind::CancelPreedit;
        session_.action(cancel);
    }
    QWidget::focusOutEvent(event);
    update();
}

} // namespace editor::app
