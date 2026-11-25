#include "CanvasWidget.h"
#include "EditorIntegration.h"
#include "MainWindow.h"
#include "TestFixtures.h"
#include "TextBoundary.h"
#include "ZipWriter.h"
#include "ContainerHitTest.h"
#include "CrossContainerCursor.h"
#include "SnapEngine.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFontComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>

#include <algorithm>

namespace {

namespace app = editor::app;
namespace data = editor::data;
namespace edit = editor::interaction;
namespace io = editor::io;

void check(bool ok, const char* message)
{
    if (!ok)
        qFatal("%s", message);
}

const data::TextContent& currentText(app::EditorSession& session)
{
    const auto* text = edit::findTextContent(session.snapshot(), session.state().caret.containerId);
    check(text != nullptr, "Expected an active text container");
    return *text;
}

QAction& action(MainWindow& window, const char* name)
{
    auto* result = window.findChild<QAction*>(QString::fromLatin1(name));
    check(result != nullptr, name);
    return *result;
}

void sendKey(QWidget& input, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {})
{
    QKeyEvent event(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(&input, &event);
}

void sendMouse(QWidget& input, QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, point, input.mapToGlobal(point), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&input, &event);
}

void clickAt(QWidget& input, QPointF point)
{
    sendMouse(input, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
}

QRectF cursorRectangle(QWidget& input)
{
    QInputMethodQueryEvent query(Qt::ImCursorRectangle);
    QApplication::sendEvent(&input, &query);
    return query.value(Qt::ImCursorRectangle).toRectF();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Expected a DOCX file");
    return file.readAll();
}

QByteArray readDocumentXml(const QString& path)
{
    const auto package = readFile(path);
    QByteArray xml;
    QStringList issues;
    check(io::zip::verifyArchive(package).isEmpty() &&
          io::zip::extractEntry(package, QStringLiteral("word/document.xml"), &xml, &issues),
          "Export did not produce a valid DOCX package");
    return xml;
}

void chooseFile(MainWindow& window, const QString& path)
{
    QTimer::singleShot(0, &window, [path] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "Expected the file dialog");
        dialog->selectFile(path);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
}

void testParagraphLayout(MainWindow& window, app::EditorSession& session, QWidget& input)
{
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("AB"));
    sendKey(input, Qt::Key_Return);
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("CD"));
    sendKey(input, Qt::Key_Home, Qt::ControlModifier);

    const auto combos = window.findChild<QToolBar*>(QStringLiteral("mainToolBar"))->findChildren<QComboBox*>();
    const auto role = std::find_if(combos.begin(), combos.end(), [](QComboBox* combo) {
        return combo->findData(-1) >= 0 && combo->findData(2) >= 0;
    });
    check(role != combos.end(), "Missing the paragraph style control");
    auto* size = window.findChild<QDoubleSpinBox*>();
    check(size != nullptr, "Missing the font size control");
    const auto font = [&] { return session.layout().textLayouts.front()->font(); };
    const int bodyPixels = font().pixelSize();
    const qreal bodyHeight = session.layout().textFragments.front().rect.height();

    (*role)->activated((*role)->findData(-1));
    const int titlePixels = font().pixelSize();
    check(titlePixels > bodyPixels && font().bold() && size->value() > 12,
          "The Title toolbar option did not change the laid-out text and displayed font size");
    (*role)->activated((*role)->findData(1));
    const int heading1Pixels = font().pixelSize();
    (*role)->activated((*role)->findData(2));
    check(titlePixels > heading1Pixels && heading1Pixels > font().pixelSize() &&
          font().pixelSize() > bodyPixels && font().bold(), "Heading levels did not change the laid-out text");
    (*role)->activated((*role)->findData(0));
    check(font().pixelSize() == bodyPixels && !font().bold() && size->value() == 12,
          "Restoring Body did not restore the inherited font");

    sendKey(input, Qt::Key_Right, Qt::ShiftModifier);
    size->setValue(72);
    const auto& layout = session.layout();
    check(layout.textFragments.size() == 2, "Expected two lines of text");
    const auto& large = layout.textFragments.front();
    const auto& normal = layout.textFragments.back();
    const auto largeLine = layout.textLayouts[large.layoutIndex]->lineAt(large.lineIndex);
    check(largeLine.height() > bodyHeight && large.rect.height() >= largeLine.height() &&
          normal.rect.height() == bodyHeight && normal.rect.top() >= large.rect.bottom(),
          "A large font must expand its line without clipping, overlapping or expanding the next line");

    auto wrapCase = editor::testing::makeLayoutCase(QStringLiteral("02"));
    auto& body = wrapCase.document.root.children.back();
    data::TextFormatSpan largeCharacter;
    largeCharacter.range = {0, 1};
    largeCharacter.style.fontSizePt = 72;
    body.text.formatSpans.push_back(largeCharacter);
    check(session.loadSnapshot(wrapCase.document), "Could not load the large text wrap case");
    const auto& wrapped = session.layout();
    const auto picture = std::find_if(wrapped.containers.begin(), wrapped.containers.end(), [](const auto& box) {
        return box.containerId == QStringLiteral("photo1");
    });
    check(picture != wrapped.containers.end(), "Missing the floating image box");
    bool tallRowReachedImage = false;
    for (const auto& fragment : wrapped.textFragments) {
        if (fragment.containerId != body.id)
            continue;
        check(!fragment.rect.intersects(picture->outerRect), "The expanded text row overlaps the floating image");
        if (fragment.sourceRange.start == 0)
            tallRowReachedImage = fragment.rect.top() < picture->outerRect.top() &&
                                  fragment.rect.bottom() > picture->outerRect.top();
    }
    check(tallRowReachedImage, "The first large character must wrap beside the image that starts below it");

    check(session.newDocument(), "Could not reset the document after testing paragraph layout");
}

void testWrapUsesCurrentRowHeight(app::EditorSession& session)
{
    auto doc = editor::testing::makeLayoutCase(QStringLiteral("02")).document;
    auto body = doc.root.children.back();
    auto picture = body.children.front();
    // The line separator starts a new visual row inside the same paragraph.
    body.children.clear();
    body.text.text = QStringLiteral("small\u2028BIG");
    data::ParagraphInfo paragraph;
    paragraph.range = {0, int(body.text.text.size())};
    body.text.paragraphs = {paragraph};
    body.text.formatSpans = {{{6, paragraph.range.end}, {}}};
    body.text.formatSpans.front().style.fontSizePt = 48;
    doc.root.children = {body};
    check(session.loadSnapshot(doc), "Could not load the mixed-height paragraph");
    check(session.layout().textFragments.size() == 2, "Expected two rows in a single paragraph");
    const QRectF firstRow = session.layout().textFragments.front().rect;
    const qreal secondHeight = session.layout().textFragments.back().rect.height();
    check(secondHeight > firstRow.height() + 1, "Expected the later row to be taller");

    picture.geometry.position = QPointF(0, firstRow.height() + 1);
    picture.geometry.margin = {};
    picture.geometry.padding = {};
    picture.image.displaySize = QSizeF(firstRow.width(), 32);
    doc.root.children.front().children.push_back(picture);
    check(session.loadSnapshot(doc), "Could not add a full-width image below the small row");
    const auto& wrapped = session.layout();
    check(wrapped.images.size() == 1 && wrapped.textFragments.size() == 2, "Expected one image and two text rows");
    check(wrapped.textFragments.front().rect == firstRow,
          "A tall later row must not make an earlier small row avoid the image");
    check(wrapped.textFragments.back().rect.top() >= wrapped.images.front().rect.bottom(),
          "The tall row must still avoid the image");
    check(wrapped.textFragments.front().sourceRange.end == wrapped.textFragments.back().sourceRange.start &&
          wrapped.textFragments.back().sourceRange.end == paragraph.range.end,
          "Retrying a blocked row lost or repeated text");

    // The second image appears only after the tall row grows, so the existing
    // left and right fragments need to be measured again as a single row.
    const qreal width = firstRow.width();
    picture.geometry.position = QPointF(width / 3, firstRow.height());
    picture.image.displaySize = QSizeF(width / 8, 42);
    auto lowerImage = picture;
    lowerImage.id = QStringLiteral("photo-lower");
    lowerImage.geometry.position = QPointF(width * 2 / 3, firstRow.height() + 32);
    auto& retryBody = doc.root.children.front();
    retryBody.children = {picture, lowerImage};
    retryBody.text.text = QStringLiteral("small\u2028W ") + QStringLiteral("word ").repeated(40);
    retryBody.text.paragraphs.front().range.end = int(retryBody.text.text.size());
    retryBody.text.formatSpans.front().range = {6, 7};
    check(session.loadSnapshot(doc), "Could not load the split-row retry case");
    const auto& split = session.layout();
    check(split.images.size() == 2 && split.textFragments.front().rect == firstRow,
          "The preceding row must stay intact when a split row is retried");
    int next = 0;
    int tallFragments = 0;
    for (const auto& fragment : split.textFragments) {
        check(fragment.sourceRange.start == next, "Retrying multiple row fragments lost or repeated text");
        next = fragment.sourceRange.end;
        if (fragment.rect.top() == firstRow.bottom() && fragment.rect.height() > 32)
            ++tallFragments;
        for (const auto& entry : split.images)
            check(!fragment.rect.intersects(entry.rect), "A retried fragment overlaps one of its obstacles");
    }
    check(tallFragments >= 2 && next == retryBody.text.text.size(),
          "The tall row should still wrap around both images");
    check(session.newDocument(), "Could not reset after the wrap height test");
}

void testEditing(MainWindow& window, app::EditorSession& session, QWidget& input, const QTemporaryDir& output)
{
    const auto firstId = session.state().caret.containerId;
    check(currentText(session).text.isEmpty() && cursorRectangle(input).isValid(),
          "The blank document must accept input at offset zero");

    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("AB"));
    sendKey(input, Qt::Key_Home);
    sendKey(input, Qt::Key_Right, Qt::ShiftModifier);
    check(session.state().selection && session.state().selection->range.end == 1,
          "Shift+Right did not select the first character");
    sendKey(input, Qt::Key_X, Qt::ControlModifier);
    check(currentText(session).text == QStringLiteral("B") &&
          QApplication::clipboard()->text() == QStringLiteral("A"), "Cut must update both text and clipboard");
    sendKey(input, Qt::Key_V, Qt::ControlModifier);
    check(currentText(session).text == QStringLiteral("AB"), "Paste did not restore the original text");

    action(window, "actionSelectAll").trigger();
    QApplication::clipboard()->clear();
    action(window, "actionCopy").trigger();
    check(QApplication::clipboard()->text() == QStringLiteral("AB") &&
          currentText(session).text == QStringLiteral("AB"), "The Copy menu did not copy the selection");
    action(window, "actionCut").trigger();
    check(currentText(session).text.isEmpty(), "The Cut menu did not remove the selection");
    action(window, "actionPaste").trigger();
    check(currentText(session).text == QStringLiteral("AB"), "The Paste menu did not restore the text");

    action(window, "actionSelectAll").trigger();
    action(window, "actionBold").trigger();
    sendKey(input, Qt::Key_B, Qt::ControlModifier);
    check(!session.currentStyle().bold, "Ctrl+B must turn off bold applied by the menu");
    sendKey(input, Qt::Key_B, Qt::ControlModifier);
    check(session.currentStyle().bold, "Ctrl+B must turn bold back on");
    auto* size = window.findChild<QDoubleSpinBox*>();
    check(size != nullptr, "Missing the original font size control");
    size->setValue(15);

    QTimer::singleShot(0, &window, [] {
        auto* dialog = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "Expected the color dialog");
        dialog->setCurrentColor(QColor(32, 64, 96));
        dialog->accept();
    });
    action(window, "actionTextColor").trigger();
    const auto& spans = currentText(session).formatSpans;
    check(spans.size() == 1 && spans.front().style.bold.value_or(false) &&
          spans.front().style.fontSizePt == 15 && spans.front().style.color == QColor(32, 64, 96),
          "The toolbar did not update the selection format");

    const auto combos = window.findChild<QToolBar*>(QStringLiteral("mainToolBar"))->findChildren<QComboBox*>();
    const auto foundRole = std::find_if(combos.begin(), combos.end(), [](QComboBox* combo) {
        return !qobject_cast<QFontComboBox*>(combo);
    });
    check(foundRole != combos.end(), "Missing the original paragraph control");
    auto* role = *foundRole;
    role->activated(role->findData(2));
    check(currentText(session).paragraphs.front().headingLevel == 2 &&
          currentText(session).paragraphs.front().role == data::ParagraphRole::Heading,
          "The paragraph toolbar did not set Heading 2");

    const auto path = output.filePath(QStringLiteral("edited.docx"));
    chooseFile(window, path);
    action(window, "actionExportWord").trigger();
    const auto xml = readDocumentXml(path);
    check(xml.contains("<w:pStyle w:val=\"Heading2\"/>") && xml.contains(">AB</w:t>"),
          "The menu did not export the current text and paragraph role");
    check(window.statusBar()->currentMessage().startsWith(QStringLiteral("已导出")), "Missing export status");

    QTimer::singleShot(0, &window, [] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "Expected the DOCX path dialog");
        dialog->reject();
    });
    action(window, "actionExportWord").trigger();

    action(window, "actionNew").trigger();
    check(session.state().caret.containerId != firstId && currentText(session).text.isEmpty() &&
          session.layout().documentRevision == session.snapshot().revision,
          "New must replace the document and reset its caret and layout");
}

void testInsertImages(MainWindow& window, app::EditorSession& session, QWidget& input, const QTemporaryDir& output)
{
    check(session.newDocument(), "Could not create a document for image insertion");
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("正文围绕图片继续排版。").repeated(20));
    sendKey(input, Qt::Key_Home, Qt::ControlModifier);
    const auto body = session.state().caret.containerId;

    const auto imagePath = output.filePath(QStringLiteral("插图.png"));
    QImage image(220, 100, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    check(image.save(imagePath), "Could not generate the source PNG");

    chooseFile(window, imagePath);
    action(window, "actionInsertFloatingImage").trigger();
    const auto& root = session.snapshot().root;
    check(root.children.size() == 1 && root.children.front().children.size() == 1,
          "The floating image menu did not add a child to the body");
    const auto& floating = root.children.front().children.front();
    check(floating.placement == data::PlacementMode::Floating && session.layout().images.size() == 1,
          "The imported image was not rendered as a floating obstacle");
    const auto placed = session.layout().images.front().rect;
    const auto& fragments = session.layout().textFragments;
    check(std::any_of(fragments.begin(), fragments.end(), [&](const auto& item) {
        return item.containerId == body && item.rect.top() >= placed.top()
            && item.rect.bottom() <= placed.bottom() && item.rect.right() < placed.left();
    }), "The actual layout has no text beside the imported image");
    const auto resource = floating.image.resourceId;

    edit::EditAction bold;
    bold.kind = edit::EditActionKind::SetCharFormat;
    bold.charStyle.bold = true;
    session.action(bold);
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("图文"));
    chooseFile(window, imagePath);
    action(window, "actionInsertImage").trigger();
    check(session.snapshot().root.children.size() == 2 &&
          session.snapshot().root.children.back().placement == data::PlacementMode::Flow &&
          session.snapshot().root.children.back().image.resourceId == resource &&
          session.snapshot().resources.size() == 1, "Flow insertion did not reuse the same original image");
    check(session.layout().images.size() == 2 && session.layout().documentRevision == session.snapshot().revision,
          "Adding the flow image did not refresh the layout");

    const auto path = output.filePath(QStringLiteral("inserted-images.docx"));
    check(session.exportTo(path).ok(), "The document with the imported images could not be exported");
    const auto xml = readDocumentXml(path);
    check(xml.contains("<wp:wrapSquare") && xml.contains("<wp:inline"), "DOCX lost an image placement mode");

    const auto revision = session.snapshot().revision;
    check(!session.insertImage(output.filePath(QStringLiteral("missing.png")), data::PlacementMode::Flow)
          && revision == session.snapshot().revision, "A failed import changed the document");
}

void testFontSelection(MainWindow& window, app::EditorSession& session, QWidget& input)
{
    auto* font = window.findChild<QFontComboBox*>();
    check(font && font->lineEdit(), "Missing the editable font selector");
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("ABC"));
    action(window, "actionSelectAll").trigger();

    const auto revision = session.snapshot().revision;
    const auto unknownFamily = QStringLiteral("Unknown Editor Font");
    font->setEditText(unknownFamily);
    sendKey(*font->lineEdit(), Qt::Key_Return);
    check(session.snapshot().revision == revision && font->findText(unknownFamily) == -1,
          "An unknown font must not change the document or be added to the list");

    const int courier = font->findText(QStringLiteral("Courier New"));
    const int arial = font->findText(QStringLiteral("Arial"));
    // The offscreen font database may not provide the Windows fonts.
    if (courier >= 0 && arial >= 0) {
        font->setFocus();
        font->setEditText(font->itemText(courier));
        check(session.snapshot().revision == revision, "Typing a font name must not change the document");
        sendKey(*font->lineEdit(), Qt::Key_Return);
        check(currentText(session).formatSpans.size() == 1 &&
              currentText(session).formatSpans.front().style.fontFamily == font->itemText(courier),
              "Confirming a font name must apply it to the selected text");

        sendKey(input, Qt::Key_End);
        font->setFocus();
        font->setCurrentIndex(arial);
        font->activated(arial);
        check(window.focusWidget() == &input, "Font confirmation must return focus to the editor");
        sendKey(input, Qt::Key_unknown, {}, QStringLiteral("Z"));
        check(currentText(session).text == QStringLiteral("ABCZ") &&
              currentText(session).formatSpans.size() == 2 &&
              currentText(session).formatSpans.back().style.fontFamily == font->itemText(arial),
              "Text entered after choosing a font must use that font");
    }
    check(session.newDocument(), "Could not reset the document after the font test");
}

void testIme(app::EditorSession& session, QWidget& input)
{
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("ab"));
    const auto revision = session.snapshot().revision;
    const auto before = cursorRectangle(input);
    QInputMethodEvent preedit(QStringLiteral("ni"), {{QInputMethodEvent::Cursor, 2, 1, {}}});
    QApplication::sendEvent(&input, &preedit);
    check(session.snapshot().revision == revision && currentText(session).text == QStringLiteral("ab") &&
          cursorRectangle(input).x() > before.x(), "Preedit must move the candidate anchor without editing text");

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("你"), -2, 2);
    QApplication::sendEvent(&input, &commit);
    check(currentText(session).text == QStringLiteral("你") && session.state().preedit.text.isEmpty(),
          "IME commit must replace its requested range once");
    sendKey(input, Qt::Key_unknown, {}, QStringLiteral("👩‍💻"));
    sendKey(input, Qt::Key_Backspace);
    check(currentText(session).text == QStringLiteral("你"), "Backspace split a grapheme cluster");
}

void testRejectedBatch()
{
    app::EditorSession session;
    edit::EditAction format;
    format.kind = edit::EditActionKind::SetCharFormat;
    format.charStyle.fontSizePt = -1;
    session.action(format);
    const auto revision = session.snapshot().revision;
    check(!session.key({Qt::Key_A, Qt::NoModifier, QStringLiteral("a")}, {}) &&
          session.snapshot().revision == revision && session.state().caret.offset == 0 &&
          currentText(session).text.isEmpty(), "A failed format request must roll back the insertion");
}

void testRealLayouts(MainWindow& window, app::EditorSession& session, QWidget& input, const QTemporaryDir& output)
{
    auto* canvas = window.findChild<CanvasWidget*>(QStringLiteral("canvas"));
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("scrollArea"));
    check(canvas != nullptr && scroll != nullptr, "Expected the canvas and scroll area from MainWindow.ui");

    for (const auto* id : {"02", "03"}) {
        check(session.loadSnapshot(editor::testing::makeLayoutCase(QString::fromLatin1(id)).document),
              "Failed to load the image wrap case");
        const auto& image = session.layout().images.front().rect;
        const auto& fragments = session.layout().textFragments;
        const auto onRow = [&](const auto& fragment) {
            return fragment.containerId == QStringLiteral("body") &&
                fragment.rect.center().y() > image.top() && fragment.rect.center().y() < image.bottom();
        };
        const auto left = std::find_if(fragments.begin(), fragments.end(), [&](const auto& fragment) {
            return onRow(fragment) && fragment.rect.right() <= image.left();
        });
        const auto right = std::find_if(fragments.begin(), fragments.end(), [&](const auto& fragment) {
            return onRow(fragment) && fragment.rect.left() >= image.right();
        });
        check(left != fragments.end() && right != fragments.end(), "Missing wrap fragments on either side of an image");
        const auto startPoint = canvas->paperToWidget(left->rect.center());
        const auto endPoint = canvas->paperToWidget(right->rect.center());
        int pointerUpdates = 0;
        const auto pointerConnection = QObject::connect(canvas, &CanvasWidget::pointerMoved, &window,
            [&pointerUpdates] { ++pointerUpdates; });
        sendMouse(input, QEvent::MouseMove, startPoint, Qt::NoButton, Qt::NoButton);
        QObject::disconnect(pointerConnection);
        check(pointerUpdates == 1, "Moving the mouse should update the paper coordinates once");

        sendMouse(input, QEvent::MouseButtonPress, startPoint, Qt::LeftButton, Qt::LeftButton);
        sendMouse(input, QEvent::MouseMove, endPoint, Qt::NoButton, Qt::LeftButton);
        sendMouse(input, QEvent::MouseButtonRelease, endPoint, Qt::LeftButton, Qt::NoButton);
        check(session.state().caret.containerId == QStringLiteral("body") &&
              session.state().selection && session.state().selection->rects.size() >= 2,
              "Dragging across the image did not select one source text range");
        sendMouse(input, QEvent::MouseButtonPress, endPoint, Qt::LeftButton, Qt::LeftButton);
        const int anchor = session.state().caret.offset;
        sendMouse(input, QEvent::MouseMove, startPoint, Qt::NoButton, Qt::LeftButton);
        sendMouse(input, QEvent::MouseButtonRelease, startPoint, Qt::LeftButton, Qt::NoButton);
        const int dragEnd = session.state().caret.offset;
        sendKey(input, Qt::Key_Right, Qt::ShiftModifier);
        check(session.state().selectionAnchor == anchor && session.state().caret.offset > dragEnd,
              "Shift+Right lost the anchor after a reverse drag");
        const auto path = output.filePath(QStringLiteral("wrapped-%1.docx").arg(QString::fromLatin1(id)));
        check(session.exportTo(path).ok() && readDocumentXml(path).contains("<wp:wrapSquare"),
              "Export did not keep the image wrap");
    }

    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("06")).document),
          "Failed to load the floating note");
    const auto& fragments = session.layout().textFragments;
    const auto note = std::find_if(fragments.begin(), fragments.end(), [](const auto& fragment) {
        return fragment.containerId == QStringLiteral("note");
    });
    check(note != fragments.end(), "Missing floating note text");
    action(window, "actionZoomIn").trigger();
    clickAt(input, canvas->paperToWidget(note->rect.center()));
    check(session.state().caret.containerId == QStringLiteral("note") && cursorRectangle(input).isValid(),
          "Zoomed mouse input did not locate the floating note");

    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("08")).document),
          "Failed to load the overflow case");
    check(session.layout().layoutBounds.bottom() > session.layout().pageRect.bottom() &&
          canvas->height() > session.layout().pageRect.height() * canvas->zoom(),
          "Canvas clipped the overflow at the paper edge");
    auto* vertical = scroll->verticalScrollBar();
    vertical->setValue(vertical->maximum() / 2);
    const auto paperAtCenter = [&] {
        return canvas->widgetToPaper(canvas->mapFrom(scroll->viewport(), QPointF(scroll->viewport()->rect().center())));
    };
    const qreal yBefore = paperAtCenter().y();
    action(window, "actionZoomIn").trigger();
    check(qAbs(paperAtCenter().y() - yBefore) < 2, "Zoom lost the scroll anchor");
    const auto path = output.filePath(QStringLiteral("overflow.docx"));
    check(session.exportTo(path).status == io::DocxExportStatus::CompatibilityError && !QFile::exists(path),
          "Export must report the overflow compatibility error without writing a file");
}

void testContainerSelectionAndDeletion(MainWindow& window, app::EditorSession& session, QWidget& input)
{
    // 1. 测试单容器选中与删空
    check(session.newDocument(), "Failed to create new document");
    const auto initialId = session.snapshot().root.children.front().id;
    check(!initialId.isEmpty(), "Expected an initial container");

    // 点击边框选中容器
    const auto initialBox = session.layout().containers.front();
    const QPointF borderPoint(initialBox.rect.left(), initialBox.rect.top());
    auto* canvas = window.findChild<CanvasWidget*>();
    clickAt(input, canvas->paperToWidget(borderPoint));
    check(session.containerSelection().isSelected(initialId), "Clicking border did not select container");

    // 正文顶层容器不暴露左右拉伸手柄，左右边缘中点应命中 ContainerBorder
    const auto handleHit = edit::hitTestScene(QPointF(initialBox.rect.left(), initialBox.rect.center().y()),
                                              session.layout(), session.snapshot(),
                                              session.containerSelection(), canvas->zoom());
    check(handleHit.type == edit::ContainerHitType::ContainerBorder,
          "Top-level text container left edge must return ContainerBorder instead of ResizeHandleLeft");

    // 按 Delete 键删除容器
    sendKey(input, Qt::Key_Delete);
    check(session.snapshot().root.children.empty(), "Container was not removed");
    check(session.state().caret.containerId.isEmpty(), "Caret should be cleared when document is empty");
    check(session.containerSelection().empty(), "Selection should be empty");

    // 2. 测试分组删除与光标平移迁移
    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("09")).document),
          "Failed to load case 09");
    const auto& children = session.snapshot().root.children;
    check(children.size() >= 2, "Expected multiple children in case 09");
    const auto groupId = children[0].id;
    const auto afterId = children[1].id;

    // 选中分组并删除，验证级联删除子节点以及光标平移迁移到 afterId
    session.selectContainer(groupId);
    check(session.containerSelection().isSelected(groupId), "Expected group selected");

    sendKey(input, Qt::Key_Delete);
    check(session.snapshot().root.children.size() == 1, "Expected only after container remaining");
    check(session.snapshot().root.children.front().id == afterId, "Expected after container to remain");
    check(session.state().caret.containerId == afterId, "Caret should migrate to afterId");
}

void testPhase2Interactions(MainWindow& window, app::EditorSession& session, QWidget& input)
{
    // 1. 测试添加顶层文本容器并立即获得输入焦点
    check(session.newDocument(), "Failed to create new document");
    const auto initialId = session.state().caret.containerId;
    check(!initialId.isEmpty(), "Expected initial text container");

    const auto addedId = session.addTextContainer({}, QStringLiteral("Second"));
    check(addedId.has_value(), "Failed to add text container");
    check(session.state().caret.containerId == *addedId, "Caret should be moved to new container");
    check(session.state().caret.offset == 0, "Caret offset should be 0 in new container");

    // 立即输入文字，验证焦点已在容器中且无需二次点击
    sendKey(input, Qt::Key_A, {}, QStringLiteral("A"));
    check(session.state().caret.containerId == *addedId, "Caret still in new container");
    check(currentText(session).text.startsWith(QStringLiteral("A")), "Text should be inserted into new container");

    // 2. 测试水平跨容器导航 (Left / Right)
    // 当前光标在 *addedId 的 offset 1 处，按 Home 到 offset 0
    sendKey(input, Qt::Key_Home);
    check(session.state().caret.offset == 0, "Caret should be at start of container 2");

    // 向左按键：应跨入前驱容器 initialId 的末尾
    sendKey(input, Qt::Key_Left);
    check(session.state().caret.containerId == initialId, "Caret should cross left to initial container");
    check(session.state().caret.offset == edit::textLength(session.snapshot(), initialId),
          "Caret should land at the end of initial container");

    // 向右按键：应跨入后继容器 *addedId 的起始
    sendKey(input, Qt::Key_Right);
    check(session.state().caret.containerId == *addedId, "Caret should cross right back to second container");
    check(session.state().caret.offset == 0, "Caret should land at 0");

    // 3. 测试跨容器格式继承 (pendingCharStyle)
    edit::EditAction boldAction;
    boldAction.kind = edit::EditActionKind::SetCharFormat;
    boldAction.charStyle.bold = true;
    session.action(boldAction);
    check(session.state().pendingCharStyle.has_value() && session.state().pendingCharStyle->bold == true,
          "Expected pending bold style");

    // 跨容器向左移动
    sendKey(input, Qt::Key_Left);
    check(session.state().caret.containerId == initialId, "Crossed left");
    check(session.state().pendingCharStyle.has_value() && session.state().pendingCharStyle->bold == true,
          "Pending bold style must be preserved across container jump");

    // 4. 测试垂直跨容器导航 (Up / Down) 与 desiredCaretX 保持
    // 此时光标在 initialId 末尾，按 Down
    sendKey(input, Qt::Key_Down);
    check(session.state().caret.containerId == *addedId, "Caret should cross down to second container");
    check(session.state().desiredCaretX.has_value(), "desiredCaretX should be preserved");

    // 5. 测试添加分组与子容器
    const auto groupId = session.addGroupContainer({});
    check(groupId.has_value(), "Failed to add group");
    check(session.containerSelection().isSelected(*groupId), "New group should be selected");

    const auto childTextId = session.addTextContainer(*groupId, QStringLiteral("Child"));
    check(childTextId.has_value(), "Failed to add child text in group");
    check(session.state().caret.containerId == *childTextId, "Caret should focus child container");

    // 6. 测试撤销 / 重做 (Undo / Redo)
    check(session.canUndo(), "Expected canUndo");
    // 撤销添加 childText
    sendKey(input, Qt::Key_Z, Qt::ControlModifier);
    check(session.canRedo(), "Expected canRedo after undo");
    // 重做添加 childText
    sendKey(input, Qt::Key_Y, Qt::ControlModifier);
    check(session.state().caret.containerId == *childTextId, "Redo should restore child text container");
}

void testPhase3Interactions(MainWindow& window, app::EditorSession& session, QWidget& input)
{
    auto* canvas = window.findChild<CanvasWidget*>();
    check(canvas != nullptr, "Missing canvas widget");

    // 1. 测试吸附对齐引擎 (SnapEngine)
    check(session.newDocument(), "Failed to create new document");
    const auto& contentRect = session.layout().contentRect;
    const auto containerId = session.snapshot().root.children.front().id;

    // 构造一个距离左页边距 2px 的测试矩形，阈值为 6px
    QRectF testRect(contentRect.left() + 2.0, 100, 200, 150);
    auto snapResult = edit::SnapEngine::snapMove(testRect, session.layout(), containerId, 6.0);
    check(snapResult.snappedX, "Expected snap on X axis");
    check(qAbs(snapResult.snappedTopLeft.x() - contentRect.left()) < 0.001, "Expected snap to left page content margin");
    check(!snapResult.guidelines.empty(), "Expected guidelines generated by SnapEngine");
    check(snapResult.guidelines.front().orientation == edit::GuidelineOrientation::Vertical,
          "Expected vertical guideline on X snap");

    // 2. 测试拖拽过程中的实时排版反馈 (Live Layout) 且不修改真实文档
    const auto revBefore = session.snapshot().revision;
    data::ContainerGeometry testGeo;
    testGeo.position = QPointF(50, 50);
    testGeo.width = 400;
    session.updateLiveDragGeometry(containerId, testGeo);
    check(session.isDragging(), "Expected isDragging during live drag");
    check(session.snapshot().revision == revBefore, "Document revision must NOT change during live drag");
    // 验证 layout 实时反映了候选几何
    const auto liveBox = session.layout().containers.front();
    check(liveBox.rect.left() == contentRect.left() + 50, "Live layout must reflect candidate position");

    session.clearLiveDrag();
    check(!session.isDragging(), "Live drag cleared");
    check(session.snapshot().revision == revBefore, "Revision still unchanged after clearing live drag");

    // 3. 验证正文顶层容器不可拉宽，而分组内子文本容器可通过右手柄拖拽改宽
    const auto topTextBox = session.layout().containers.front();
    session.selectContainer(containerId);
    const QPointF topRightEdgeWidget = canvas->paperToWidget(QPointF(topTextBox.rect.right(), topTextBox.rect.center().y()));
    const QPointF topDraggedRightWidget = canvas->paperToWidget(QPointF(topTextBox.rect.right() - 80, topTextBox.rect.center().y()));
    sendMouse(input, QEvent::MouseButtonPress, topRightEdgeWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, topDraggedRightWidget, Qt::NoButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseButtonRelease, topDraggedRightWidget, Qt::LeftButton, Qt::NoButton);
    check(session.snapshot().root.children.front().geometry.width == 0,
          "Top-level text container width must remain unchanged on edge drag");

    const auto resizeGroupId = session.addGroupContainer({});
    check(resizeGroupId.has_value(), "Failed to add group for resize test");
    const auto resizeChildId = session.addTextContainer(*resizeGroupId, QStringLiteral("Group Child Text"));
    check(resizeChildId.has_value(), "Failed to add child text in group for resize test");
    data::ContainerLayout childBoxBefore;
    for (const auto& b : session.layout().containers) {
        if (b.containerId == *resizeChildId) {
            childBoxBefore = b;
            break;
        }
    }
    const auto revBeforeChildResize = session.snapshot().revision;
    session.selectContainer(*resizeChildId);
    const QPointF rightHandleCenter(childBoxBefore.rect.right(), childBoxBefore.rect.center().y());
    const QPointF rightHandleWidget = canvas->paperToWidget(rightHandleCenter);
    const QPointF draggedWidget = canvas->paperToWidget(rightHandleCenter - QPointF(80, 0));

    // 按下手柄开始拖拽缩窄
    sendMouse(input, QEvent::MouseButtonPress, rightHandleWidget, Qt::LeftButton, Qt::LeftButton);
    // 移动手柄
    sendMouse(input, QEvent::MouseMove, draggedWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Session should be in live dragging state during handle drag");
    // 松开手柄提交
    sendMouse(input, QEvent::MouseButtonRelease, draggedWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Dragging state finished after mouse release");
    check(session.snapshot().revision > revBeforeChildResize, "Revision must advance after committed drag");

    // 4. 测试撤销手柄调整
    check(session.canUndo(), "Expected canUndo after resize");
    sendKey(input, Qt::Key_Z, Qt::ControlModifier);
    check(session.canRedo(), "Expected canRedo after undoing resize");

    // 5. 测试浮动图片鼠标拖拽与实时绕排重排
    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("02")).document),
          "Failed to load case 02 with floating image");
    check(!session.layout().images.empty(), "Expected floating image in case 02");
    const auto imageBoxBefore = session.layout().images.front().rect;
    const auto imageId = session.layout().images.front().containerId;

    // 鼠标点击图片中心开始拖拽
    const QPointF imgCenter = imageBoxBefore.center();
    const QPointF imgCenterWidget = canvas->paperToWidget(imgCenter);
    const QPointF imgDraggedWidget = canvas->paperToWidget(imgCenter + QPointF(50, 40));

    sendMouse(input, QEvent::MouseButtonPress, imgCenterWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, imgDraggedWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Session must be in live drag state during image drag");
    // 验证排版中图片的实时位置已经移动
    const auto imageBoxDuring = session.layout().images.front().rect;
    check(imageBoxDuring.left() != imageBoxBefore.left(), "Image rect must update in live layout during drag");
    check(!session.layout().textFragments.empty(), "Text fragments should be present");

    // 松开鼠标提交
    sendMouse(input, QEvent::MouseButtonRelease, imgDraggedWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Live drag finished after mouse release");
    const auto imageBoxAfter = session.layout().images.front().rect;
    check(imageBoxAfter.left() == imageBoxDuring.left(), "Committed image rect should match dropped position");

    // 6. 测试容器上下移动与同级重排 (Container Vertical Movement & Sibling Reordering)
    check(session.newDocument(), "Failed to create new doc for reordering test");
    const auto topId = session.snapshot().root.children.front().id;
    const auto bottomId = session.addTextContainer({}, QStringLiteral("Bottom Container"));
    check(bottomId.has_value(), "Failed to add second container");
    check(session.snapshot().root.children.size() == 2, "Expected 2 containers");
    check(session.snapshot().root.children.front().id == topId, "Initial order: top first");
    check(session.snapshot().root.children.back().id == *bottomId, "Initial order: bottom second");

    const auto topBoxBefore = session.layout().containers.front();
    const auto bottomBoxBefore = session.layout().containers.back();
    check(bottomBoxBefore.rect.top() >= topBoxBefore.rect.bottom(), "Bottom box must initially be below top box");

    // 点击 bottomBox 边框并向上拖拽超过 topBox
    const QPointF bottomBorderPos(bottomBoxBefore.rect.left() + 1, bottomBoxBefore.rect.center().y());
    const QPointF bottomBorderWidget = canvas->paperToWidget(bottomBorderPos);
    // 拖动到 topBox 上方
    const QPointF targetUpPos(bottomBoxBefore.rect.left() + 1, topBoxBefore.rect.top() - 20);
    const QPointF targetUpWidget = canvas->paperToWidget(targetUpPos);

    sendMouse(input, QEvent::MouseButtonPress, bottomBorderWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, targetUpWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Expected live drag while dragging container up");
    // 验证实时排版中顺序已颠倒：bottomId 排在第一位
    check(session.layout().containers.front().containerId == *bottomId, "Live layout must show bottom container moved to top");
    // 松手提交
    sendMouse(input, QEvent::MouseButtonRelease, targetUpWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Drag finished");
    check(session.snapshot().root.children.front().id == *bottomId, "Document must have bottom container first now");
    check(session.snapshot().root.children.back().id == topId, "Document must have top container second now");

    // 6.2 测试容器向下移动 (Container Moving Down past sibling)
    const auto topNowBox = session.layout().containers.front();
    const auto bottomNowBox = session.layout().containers.back();
    // 点击顶部边框（避开左右中心的手柄）
    const QPointF topBorderPos(topNowBox.rect.center().x(), topNowBox.rect.top() + 1);
    const QPointF topBorderWidget = canvas->paperToWidget(topBorderPos);
    // 拖动到 bottomNowBox 下方
    const QPointF targetDownPos(topNowBox.rect.center().x(), bottomNowBox.rect.bottom() + 40);
    const QPointF targetDownWidget = canvas->paperToWidget(targetDownPos);

    sendMouse(input, QEvent::MouseButtonPress, topBorderWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, targetDownWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Expected live drag while dragging container down");
    // 验证实时排版中 topId 重新排到第一位
    check(session.layout().containers.front().containerId == topId, "Live layout must show container moved down to bottom");
    // 松手提交
    sendMouse(input, QEvent::MouseButtonRelease, targetDownWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Drag finished");
    check(session.snapshot().root.children.front().id == topId, "Document must have topId first again");
    check(session.snapshot().root.children.back().id == *bottomId, "Document must have bottomId second again");

    // 7. 测试跨容器迁移图片 (Move Floating Image across Containers)
    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("02")).document),
          "Failed to load case 02 with floating image");
    data::ContainerId srcTextId;
    data::ContainerId imgNodeId;
    for (const auto& c : session.snapshot().root.children) {
        if (!c.children.empty()) {
            srcTextId = c.id;
            imgNodeId = c.children.front().id;
            break;
        }
    }
    check(!srcTextId.isEmpty() && !imgNodeId.isEmpty(), "Found source text and image in case 02");
    // 添加第二个文本容器
    const auto dstTextId = session.addTextContainer({}, QStringLiteral("Destination container with text for wrapping..."));
    check(dstTextId.has_value(), "Failed to add destination container");

    // 获取图片初始位置和目标容器位置
    const auto imgBox = session.layout().images.front().rect;
    QRectF dstContentRect;
    for (const auto& b : session.layout().containers) {
        if (b.containerId == *dstTextId) {
            dstContentRect = b.contentRect;
            break;
        }
    }
    check(dstContentRect.isValid(), "Destination container content rect must be valid");

    // 点击图片并拖拽到目标容器的 contentRect 内部
    const QPointF imgPressPos = imgBox.center();
    const QPointF imgPressWidget = canvas->paperToWidget(imgPressPos);
    const QPointF imgTargetPos = dstContentRect.topLeft() + QPointF(20, 20);
    const QPointF imgTargetWidget = canvas->paperToWidget(imgTargetPos);

    sendMouse(input, QEvent::MouseButtonPress, imgPressWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, imgTargetWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Session must be dragging image across containers");

    // 松手提交跨容器迁移
    sendMouse(input, QEvent::MouseButtonRelease, imgTargetWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Image cross-container drag finished");

    const auto findTestNode = [](auto& self, const data::ContainerNode& root, const data::ContainerId& id) -> const data::ContainerNode* {
        if (root.id == id)
            return &root;
        for (const auto& child : root.children) {
            if (const auto* found = self(self, child, id))
                return found;
        }
        return nullptr;
    };

    // 验证快照中文档节点关系：源文本容器不再拥有该图片，目标文本容器拥有该图片
    const auto* srcNodeAfter = findTestNode(findTestNode, session.snapshot().root, srcTextId);
    const auto* dstNodeAfter = findTestNode(findTestNode, session.snapshot().root, *dstTextId);
    check(srcNodeAfter != nullptr && srcNodeAfter->children.empty(), "Source container must not have image anymore");
    check(dstNodeAfter != nullptr && !dstNodeAfter->children.empty(), "Destination container must now have image");
    check(dstNodeAfter->children.front().id == imgNodeId, "Moved image ID must match");

    // 测试撤销：图片应该回到源文本容器
    check(session.canUndo(), "Expected canUndo after cross container image move");
    session.undo();
    const auto* srcNodeUndo = findTestNode(findTestNode, session.snapshot().root, srcTextId);
    const auto* dstNodeUndo = findTestNode(findTestNode, session.snapshot().root, *dstTextId);
    check(srcNodeUndo != nullptr && !srcNodeUndo->children.empty(), "Source container must have image back after undo");
    check(dstNodeUndo != nullptr && dstNodeUndo->children.empty(), "Destination container must be empty of images after undo");

    // 8. 测试浮动图片右下角手柄等比例缩放、实时绕排重排与 Undo/Redo
    check(session.loadSnapshot(editor::testing::makeLayoutCase(QStringLiteral("02")).document),
          "Failed to load case 02 for image resize test");
    data::ContainerId resizeImgId;
    QRectF imgFrameBefore;
    for (const auto& img : session.layout().images) {
        resizeImgId = img.containerId;
        for (const auto& box : session.layout().containers) {
            if (box.containerId == resizeImgId) {
                imgFrameBefore = box.rect;
                break;
            }
        }
        break;
    }
    check(!resizeImgId.isEmpty() && imgFrameBefore.isValid(), "Expected valid image frame before resize");
    const auto* imgNodeBefore = findTestNode(findTestNode, session.snapshot().root, resizeImgId);
    check(imgNodeBefore != nullptr, "Missing image node before resize");
    const QSizeF imgDispBefore = imgNodeBefore->image.displaySize;
    const auto revImgBefore = session.snapshot().revision;

    // 单击选中浮动图片以激活 8 手柄
    clickAt(input, canvas->paperToWidget(imgFrameBefore.center()));
    check(session.containerSelection().isSelected(resizeImgId), "Image must be selected after click");

    // 按住右下角手柄拖拽放大 (+40, +20) -> 保持等比例
    const QPointF brHandleWidget = canvas->paperToWidget(imgFrameBefore.bottomRight());
    const QPointF brDraggedWidget = canvas->paperToWidget(imgFrameBefore.bottomRight() + QPointF(40, 20));
    sendMouse(input, QEvent::MouseButtonPress, brHandleWidget, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, brDraggedWidget, Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Session must enter live drag during corner image resize");

    const QRectF liveImgRect = session.layout().images.front().rect;
    check(liveImgRect.width() > imgDispBefore.width() + 20.0 && liveImgRect.height() > imgDispBefore.height() + 10.0,
          "Live image layout must reflect enlarged display size");
    const qreal origRatio = imgDispBefore.width() / imgDispBefore.height();
    const qreal liveRatio = liveImgRect.width() / liveImgRect.height();
    check(qAbs(origRatio - liveRatio) < 0.05, "Corner resize without Shift must preserve aspect ratio");
    for (const auto& frag : session.layout().textFragments) {
        if (frag.containerId == resizeImgId)
            continue;
        // 文本片段不得与放大后的图片框重叠
        check(!frag.rect.intersects(liveImgRect), "Reflowed text fragments must avoid enlarged live image");
    }

    sendMouse(input, QEvent::MouseButtonRelease, brDraggedWidget, Qt::LeftButton, Qt::NoButton);
    check(!session.isDragging(), "Live drag must stop after image resize release");
    check(session.snapshot().revision > revImgBefore, "Document revision must increment on image resize commit");
    const QSizeF imgDispAfter = findTestNode(findTestNode, session.snapshot().root, resizeImgId)->image.displaySize;
    check(qAbs(imgDispAfter.width() - liveImgRect.width()) < 0.5 &&
          qAbs(imgDispAfter.height() - liveImgRect.height()) < 0.5,
          "Committed displaySize must match live preview dimensions");

    // 撤销与重做验证
    check(session.undo(), "Undo after image resize must succeed");
    const QSizeF imgDispUndo = findTestNode(findTestNode, session.snapshot().root, resizeImgId)->image.displaySize;
    check(qAbs(imgDispUndo.width() - imgDispBefore.width()) < 0.5 &&
          qAbs(imgDispUndo.height() - imgDispBefore.height()) < 0.5,
          "Undo must restore original image displaySize");
    check(session.redo(), "Redo after image resize must succeed");
    const QSizeF imgDispRedo = findTestNode(findTestNode, session.snapshot().root, resizeImgId)->image.displaySize;
    check(qAbs(imgDispRedo.width() - imgDispAfter.width()) < 0.5,
          "Redo must reapply resized image displaySize");

    // 9. 测试图片右中点边手柄单轴缩窄（高度保持不变）
    QRectF imgFrameAfterResize;
    for (const auto& box : session.layout().containers) {
        if (box.containerId == resizeImgId) {
            imgFrameAfterResize = box.rect;
            break;
        }
    }
    // redo() 会清空容器选中状态，先重新单击选中图片以显示手柄
    clickAt(input, canvas->paperToWidget(imgFrameAfterResize.center()));
    check(session.containerSelection().isSelected(resizeImgId), "Image must be selected before edge handle resize");
    const QSizeF sizeBeforeEdge = findTestNode(findTestNode, session.snapshot().root, resizeImgId)->image.displaySize;
    const QPointF rightEdgeHandle = canvas->paperToWidget(QPointF(imgFrameAfterResize.right(), imgFrameAfterResize.center().y()));
    const QPointF rightEdgeDragged = canvas->paperToWidget(QPointF(imgFrameAfterResize.right() - 30.0, imgFrameAfterResize.center().y()));
    sendMouse(input, QEvent::MouseButtonPress, rightEdgeHandle, Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, rightEdgeDragged, Qt::NoButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseButtonRelease, rightEdgeDragged, Qt::LeftButton, Qt::NoButton);
    const QSizeF sizeAfterEdge = findTestNode(findTestNode, session.snapshot().root, resizeImgId)->image.displaySize;
    check(sizeAfterEdge.width() < sizeBeforeEdge.width() - 20.0 &&
          qAbs(sizeAfterEdge.height() - sizeBeforeEdge.height()) < 0.5,
          "Right edge handle resize must change width while keeping height constant");

    // 10. 测试顶层流式图片 (Flow Image) 缩放
    data::DocumentSnapshot flowDoc;
    flowDoc.page.size = QSizeF(760, 1000);
    flowDoc.page.margins = QMarginsF(44, 44, 44, 44);
    QImage sampleImg(160, 100, QImage::Format_RGB32);
    sampleImg.fill(Qt::darkBlue);
    flowDoc.resources.insert(QStringLiteral("flow-res"), sampleImg);
    data::ContainerNode flowImgNode;
    flowImgNode.id = QStringLiteral("flow-img-1");
    flowImgNode.type = data::ContainerType::Image;
    flowImgNode.placement = data::PlacementMode::Flow;
    flowImgNode.image = {QStringLiteral("flow-res"), QSizeF(160, 100)};
    data::ContainerNode flowTextNode;
    flowTextNode.id = QStringLiteral("flow-text-1");
    flowTextNode.type = data::ContainerType::Text;
    flowTextNode.placement = data::PlacementMode::Flow;
    flowTextNode.text.text = QStringLiteral("Below flow image");
    flowTextNode.text.paragraphs = {{{0, 16}, data::ParagraphRole::Body, 1, {}, {}}};
    flowDoc.root.children = {flowImgNode, flowTextNode};
    check(session.loadSnapshot(flowDoc), "Failed to load flow image snapshot");

    const QRectF flowBoxBefore = session.layout().containers.front().rect;
    clickAt(input, canvas->paperToWidget(flowBoxBefore.center()));
    check(session.containerSelection().isSelected(QStringLiteral("flow-img-1")), "Flow image must be selectable");
    sendMouse(input, QEvent::MouseButtonPress, canvas->paperToWidget(flowBoxBefore.bottomRight()), Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, canvas->paperToWidget(flowBoxBefore.bottomRight() + QPointF(48, 30)), Qt::NoButton, Qt::LeftButton);
    check(session.isDragging(), "Flow image resize must trigger live drag");
    sendMouse(input, QEvent::MouseButtonRelease, canvas->paperToWidget(flowBoxBefore.bottomRight() + QPointF(48, 30)), Qt::LeftButton, Qt::NoButton);
    const QSizeF flowSizeAfter = findTestNode(findTestNode, session.snapshot().root, QStringLiteral("flow-img-1"))->image.displaySize;
    check(flowSizeAfter.width() > 200.0 && flowSizeAfter.height() > 120.0,
          "Flow image displaySize must grow on corner resize commit");

    // 11. 测试分组内子文本容器左手柄调整宽度并保持右边距锚定
    check(session.newDocument(), "Failed to reset document for left handle test");
    const auto leftResizeGroupId = session.addGroupContainer({});
    check(leftResizeGroupId.has_value(), "Failed to add group for left handle test");
    const auto narrowId = session.addTextContainer(*leftResizeGroupId, QStringLiteral("Narrow child text"));
    check(narrowId.has_value(), "Failed to add child text for left handle test");
    data::ContainerGeometry narrowGeo;
    narrowGeo.position = QPointF(120, 0);
    narrowGeo.width = 300;
    check(session.commitContainerGeometry(*narrowId, narrowGeo), "Failed to set narrow container geometry");
    session.selectContainer(*narrowId);
    data::ContainerLayout narrowBoxBeforeLayout;
    for (const auto& b : session.layout().containers) {
        if (b.containerId == *narrowId) {
            narrowBoxBeforeLayout = b;
            break;
        }
    }
    const QRectF narrowBoxBefore = narrowBoxBeforeLayout.rect;
    const QPointF leftHandlePos(narrowBoxBefore.left(), narrowBoxBefore.center().y());
    const QPointF leftDraggedPos = leftHandlePos - QPointF(40, 0);
    sendMouse(input, QEvent::MouseButtonPress, canvas->paperToWidget(leftHandlePos), Qt::LeftButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseMove, canvas->paperToWidget(leftDraggedPos), Qt::NoButton, Qt::LeftButton);
    sendMouse(input, QEvent::MouseButtonRelease, canvas->paperToWidget(leftDraggedPos), Qt::LeftButton, Qt::NoButton);
    QRectF narrowBoxAfter;
    for (const auto& b : session.layout().containers) {
        if (b.containerId == *narrowId) {
            narrowBoxAfter = b.rect;
            break;
        }
    }
    check(qAbs(narrowBoxAfter.right() - narrowBoxBefore.right()) < 1.0 &&
          narrowBoxAfter.width() > 335.0 &&
          narrowBoxAfter.left() < narrowBoxBefore.left() - 35.0,
          "Left handle drag on child text container must expand width leftward while keeping right edge anchored");
}

} // namespace

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    MainWindow window;
    app::EditorIntegration integration(window);
    window.show();
    QApplication::processEvents();
    auto* input = window.findChild<QWidget*>(QStringLiteral("editorInput"));
    check(input != nullptr, "Missing the input widget");

    const QTemporaryDir output(QApplication::applicationDirPath() + QStringLiteral("/editor-test-XXXXXX"));
    check(output.isValid(), "Could not create a temporary directory for DOCX files");
    auto& session = integration.session();
    testParagraphLayout(window, session, *input);
    testWrapUsesCurrentRowHeight(session);
    testEditing(window, session, *input, output);
    testFontSelection(window, session, *input);
    testIme(session, *input);
    testRejectedBatch();
    testInsertImages(window, session, *input, output);
    testRealLayouts(window, session, *input, output);
    testContainerSelectionAndDeletion(window, session, *input);
    testPhase2Interactions(window, session, *input);
    testPhase3Interactions(window, session, *input);
    check(session.newDocument() &&
          session.exportTo(output.filePath(QStringLiteral("missing/failure.docx"))).status == io::DocxExportStatus::IoError,
          "An invalid export path must return an IO error");
    qInfo("Editor integration passed");
}
