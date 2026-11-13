// 第六刀自测：真实窗口事件 -> 交互归约 -> 本地执行 -> 重新投影 -> DOCX 导出。
// 在离屏平台 (offscreen) 下构造 EditorWindow，用 QCoreApplication::sendEvent
// 投递真实的 QMouseEvent / QKeyEvent / QInputMethodEvent，全程不弹任何窗口。
// 输出全部 ASCII，避免 Qt Creator 控制台乱码。

#include "DocumentCanvas.h"
#include "EditorWindow.h"
#include "ZipWriter.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QString>
#include <QtGlobal>
#include <QtGui/qevent.h>

#include <cstdio>
#include <optional>

namespace data = editor::data;

using editor::demo::DocumentCanvas;
using editor::demo::EditorWindow;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const QString& what)
{
    if (ok) {
        ++g_passed;
        std::printf("[PASS] %s\n", what.toUtf8().constData());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.toUtf8().constData());
    }
}

const data::ContainerNode* findTextNode(const data::ContainerNode& node,
                                        const data::ContainerId& id)
{
    if (node.type == data::ContainerType::Text && node.id == id)
        return &node;
    for (const auto& child : node.children) {
        if (const data::ContainerNode* found = findTextNode(child, id))
            return found;
    }
    return nullptr;
}

QString bodyText(const data::DocumentSnapshot& doc, const data::ContainerId& id)
{
    const data::ContainerNode* node = findTextNode(doc.root, id);
    return node ? node->text.text : QString();
}

int paragraphCount(const data::DocumentSnapshot& doc, const data::ContainerId& id)
{
    const data::ContainerNode* node = findTextNode(doc.root, id);
    return node ? static_cast<int>(node->text.paragraphs.size()) : 0;
}

bool hasBoldSpan(const data::DocumentSnapshot& doc, const data::ContainerId& id)
{
    const data::ContainerNode* node = findTextNode(doc.root, id);
    if (!node) return false;
    for (const auto& span : node->text.formatSpans)
        if (span.style.bold && *span.style.bold) return true;
    return false;
}

bool hasHeading2(const data::DocumentSnapshot& doc, const data::ContainerId& id)
{
    const data::ContainerNode* node = findTextNode(doc.root, id);
    if (!node) return false;
    for (const auto& p : node->text.paragraphs)
        if (p.role == data::ParagraphRole::Heading && p.headingLevel == 2) return true;
    return false;
}

QString partText(const QByteArray& package, const QString& name)
{
    QByteArray bytes;
    return editor::io::zip::extractEntry(package, name, &bytes, nullptr)
               ? QString::fromUtf8(bytes) : QString();
}

QByteArray readFileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

data::ContainerId activeId(const DocumentCanvas* canvas)
{
    return canvas->interactionState().caret.containerId;
}

const data::TextFragment* firstFragment(const DocumentCanvas* canvas)
{
    const data::ContainerId id = activeId(canvas);
    for (const auto& f : canvas->layout().textFragments) {
        if (f.containerId == id)
            return &f;
    }
    return nullptr;
}

void sendKey(DocumentCanvas* canvas, int key, Qt::KeyboardModifiers mods,
             const QString& text = QString())
{
    QKeyEvent event(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(canvas, &event);
}

void sendMouse(DocumentCanvas* canvas, QEvent::Type type, const QPointF& local,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, local, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &event);
}

void sendPreedit(DocumentCanvas* canvas, const QString& text)
{
    QInputMethodEvent event(text, QList<QInputMethodEvent::Attribute>());
    QCoreApplication::sendEvent(canvas, &event);
}

void sendCommit(DocumentCanvas* canvas, const QString& text)
{
    QInputMethodEvent event;
    event.setCommitString(text);
    QCoreApplication::sendEvent(canvas, &event);
}

// 把画布真实重绘到 PNG，人工核对字体颜色、高亮和拖拽反馈。
void renderPreview(DocumentCanvas* canvas, const QString& path)
{
    QImage image(canvas->size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    canvas->render(&painter);
    painter.end();
    image.save(path);
}

// 回归检查：文字区域必须存在深色像素。QTextLayout::draw 会继承 QPainter 的
// 当前 pen，之前容器框留下浅蓝点线、选区留下 NoPen，导致正文看不见。
bool hasDarkTextPixels(const QString& path)
{
    const QImage image(path);
    if (image.isNull())
        return false;
    int dark = 0;
    const int maxY = qMin(240, image.height());
    const int maxX = qMin(700, image.width());
    for (int y = 40; y < maxY; ++y) {
        for (int x = 40; x < maxX; ++x) {
            const QRgb c = image.pixel(x, y);
            if (qRed(c) < 90 && qGreen(c) < 90 && qBlue(c) < 90)
                ++dark;
        }
    }
    return dark > 150;
}

} // namespace

int main(int argc, char** argv)
{
    // 离屏平台：不需要显示器，也不弹任何窗口。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::printf("===== UI self-check: real events -> DOCX =====\n");

    const QString outDir = QCoreApplication::applicationDirPath()
                               + QStringLiteral("/ui-export");
    QDir().mkpath(outDir);

    EditorWindow window;
    DocumentCanvas* canvas = window.canvas();
    const data::ContainerId id = activeId(canvas);

    check(canvas != nullptr, "window exposes a live canvas");
    check(canvas->document().revision == 101,
          "case 01 fixture loads with its fixed revision");
    check(canvas->layout().documentRevision == canvas->document().revision,
          "projected layout matches snapshot revision");
    check(!canvas->layout().textFragments.empty(), "canvas has text fragments");
    check(canvas->width() >= 760 && canvas->height() >= 1000,
          "canvas sized to the paper");
    check(!canvas->interactionState().caret.rect.isEmpty(), "caret rect valid");
    check(id == QStringLiteral("title"),
          "case 01 caret starts in the first text container");

    renderPreview(canvas, outDir + QStringLiteral("/canvas-initial.png"));
    check(QFile::exists(outDir + QStringLiteral("/canvas-initial.png")),
          "wrote initial canvas preview for visual review");
    check(hasDarkTextPixels(outDir + QStringLiteral("/canvas-initial.png")),
          "body text uses a dark pen and stays readable");

    // ---------- 真实鼠标按下 + Ctrl+V 从系统剪贴板插入 ----------
    {
        const data::TextFragment* first = firstFragment(canvas);
        check(first != nullptr, "first text fragment found for mouse test");
        if (first) {
            const QPointF point(first->rect.left() + 30.0, first->rect.center().y());
            sendMouse(canvas, QEvent::MouseButtonPress, point,
                      Qt::LeftButton, Qt::LeftButton);
            check(canvas->interactionState().caret.offset > 0,
                  "mouse press hit-test moves the caret");

            if (QClipboard* clipboard = QGuiApplication::clipboard())
                clipboard->setText(QStringLiteral("UiClip"));
            sendKey(canvas, Qt::Key_V, Qt::ControlModifier);
            check(bodyText(canvas->document(), id).contains(QStringLiteral("UiClip")),
                  "Ctrl+V inserts clipboard text");
        }
    }

    // ---------- 真实拖选 + Ctrl+C ----------
    {
        const data::TextFragment* first = firstFragment(canvas);
        check(first != nullptr, "fresh fragment after clipboard insert");
        if (first) {
            const QPointF from(first->rect.left() + 30.0, first->rect.center().y());
            const QPointF to(first->rect.left() + 150.0, first->rect.center().y());
            sendMouse(canvas, QEvent::MouseButtonPress, from,
                      Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
            const auto& selection = canvas->interactionState().selection;
            check(selection.has_value() && !selection->rects.empty(),
                  "drag paints selection rects");
            sendMouse(canvas, QEvent::MouseButtonRelease, to,
                      Qt::LeftButton, Qt::NoButton);
            check(canvas->interactionState().selection.has_value(),
                  "release keeps the finished selection");
            renderPreview(canvas, outDir + QStringLiteral("/canvas-drag.png"));
            check(QFile::exists(outDir + QStringLiteral("/canvas-drag.png")),
                  "wrote drag canvas preview for visual review");
            check(hasDarkTextPixels(outDir + QStringLiteral("/canvas-drag.png")),
                  "body text stays visible while a drag selection is active");

            sendKey(canvas, Qt::Key_C, Qt::ControlModifier);
            check(!canvas->clipboardText().isEmpty(),
                  "Ctrl+C copies selected text");
        }
    }

    // ---------- 粘贴替换 + 真实输入法事件 ----------
    {
        canvas->pasteText(QStringLiteral("UiPaste"));
        check(bodyText(canvas->document(), id).contains(QStringLiteral("UiPaste")),
              "paste replaces the selection");

        const quint64 beforePaste = canvas->document().revision;
        sendPreedit(canvas, QStringLiteral("UiPreedit"));
        check(canvas->interactionState().preedit.text == QStringLiteral("UiPreedit"),
              "IME preedit lands in UI state");
        check(!bodyText(canvas->document(), id).contains(QStringLiteral("UiPreedit")),
              "preedit never enters the document");
        sendCommit(canvas, QStringLiteral("UiIme"));
        check(bodyText(canvas->document(), id).count(QStringLiteral("UiIme")) == 1,
              "IME commit inserts exactly once");
        check(canvas->interactionState().preedit.text.isEmpty(),
              "preedit cleared after commit");
        check(canvas->document().revision > beforePaste,
              "revision advanced across the edit chain");
    }

    // ---------- 真实键盘：字符、回车、全选、加粗 ----------
    int paragraphsBefore = 0;
    {
        sendKey(canvas, Qt::Key_Z, Qt::NoModifier, QStringLiteral("Z"));
        check(bodyText(canvas->document(), id).contains(QStringLiteral("Z")),
              "typed character applied");

        paragraphsBefore = paragraphCount(canvas->document(), id);
        sendKey(canvas, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        check(paragraphCount(canvas->document(), id) == paragraphsBefore + 1,
              "Enter splits the paragraph");

        sendKey(canvas, Qt::Key_A, Qt::ControlModifier);
        const auto& selection = canvas->interactionState().selection;
        check(selection.has_value()
                  && selection->range.end - selection->range.start
                         == bodyText(canvas->document(), id).size(),
              "Ctrl+A selects the whole container");

        sendKey(canvas, Qt::Key_B, Qt::ControlModifier);
        check(hasBoldSpan(canvas->document(), id), "Ctrl+B bolds the selection");
    }

    // ---------- 工具栏动作真的被触发 ----------
    {
        QAction* heading2 = window.action(QStringLiteral("actionHeading2"));
        check(heading2 != nullptr, "toolbar exposes the H2 action");
        if (heading2) {
            heading2->trigger();
            check(hasHeading2(canvas->document(), id),
                  "toolbar H2 action lands in the snapshot");
        }

        QAction* selectAllAction = window.action(QStringLiteral("actionSelectAll"));
        check(selectAllAction != nullptr, "toolbar exposes select-all");
        check(window.action(QStringLiteral("actionExport")) != nullptr,
              "toolbar exposes the export action");
    }

    check(canvas->appliedRequests() >= 6, "edit requests counted across the loop");
    check(canvas->layout().documentRevision == canvas->document().revision,
          "layout stays in sync after edits");

    // ---------- 导出并核对包内容 ----------
    {
        const QString path = outDir + QStringLiteral("/ui-edited-01.docx");
        QString error;
        check(window.exportToFile(path, &error) && QFile::exists(path),
              "exported ui-edited-01.docx from the live window");

        const QByteArray package = readFileBytes(path);
        check(editor::io::zip::verifyArchive(package).isEmpty(),
              "exported package is a valid ZIP");
        const QString xml = partText(package, QStringLiteral("word/document.xml"));
        check(xml.contains(QStringLiteral("UiPaste"))
                  && xml.contains(QStringLiteral("UiIme"))
                  && xml.contains(QStringLiteral("Z")),
              "pasted, committed and typed text exported");
        check(!xml.contains(QStringLiteral("UiPreedit")),
              "preedit text absent from DOCX");
        check(xml.contains(QStringLiteral("<w:b/>")), "bold run exported");
        check(xml.contains(QStringLiteral("Heading2")), "Heading2 style exported");
    }

    // ---------- 换绕排图片样例：画布与导出链都成立 ----------
    {
        window.loadCase(QStringLiteral("02"));
        const data::ContainerId imageCaseId = activeId(canvas);
        check(!canvas->layout().images.empty(), "case 02 loads floating images");
        check(!bodyText(canvas->document(), imageCaseId).isEmpty(),
              "case 02 keeps editable body text");

        const QString path = outDir + QStringLiteral("/ui-edited-02.docx");
        QString error;
        check(window.exportToFile(path, &error) && QFile::exists(path),
              "exported ui-edited-02.docx from the live window");

        const QByteArray package = readFileBytes(path);
        check(editor::io::zip::verifyArchive(package).isEmpty(),
              "case 02 package is a valid ZIP");
        const QString xml = partText(package, QStringLiteral("word/document.xml"));
        check(xml.contains(QStringLiteral("<wp:anchor"))
                  && xml.contains(QStringLiteral("wrapSquare")),
              "case 02 keeps floating anchor and square wrap");
        QByteArray media;
        check(editor::io::zip::extractEntry(
                  package, QStringLiteral("word/media/image1.png"), &media, nullptr)
                  && !QImage::fromData(media).isNull(),
              "case 02 image media present and decodable");
    }

    // ---------- 正文段落里的真实拖选（不只是短标题） ----------
    {
        window.loadCase(QStringLiteral("01"));
        const data::TextFragment* body = nullptr;
        for (const auto& f : canvas->layout().textFragments) {
            if (f.containerId == QStringLiteral("p1")) { body = &f; break; }
        }
        check(body != nullptr, "case 01 has a p1 body fragment");
        if (body) {
            const QPointF from(body->rect.left() + 30.0, body->rect.center().y());
            const QPointF to(body->rect.left() + 220.0, body->rect.center().y());
            sendMouse(canvas, QEvent::MouseButtonPress, from,
                      Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
            const auto& selection = canvas->interactionState().selection;
            check(selection.has_value()
                      && selection->containerId == QStringLiteral("p1")
                      && !selection->rects.empty(),
                  "dragging inside body text paints a p1 selection");
            sendMouse(canvas, QEvent::MouseButtonRelease, to,
                      Qt::LeftButton, Qt::NoButton);
            renderPreview(canvas, outDir + QStringLiteral("/canvas-body-drag.png"));
            check(hasDarkTextPixels(outDir + QStringLiteral("/canvas-body-drag.png")),
                  "body text remains readable during a body drag");
        }
    }

    std::printf("===== UI self-check: %s =====\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    std::printf("[OK] UI self-check %s.\n",
                g_failed == 0 ? "passed" : "failed");
    return g_failed == 0 ? 0 : 1;
}
