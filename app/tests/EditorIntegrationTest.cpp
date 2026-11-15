#include "CanvasWidget.h"
#include "EditorIntegration.h"
#include "MainWindow.h"
#include "TestFixtures.h"
#include "TextBoundary.h"
#include "ZipWriter.h"

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
        sendMouse(input, QEvent::MouseButtonPress, startPoint, Qt::LeftButton, Qt::LeftButton);
        sendMouse(input, QEvent::MouseMove, endPoint, Qt::NoButton, Qt::LeftButton);
        sendMouse(input, QEvent::MouseButtonRelease, endPoint, Qt::LeftButton, Qt::NoButton);
        check(session.state().caret.containerId == QStringLiteral("body") &&
              session.state().selection && session.state().selection->rects.size() >= 2,
              "Dragging across the image did not select one source text range");
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
    testEditing(window, session, *input, output);
    testIme(session, *input);
    testRejectedBatch();
    testInsertImages(window, session, *input, output);
    testRealLayouts(window, session, *input, output);
    check(session.newDocument() &&
          session.exportTo(output.filePath(QStringLiteral("missing/failure.docx"))).status == io::DocxExportStatus::IoError,
          "An invalid export path must return an IO error");
    qInfo("Editor integration passed");
}
