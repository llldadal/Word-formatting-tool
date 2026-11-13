// 第五刀：编辑 -> 导出 的完整纵切自测。
//
// 用本地替身顶掉尚未交付的文档/排版模块：
//   InteractionController 产出 EditRequest → LocalDocumentExecutor 落到新快照
//   → FlowGeometry 重新投影 LayoutResult → InteractionController::refresh
//   → DocxExporter 导出 → 校验包结构和编辑结果。
// 全程只用仓库自带的固定输入，输出全部 ASCII，避免控制台乱码。

#include "DocxExporter.h"
#include "FlowGeometry.h"
#include "HitTest.h"
#include "InteractionController.h"
#include "InteractionTypes.h"
#include "LocalDocumentExecutor.h"
#include "Selection.h"
#include "TestFixtures.h"
#include "ZipWriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPointF>
#include <QTemporaryDir>

#include <cstdio>
#include <optional>

// 共享契约位于 editor::data，这里起一个短别名，与其余模块源码写法一致。
namespace data = editor::data;

using data::ContainerId;
using data::ContainerNode;
using data::DocumentSnapshot;
using data::LayoutResult;
using data::TextFragment;
using editor::interaction::EditOp;
using editor::interaction::Hit;
using editor::interaction::InteractionController;
using editor::interaction::InteractionOutput;
using editor::interaction::InteractionState;
using editor::interaction::ImeInput;
using editor::interaction::KeyInput;
using editor::interaction::hitTest;
using editor::interaction::makeSelection;
using editor::interaction::selectedText;
using editor::io::DocxBuildResult;
using editor::io::DocxExportStatus;
using editor::io::buildDocx;
using editor::io::exportDocx;
using editor::slice::applyRequests;
using editor::slice::projectLayout;
using editor::testing::makeLayoutCase;

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

QString partText(const QByteArray& package, const QString& name)
{
    QByteArray bytes;
    return editor::io::zip::extractEntry(package, name, &bytes, nullptr)
               ? QString::fromUtf8(bytes) : QString();
}

const ContainerNode* findTextNode(const ContainerNode& node, const ContainerId& id)
{
    if (node.id == id && node.type == data::ContainerType::Text) return &node;
    for (const auto& child : node.children)
        if (const ContainerNode* found = findTextNode(child, id)) return found;
    return nullptr;
}

QString bodyText(const DocumentSnapshot& doc, const ContainerId& id)
{
    const ContainerNode* node = findTextNode(doc.root, id);
    return node ? node->text.text : QString();
}

int paragraphCount(const DocumentSnapshot& doc, const ContainerId& id)
{
    const ContainerNode* node = findTextNode(doc.root, id);
    return node ? static_cast<int>(node->text.paragraphs.size()) : 0;
}

bool hasBoldSpan(const DocumentSnapshot& doc, const ContainerId& id)
{
    const ContainerNode* node = findTextNode(doc.root, id);
    if (!node) return false;
    for (const auto& span : node->text.formatSpans)
        if (span.style.bold && *span.style.bold) return true;
    return false;
}

bool hasUnderlineSpan(const DocumentSnapshot& doc, const ContainerId& id)
{
    const ContainerNode* node = findTextNode(doc.root, id);
    if (!node) return false;
    for (const auto& span : node->text.formatSpans)
        if (span.style.underline && *span.style.underline) return true;
    return false;
}

bool hasHeading2(const DocumentSnapshot& doc, const ContainerId& id)
{
    const ContainerNode* node = findTextNode(doc.root, id);
    if (!node) return false;
    for (const auto& p : node->text.paragraphs)
        if (p.role == data::ParagraphRole::Heading && p.headingLevel == 2) return true;
    return false;
}

KeyInput typedKey(const QString& text)
{
    KeyInput key;
    key.key = 0;
    key.text = text;
    return key;
}

KeyInput plainKey(int qtKey)
{
    KeyInput key;
    key.key = qtKey;
    return key;
}

KeyInput controlKey(int qtKey)
{
    KeyInput key = plainKey(qtKey);
    key.mods = Qt::ControlModifier;
    return key;
}

// 一次编辑循环的完整状态：文档、排版和控制器始终指向同一版本。
struct Slice {
    DocumentSnapshot doc;
    LayoutResult layout;
    InteractionController controller;
    int appliedRequests = 0;
    QString lastError;

    void init(const DocumentSnapshot& initial, const ContainerId& id, int offset)
    {
        doc = initial;
        layout = projectLayout(doc);
        controller.reset(doc, layout, id, offset);
        appliedRequests = 0;
        lastError.clear();
    }

    bool commit(const InteractionOutput& output)
    {
        if (output.requests.empty()) return true;
        const auto result = applyRequests(doc, output.requests);
        if (!result.ok) {
            lastError = result.error;
            return false;
        }
        doc = result.document;
        layout = projectLayout(doc);
        controller.refresh(layout);
        appliedRequests += static_cast<int>(output.requests.size());
        return true;
    }
};

} // 匿名命名空间结束

int main(int argc, char** argv)
{
    // 几何投影使用 QTextLayout，需要 QGuiApplication 提供字体库。
    QGuiApplication app(argc, argv);
    std::printf("===== Vertical slice self-check: edit -> export =====\n");

    const QString outDir = QCoreApplication::applicationDirPath()
                               + QStringLiteral("/vertical-export");
    QDir().mkpath(outDir);

    // ================= 场景 A：完整编辑链 =================
    {
        const auto fixture = makeLayoutCase(QStringLiteral("01"));
        const DocumentSnapshot baseDoc = fixture.document;
        const LayoutResult baseLayout = projectLayout(baseDoc);
        const DocxBuildResult baseline = buildDocx(baseDoc, baseLayout);
        const QString baselineXml = partText(baseline.package,
                                             QStringLiteral("word/document.xml"));
        const int baselineParagraphs = baselineXml.count(QStringLiteral("<w:p>"));
        check(baseline.result.ok() && baselineParagraphs == 3,
              "A: baseline export has 3 paragraphs");

        Slice s;
        s.init(baseDoc, QStringLiteral("p1"), 0);
        check(s.layout.documentRevision == s.doc.revision,
              "A: projected layout carries current revision");

        // 用重新投影的行对象做命中测试，再按偏移建立选区。
        const TextFragment* first = nullptr;
        for (const auto& f : s.layout.textFragments) {
            if (f.containerId == QStringLiteral("p1")) { first = &f; break; }
        }
        check(first != nullptr, "A: projector produced p1 text fragments");
        Hit hit;
        if (first) {
            const QPointF point(first->rect.left() + 40.0, first->rect.center().y());
            hit = hitTest(s.layout, point);
        }
        check(hit.valid() && hit.containerId == QStringLiteral("p1") && hit.offset > 0,
              "A: hit-test resolves an offset on the fresh layout");

        InteractionState state = s.controller.state();
        state.caret.containerId = QStringLiteral("p1");
        state.caret.offset = hit.offset + 6;
        state.selection = makeSelection(s.layout,
                                        Hit{QStringLiteral("p1"), hit.offset},
                                        Hit{QStringLiteral("p1"), hit.offset + 6});
        s.controller.setState(state);
        const QString chosen = selectedText(s.controller.state(), s.doc);
        check(chosen.size() == 6, "A: six characters selected");

        // Ctrl+B：选区产生一条 SetCharFormat。
        const auto boldOut = s.controller.key(controlKey(Qt::Key_B), s.doc, s.layout);
        check(boldOut.requests.size() == 1
              && boldOut.requests.front().op == EditOp::SetCharFormat,
              "A: Ctrl+B emits one char-format request");
        check(s.commit(boldOut) && hasBoldSpan(s.doc, QStringLiteral("p1")),
              "A: bold request lands in snapshot");

        // 粘贴替换：选区被换掉，光标推进，版本继续前进。
        const int pasteAt = hit.offset;
        const auto pasteOut = s.controller.paste(QStringLiteral("VSlicePaste"),
                                                 s.doc, s.layout);
        check(pasteOut.requests.size() == 1
              && pasteOut.requests.front().op == EditOp::Replace
              && pasteOut.requests.front().range.start == pasteAt
              && pasteOut.requests.front().range.end == pasteAt + 6,
              "A: paste replaces exactly the selection");
        check(s.commit(pasteOut)
              && bodyText(s.doc, QStringLiteral("p1")).mid(pasteAt, 11)
                     == QStringLiteral("VSlicePaste"),
              "A: replacement text present at the selection offset");

        // 输入法：预编辑不进文档，commit 只插入一次。
        ImeInput preedit;
        preedit.preedit = QStringLiteral("VSlicePreedit");
        const auto preeditOut = s.controller.ime(preedit, s.doc, s.layout);
        check(preeditOut.requests.empty(),
              "A: IME preedit emits no document request");
        check(s.controller.state().preedit.text == QStringLiteral("VSlicePreedit"),
              "A: preedit stays in UI state");
        check(!bodyText(s.doc, QStringLiteral("p1"))
                   .contains(QStringLiteral("VSlicePreedit")),
              "A: preedit never enters the document");
        ImeInput committed;
        committed.commit = QStringLiteral("VSliceIme");
        const auto imeOut = s.controller.ime(committed, s.doc, s.layout);
        check(imeOut.requests.size() == 1
              && imeOut.requests.front().op == EditOp::Insert
              && imeOut.requests.front().text == QStringLiteral("VSliceIme"),
              "A: IME commit emits exactly one insert");
        check(s.commit(imeOut)
              && bodyText(s.doc, QStringLiteral("p1"))
                     .count(QStringLiteral("VSliceIme")) == 1,
              "A: committed text appears exactly once");
        check(s.controller.state().preedit.text.isEmpty(),
              "A: preedit cleared after commit");

        // 回车分段、Delete 删除一个字形簇。
        const auto enterOut = s.controller.key(plainKey(Qt::Key_Return),
                                               s.doc, s.layout);
        check(enterOut.requests.size() == 1
              && enterOut.requests.front().text == QStringLiteral("\n"),
              "A: Enter emits a newline insert");
        const int paragraphsBefore = paragraphCount(s.doc, QStringLiteral("p1"));
        check(s.commit(enterOut)
              && paragraphCount(s.doc, QStringLiteral("p1")) == paragraphsBefore + 1,
              "A: Enter splits the paragraph");

        const int lengthBefore = bodyText(s.doc, QStringLiteral("p1")).size();
        const auto deleteOut = s.controller.key(plainKey(Qt::Key_Delete),
                                                s.doc, s.layout);
        check(deleteOut.requests.size() == 1
              && deleteOut.requests.front().op == EditOp::Delete,
              "A: Delete emits one delete request");
        check(s.commit(deleteOut)
              && bodyText(s.doc, QStringLiteral("p1")).size() == lengthBefore - 1,
              "A: Delete removes one grapheme");

        // 工具栏段落样式：当前段落改成二级标题。
        const auto headingOut = s.controller.paragraphFormat(
            s.doc, s.layout, std::nullopt, data::ParagraphRole::Heading, 2);
        check(!headingOut.requests.empty()
              && headingOut.requests.front().op == EditOp::SetParagraphFormat,
              "A: toolbar emits paragraph-format request");
        check(s.commit(headingOut) && hasHeading2(s.doc, QStringLiteral("p1")),
              "A: Heading2 lands in the edited snapshot");

        // 导出并检查最终包。
        const DocxBuildResult finalBuild = buildDocx(s.doc, s.layout);
        const QString xml = partText(finalBuild.package,
                                     QStringLiteral("word/document.xml"));
        check(finalBuild.result.ok(), "A: edited document exports");
        check(editor::io::zip::verifyArchive(finalBuild.package).isEmpty(),
              "A: exported package is a valid ZIP");
        check(xml.count(QStringLiteral("VSlicePaste")) == 1,
              "A: pasted text exported once");
        check(xml.count(QStringLiteral("VSliceIme")) == 1,
              "A: IME text exported once, no duplicate");
        check(!xml.contains(QStringLiteral("VSlicePreedit")),
              "A: preedit text absent from DOCX");
        check(xml.contains(QStringLiteral("<w:b/>")),
              "A: bold run exported");
        check(xml.contains(QStringLiteral("w:val=\"Heading2\"")),
              "A: Heading2 paragraph style exported");
        check(xml.count(QStringLiteral("<w:p>")) == baselineParagraphs + 1,
              "A: Enter produced one extra Word paragraph");
        check(s.doc.revision > baseDoc.revision && s.appliedRequests >= 6,
              "A: revision advanced after the edit loop");

        // 旧排版结果必须被版本校验拒绝。
        const auto stale = buildDocx(s.doc, baseLayout);
        check(stale.result.status == DocxExportStatus::IoError,
              "A: stale layout rejected by revision check");

        const QString path = outDir + QStringLiteral("/edited-01.docx");
        check(exportDocx(s.doc, s.layout, path).ok() && QFile::exists(path),
              "A: wrote Word-openable edited-01.docx");
    }

    // ================= 场景 B：编辑带浮动图片的正文 =================
    {
        const auto fixture = makeLayoutCase(QStringLiteral("02"));
        Slice s;
        s.init(fixture.document, QStringLiteral("body"), 0);

        const auto z = s.controller.key(typedKey(QStringLiteral("Z")), s.doc, s.layout);
        check(z.requests.size() == 1 && s.commit(z),
              "B: typed character applied");
        const auto q = s.controller.key(typedKey(QStringLiteral("Q")), s.doc, s.layout);
        check(q.requests.size() == 1 && s.commit(q),
              "B: second typed character applied at advanced caret");
        check(bodyText(s.doc, QStringLiteral("body")).contains(QStringLiteral("ZQ")),
              "B: caret advancement keeps typed text contiguous");

        const DocxBuildResult build = buildDocx(s.doc, s.layout);
        const QString xml = partText(build.package,
                                     QStringLiteral("word/document.xml"));
        check(build.result.ok(), "B: edited floating-image document exports");
        check(editor::io::zip::verifyArchive(build.package).isEmpty(),
              "B: package valid");
        check(xml.contains(QStringLiteral("<wp:anchor"))
              && xml.contains(QStringLiteral("<wp:wrapSquare wrapText=\"bothSides\"/>")),
              "B: floating anchor and square wrap survive editing");
        check(xml.contains(QStringLiteral("ZQ")),
              "B: edited text exported");
        QByteArray media;
        check(editor::io::zip::extractEntry(build.package,
                                           QStringLiteral("word/media/image1.png"),
                                           &media, nullptr)
              && !QImage::fromData(media).isNull(),
              "B: image media still present and decodable");

        const QString path = outDir + QStringLiteral("/edited-02.docx");
        check(exportDocx(s.doc, s.layout, path).ok(),
              "B: wrote Word-openable edited-02.docx");
    }

    // ================= 场景 C：光标无选区时的待用格式 =================
    {
        const auto fixture = makeLayoutCase(QStringLiteral("01"));
        Slice s;
        s.init(fixture.document, QStringLiteral("p1"), 0);

        data::TextStyleOverride style;
        style.underline = true;
        const auto pending = s.controller.charFormat(s.doc, s.layout, style);
        check(pending.requests.empty()
              && s.controller.state().pendingCharStyle.has_value(),
              "C: toolbar on collapsed caret stores pending style");

        const auto typed = s.controller.key(typedKey(QStringLiteral("X")),
                                           s.doc, s.layout);
        check(typed.requests.size() == 2
              && typed.requests[0].op == EditOp::Insert
              && typed.requests[1].op == EditOp::SetCharFormat,
              "C: next input emits insert + format pair");
        check(s.commit(typed)
              && hasUnderlineSpan(s.doc, QStringLiteral("p1"))
              && !s.controller.state().pendingCharStyle.has_value(),
              "C: pending style consumed by the next input");

        const DocxBuildResult build = buildDocx(s.doc, s.layout);
        const QString xml = partText(build.package,
                                     QStringLiteral("word/document.xml"));
        check(build.result.ok()
              && xml.contains(QStringLiteral("<w:u w:val=\"single\"/>")),
              "C: underline exported as a real run property");

        const QString path = outDir + QStringLiteral("/pending-style.docx");
        check(exportDocx(s.doc, s.layout, path).ok(),
              "C: wrote Word-openable pending-style.docx");
    }

    std::printf("===== Vertical slice self-check: %s =====\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    std::printf("[OK] Vertical slice self-check %s.\n",
                g_failed == 0 ? "passed" : "failed");
    return g_failed == 0 ? 0 : 1;
}
