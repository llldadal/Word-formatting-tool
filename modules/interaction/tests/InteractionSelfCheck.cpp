// 交互模块自测。
// 只使用仓库自带的固定输入和标准排版，不链接其他组的模块。
// 覆盖 I01 / I02 / I03 / I04 / I05 / I06 / I07 / I08，以及 U05 的交互侧行为。

#include "HitTest.h"
#include "InteractionController.h"
#include "Selection.h"
#include "TextBoundary.h"
#include "ContainerDragEngine.h"
#include "ContainerHitTest.h"
#include "RichClipboard.h"
#include "TestFixtures.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>
#include <QtGlobal>

#include <cmath>
#include <optional>
#include <set>
#include <vector>

namespace ei = editor::interaction;
namespace ed = editor::data;
namespace et = editor::testing;

static QTextStream out(stdout);
static int failures = 0;

static void check(bool ok, const QString& what)
{
    out << (ok ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    if (!ok)
        ++failures;
}

static QPointF at(const QRectF& r, qreal ratio)
{
    return QPointF(r.left() + r.width() * ratio, r.center().y());
}

static ei::KeyInput makeKey(int key,
                            Qt::KeyboardModifiers mods = Qt::NoModifier,
                            const QString& text = QString())
{
    ei::KeyInput input;
    input.key = key;
    input.mods = mods;
    input.text = text;
    return input;
}

static ei::EditAction makeAction(ei::EditActionKind kind)
{
    ei::EditAction action;
    action.kind = kind;
    return action;
}

static int surrogatePairStart(const QString& s)
{
    for (int i = 0; i + 1 < s.size(); ++i) {
        if (QChar::isHighSurrogate(s.at(i).unicode())
            && QChar::isLowSurrogate(s.at(i + 1).unicode())) {
            return i;
        }
    }
    return -1;
}

// I01 / I02 / I05：点击定位、容器归属、绕排片段、页边空白。
static void testHitCases()
{
    const QStringList caseIds = { QStringLiteral("02"), QStringLiteral("03"),
                                  QStringLiteral("06"), QStringLiteral("10") };

    for (const QString& caseId : caseIds) {
        out << "\n=== Case " << caseId << ": hit-test ===\n";
        const ed::LayoutResult layout = et::makeStandardLayout(caseId);

        int sampled = 0;
        for (size_t i = 0; i < layout.textFragments.size() && sampled < 8; ++i) {
            const auto& f = layout.textFragments[i];
            if (f.rect.width() <= 0)
                continue;
            const ei::Hit left  = ei::hitTest(layout, at(f.rect, 0.25));
            const ei::Hit right = ei::hitTest(layout, at(f.rect, 0.75));
            const bool ok = left.valid()
                && left.containerId == f.containerId
                && left.offset >= f.sourceRange.start
                && left.offset <= f.sourceRange.end
                && right.containerId == f.containerId
                && right.offset >= left.offset
                && right.offset <= f.sourceRange.end;
            check(ok, QStringLiteral("fragment %1 [%2,%3) container=%4 left=%5 right=%6")
                        .arg(static_cast<int>(i)).arg(f.sourceRange.start).arg(f.sourceRange.end)
                        .arg(f.containerId).arg(left.offset).arg(right.offset));
            ++sampled;
        }

        bool foundPair = false;
        for (size_t i = 0; i < layout.textFragments.size() && !foundPair; ++i) {
            for (size_t j = 0; j < layout.textFragments.size() && !foundPair; ++j) {
                const auto& a = layout.textFragments[i];
                const auto& b = layout.textFragments[j];
                if (a.containerId != b.containerId)
                    continue;
                if (std::fabs(a.rect.y() - b.rect.y()) > 0.5)
                    continue;
                if (b.rect.left() <= a.rect.left())
                    continue;
                const ei::Hit l = ei::hitTest(layout, at(a.rect, 0.25));
                const ei::Hit r = ei::hitTest(layout, at(b.rect, 0.25));
                check(r.offset > l.offset,
                      QStringLiteral("wrap fragments keep source order: %1 -> %2, container %3")
                          .arg(l.offset).arg(r.offset).arg(a.containerId));
                foundPair = true;
            }
        }
        if (!foundPair)
            out << "  (no same-line wrap pair in this case)\n";

        if (caseId == QStringLiteral("06")) {
            std::set<QString> ids;
            for (const auto& f : layout.textFragments)
                ids.insert(f.containerId);
            check(ids.size() >= 2,
                  QStringLiteral("found %1 distinct text containers").arg(static_cast<int>(ids.size())));
            for (const auto& f : layout.textFragments) {
                if (f.containerId != QStringLiteral("note"))
                    continue;
                const ei::Hit h = ei::hitTest(layout, at(f.rect, 0.5));
                check(h.containerId == QStringLiteral("note"),
                      QStringLiteral("click note -> container %1 offset %2")
                          .arg(h.containerId).arg(h.offset));
                break;
            }
        }

        const ei::Hit blank = ei::hitTest(layout, QPointF(740, 500));
        check(blank.valid(), QStringLiteral("blank paper -> container %1 offset %2")
                                .arg(blank.containerId).arg(blank.offset));
    }
}

// I08：字形簇边界。emoji 代理对、组合字符、空段落。
static void testBoundaries()
{
    out << "\n=== Case 10: UTF-16 grapheme boundaries ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("10"));
    const auto* content = ei::findTextContent(fixture.document, QStringLiteral("unicode"));
    check(content != nullptr, QStringLiteral("unicode container found"));
    if (!content)
        return;

    const QString& s = content->text;
    const int len = static_cast<int>(s.size());
    check(ei::textLength(fixture.document, QStringLiteral("unicode")) == len,
          QStringLiteral("container length = QString::size() = %1").arg(qlonglong(len)));

    const int emoji = surrogatePairStart(s);
    check(emoji > 0, QStringLiteral("emoji surrogate pair found at %1").arg(emoji));
    if (emoji > 0) {
        const int next = ei::nextBoundary(s, emoji);
        check(next == emoji + 2,
              QStringLiteral("emoji right move spans 2 code units: %1 -> %2").arg(emoji).arg(next));
        check(ei::prevBoundary(s, emoji + 2) == emoji,
              QStringLiteral("emoji left move returns to pair start"));
        check(ei::nextBoundary(fixture.document, QStringLiteral("unicode"), emoji) == emoji + 2,
              QStringLiteral("container overload matches direct text overload"));
    }

    const int combining = s.indexOf(QChar(0x0301));
    check(combining > 0, QStringLiteral("combining U+0301 found at %1").arg(combining));
    if (combining > 0) {
        check(ei::nextBoundary(s, combining - 1) == combining + 1,
              QStringLiteral("letter + combining mark is one grapheme"));
        check(ei::prevBoundary(s, combining + 1) == combining - 1,
              QStringLiteral("backspace from combining mark returns to letter start"));
    }

    check(ei::nextBoundary(s, len) == len, QStringLiteral("right boundary at text end stays in range"));
    check(ei::prevBoundary(s, 0) == 0, QStringLiteral("left boundary at text start stays in range"));
    check(ei::nextBoundary(QString(), 0) == 0, QStringLiteral("empty text does not crash"));

    const int doubleNewline = s.indexOf(QStringLiteral("\n\n"));
    check(doubleNewline > 0, QStringLiteral("fixture contains an empty paragraph"));
    if (doubleNewline > 0) {
        const int next = ei::nextBoundary(s, doubleNewline);
        check(next > doubleNewline && next <= doubleNewline + 2,
              QStringLiteral("paragraph separator is a boundary: %1 -> %2")
                  .arg(doubleNewline).arg(next));
    }
}

// I04 / I05：区间归一化、分段高亮矩形、Ctrl+A、拖选夹取、光标矩形。
static void testSelection()
{
    out << "\n=== Case 02: split selection / Ctrl+A / drag outside ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const QString body = QStringLiteral("body");

    const ed::TextRange legal = ei::containerRange(layout, body);
    check(legal.start == 0 && legal.end > 200,
          QStringLiteral("body legal range [0, %1)").arg(legal.end));

    const auto sel = ei::makeSelection(layout, body, ed::TextRange{46, 60});
    check(sel.range.start == 46 && sel.range.end == 60,
          QStringLiteral("range normalized to [46, 60)"));
    check(sel.rects.size() == 2,
          QStringLiteral("selection split by image produces %1 rectangles")
              .arg(static_cast<int>(sel.rects.size())));
    if (sel.rects.size() == 2) {
        check(sel.rects[0].left() < 300 && sel.rects[1].left() > 400,
              QStringLiteral("left/right rectangles flank the image: %1 / %2")
                  .arg(sel.rects[0].left()).arg(sel.rects[1].left()));
        for (size_t i = 0; i < sel.rects.size(); ++i) {
            const ei::Hit h = ei::hitTest(layout, sel.rects[i].center());
            check(h.valid() && h.offset >= 46 && h.offset <= 60,
                  QStringLiteral("rectangle %1 center hits offset %2 inside selection")
                      .arg(static_cast<int>(i)).arg(h.offset));
        }
    }

    const auto reverse = ei::makeSelection(layout,
                                           ei::Hit{body, 60},
                                           ei::Hit{body, 46});
    check(reverse.range.start == 46 && reverse.range.end == 60,
          QStringLiteral("reverse drag normalizes to the same range"));

    const auto all = ei::selectAllRange(fixture.document, body);
    check(all.start == 0 && all.end == ei::textLength(fixture.document, body),
          QStringLiteral("Ctrl+A = [0, %1), current container only").arg(all.end));
    const auto allSel = ei::makeSelection(layout, body, all);
    check(allSel.rects.size() >= 10,
          QStringLiteral("select-all produces %1 fragment rectangles")
              .arg(static_cast<int>(allSel.rects.size())));

    const ei::Hit anchor{body, 46};
    const ei::Hit escaped = ei::clampFocus(layout, anchor, QPointF(740, 900));
    check(escaped.containerId == body
              && escaped.offset >= legal.start && escaped.offset <= legal.end,
          QStringLiteral("drag outside stays in body and clamps to %1").arg(escaped.offset));

    const QRectF caret = ei::caretRect(layout, body, 46);
    check(caret.width() > 0 && caret.height() > 0,
          QStringLiteral("caret rect x=%1 height=%2").arg(caret.left()).arg(caret.height()));

    out << "\n=== Case 06: note container does not leak into main text ===\n";
    const auto fixture6 = et::makeLayoutCase(QStringLiteral("06"));
    const auto layout6  = et::makeStandardLayout(QStringLiteral("06"));
    const ed::TextRange noteRange = ei::containerRange(layout6, QStringLiteral("note"));
    const ei::Hit noteAnchor{QStringLiteral("note"), noteRange.start};
    const ei::Hit escaped6 = ei::clampFocus(layout6, noteAnchor, QPointF(120, 140));
    check(escaped6.containerId == QStringLiteral("note")
              && escaped6.offset >= noteRange.start && escaped6.offset <= noteRange.end,
          QStringLiteral("drag into main text stays in note at offset %1").arg(escaped6.offset));

    const auto noteAll = ei::makeSelection(
        layout6, QStringLiteral("note"),
        ei::selectAllRange(fixture6.document, QStringLiteral("note")));
    check(!noteAll.rects.empty(),
          QStringLiteral("note Ctrl+A produces %1 rectangles")
              .arg(static_cast<int>(noteAll.rects.size())));
}

// I03 / I06 / I08：输入、粘贴、回车、退格、删除、复制、剪切、快捷键与移动。
static void testEditRequests()
{
    out << "\n=== I03 / I06 / I08: edit request reduction ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const ed::DocumentSnapshot& doc = fixture.document;
    const QString body = QStringLiteral("body");
    const auto* content = ei::findTextContent(doc, body);
    check(content != nullptr, QStringLiteral("body text content found"));
    if (!content)
        return;

    const int bodyLength = ei::textLength(doc, body);
    ei::InteractionState state = ei::makeInitialState(doc, layout, body, 10);

    auto typed = ei::applyKey(state, makeKey(0, Qt::NoModifier, QStringLiteral("A")),
                              doc, layout);
    check(typed.requests.size() == 1, QStringLiteral("typing produces one request"));
    if (typed.requests.size() == 1) {
        const auto& r = typed.requests[0];
        check(r.containerId == body && r.op == ei::EditOp::Insert
                  && r.range.start == 10 && r.range.end == 10
                  && r.text == QStringLiteral("A"),
              QStringLiteral("typing request is Insert [10,10) with text A"));
    }
    check(typed.state.caret.containerId == body && typed.state.caret.offset == 11,
          QStringLiteral("caret advances to 11 after one inserted character"));
    check(!typed.state.selection.has_value(), QStringLiteral("typing clears selection"));

    state = ei::makeInitialState(doc, layout, body, 20);
    state.selection = ei::makeSelection(layout, body, ed::TextRange{10, 20});
    const QString expectedCopy = content->text.mid(10, 10);

    auto pasted = ei::applyPaste(state, QStringLiteral("XY"), doc, layout);
    check(pasted.requests.size() == 1, QStringLiteral("paste over selection produces one request"));
    if (pasted.requests.size() == 1) {
        const auto& r = pasted.requests[0];
        check(r.op == ei::EditOp::Replace
                  && r.range.start == 10 && r.range.end == 20
                  && r.text == QStringLiteral("XY"),
              QStringLiteral("paste request is Replace [10,20) with text XY"));
    }
    check(pasted.state.caret.offset == 12, QStringLiteral("caret after replacement is 12"));

    auto backspaced = ei::applyKey(state, makeKey(Qt::Key_Backspace), doc, layout);
    check(backspaced.requests.size() == 1, QStringLiteral("Backspace over selection produces one request"));
    if (backspaced.requests.size() == 1) {
        const auto& r = backspaced.requests[0];
        check(r.op == ei::EditOp::Delete && r.range.start == 10 && r.range.end == 20,
              QStringLiteral("Backspace request is Delete [10,20)"));
    }
    check(backspaced.state.caret.offset == 10, QStringLiteral("caret moves to selection start after Backspace"));

    auto deleted = ei::applyKey(state, makeKey(Qt::Key_Delete), doc, layout);
    check(deleted.requests.size() == 1, QStringLiteral("Delete over selection produces one request"));
    if (deleted.requests.size() == 1) {
        const auto& r = deleted.requests[0];
        check(r.op == ei::EditOp::Delete && r.range.start == 10 && r.range.end == 20,
              QStringLiteral("Delete request is Delete [10,20)"));
    }

    auto enter = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                              makeKey(Qt::Key_Return), doc, layout);
    check(enter.requests.size() == 1, QStringLiteral("Enter produces one request"));
    if (enter.requests.size() == 1) {
        const auto& r = enter.requests[0];
        check(r.op == ei::EditOp::Insert
                  && r.range.start == 10 && r.range.end == 10
                  && r.text == QStringLiteral("\n"),
              QStringLiteral("Enter request inserts one LF character"));
    }
    check(enter.state.caret.offset == 11, QStringLiteral("caret advances past inserted LF"));

    auto copied = ei::applyAction(state, makeAction(ei::EditActionKind::Copy), doc, layout);
    check(copied.requests.empty(), QStringLiteral("Copy produces no document request"));
    check(copied.clipboardText.has_value() && *copied.clipboardText == expectedCopy,
          QStringLiteral("Copy clipboard text matches selected source text"));

    auto cut = ei::applyAction(state, makeAction(ei::EditActionKind::Cut), doc, layout);
    check(cut.clipboardText.has_value() && *cut.clipboardText == expectedCopy,
          QStringLiteral("Cut clipboard text matches selected source text"));
    check(cut.requests.size() == 1, QStringLiteral("Cut produces one delete request"));
    if (cut.requests.size() == 1) {
        const auto& r = cut.requests[0];
        check(r.op == ei::EditOp::Delete && r.range.start == 10 && r.range.end == 20,
              QStringLiteral("Cut request is Delete [10,20)"));
    }
    check(cut.state.caret.offset == 10 && !cut.state.selection.has_value(),
          QStringLiteral("Cut clears selection and moves caret to start"));

    auto selectAll = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                                  makeKey(Qt::Key_A, Qt::ControlModifier), doc, layout);
    check(selectAll.state.selection.has_value()
              && selectAll.state.selection->range.start == 0
              && selectAll.state.selection->range.end == bodyLength,
          QStringLiteral("Ctrl+A selects [0, %1)").arg(bodyLength));
    check(selectAll.requests.empty(), QStringLiteral("Ctrl+A produces no document request"));

    auto ctrlC = ei::applyKey(state, makeKey(Qt::Key_C, Qt::ControlModifier), doc, layout);
    check(ctrlC.clipboardText.has_value() && *ctrlC.clipboardText == expectedCopy,
          QStringLiteral("Ctrl+C copies selected text"));

    auto ctrlX = ei::applyKey(state, makeKey(Qt::Key_X, Qt::ControlModifier), doc, layout);
    check(ctrlX.requests.size() == 1
              && ctrlX.clipboardText.has_value()
              && *ctrlX.clipboardText == expectedCopy,
          QStringLiteral("Ctrl+X copies and cuts selected text"));

    auto ctrlV = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                              makeKey(Qt::Key_V, Qt::ControlModifier),
                              doc, layout, QStringLiteral("V"));
    check(ctrlV.requests.size() == 1, QStringLiteral("Ctrl+V produces one request"));
    if (ctrlV.requests.size() == 1) {
        const auto& r = ctrlV.requests[0];
        check(r.op == ei::EditOp::Insert
                  && r.range.start == 10 && r.range.end == 10
                  && r.text == QStringLiteral("V"),
              QStringLiteral("Ctrl+V request inserts clipboard text V"));
    }

    const int right = ei::nextBoundary(doc, body, 10);
    auto movedRight = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                                   makeKey(Qt::Key_Right), doc, layout);
    check(movedRight.state.caret.offset == right && movedRight.requests.empty(),
          QStringLiteral("Right arrow moves one grapheme to %1").arg(right));
    auto movedLeft = ei::applyKey(movedRight.state, makeKey(Qt::Key_Left), doc, layout);
    check(movedLeft.state.caret.offset == ei::prevBoundary(doc, body, right),
          QStringLiteral("Left arrow returns to the previous grapheme boundary"));

    auto shiftRight = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                                   makeKey(Qt::Key_Right, Qt::ShiftModifier), doc, layout);
    check(shiftRight.state.selection.has_value()
              && shiftRight.state.selection->range.start == qMin(10, right)
              && shiftRight.state.selection->range.end == qMax(10, right),
          QStringLiteral("Shift+Right extends selection to the next grapheme"));

    auto movedDown = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                                  makeKey(Qt::Key_Down), doc, layout);
    check(movedDown.state.caret.offset >= 0
              && movedDown.state.caret.offset <= bodyLength
              && movedDown.requests.empty(),
          QStringLiteral("Down arrow stays in document bounds"));

    ei::InteractionState preeditState = ei::makeInitialState(doc, layout, body, 10);
    preeditState.preedit.text = QStringLiteral("ni");
    auto blockedWhilePreedit = ei::applyKey(preeditState,
                                            makeKey(0, Qt::NoModifier, QStringLiteral("A")),
                                            doc, layout);
    check(blockedWhilePreedit.requests.empty()
              && blockedWhilePreedit.state.preedit.text == QStringLiteral("ni"),
          QStringLiteral("plain key input is ignored while IME preedit is active"));
    auto escapedPreedit = ei::applyKey(preeditState, makeKey(Qt::Key_Escape), doc, layout);
    check(escapedPreedit.requests.empty() && escapedPreedit.state.preedit.text.isEmpty(),
          QStringLiteral("Escape cancels preedit without a document request"));

    const auto fixture10 = et::makeLayoutCase(QStringLiteral("10"));
    const auto layout10  = et::makeStandardLayout(QStringLiteral("10"));
    const auto* unicode = ei::findTextContent(fixture10.document, QStringLiteral("unicode"));
    check(unicode != nullptr, QStringLiteral("unicode fixture found for deletion checks"));
    if (unicode) {
        const QString& u = unicode->text;
        const int emoji = surrogatePairStart(u);
        check(emoji > 0, QStringLiteral("unicode fixture has an emoji pair"));
        if (emoji > 0) {
            auto back = ei::applyKey(
                ei::makeInitialState(fixture10.document, layout10,
                                     QStringLiteral("unicode"), emoji + 2),
                makeKey(Qt::Key_Backspace), fixture10.document, layout10);
            check(back.requests.size() == 1
                      && back.requests[0].op == ei::EditOp::Delete
                      && back.requests[0].range.start == emoji
                      && back.requests[0].range.end == emoji + 2,
                  QStringLiteral("Backspace deletes the whole emoji pair"));
            check(back.state.caret.offset == emoji,
                  QStringLiteral("caret returns to emoji pair start"));

            auto forward = ei::applyKey(
                ei::makeInitialState(fixture10.document, layout10,
                                     QStringLiteral("unicode"), emoji),
                makeKey(Qt::Key_Delete), fixture10.document, layout10);
            check(forward.requests.size() == 1
                      && forward.requests[0].range.start == emoji
                      && forward.requests[0].range.end == emoji + 2,
                  QStringLiteral("Delete removes the whole emoji pair"));
        }

        const int combining = u.indexOf(QChar(0x0301));
        if (combining > 0) {
            auto back = ei::applyKey(
                ei::makeInitialState(fixture10.document, layout10,
                                     QStringLiteral("unicode"), combining + 1),
                makeKey(Qt::Key_Backspace), fixture10.document, layout10);
            check(back.requests.size() == 1
                      && back.requests[0].range.start == combining - 1
                      && back.requests[0].range.end == combining + 1,
                  QStringLiteral("Backspace keeps base letter and combining mark together"));
        }

        const int emptyOffset = u.indexOf(QStringLiteral("\n\n")) + 1;
        check(emptyOffset > 0, QStringLiteral("empty paragraph offset found"));
        if (emptyOffset > 0) {
            auto inserted = ei::applyKey(
                ei::makeInitialState(fixture10.document, layout10,
                                     QStringLiteral("unicode"), emptyOffset),
                makeKey(0, Qt::NoModifier, QStringLiteral("X")),
                fixture10.document, layout10);
            check(inserted.requests.size() == 1
                      && inserted.requests[0].op == ei::EditOp::Insert
                      && inserted.requests[0].range.start == emptyOffset
                      && inserted.requests[0].range.end == emptyOffset
                      && inserted.requests[0].text == QStringLiteral("X"),
                  QStringLiteral("empty paragraph accepts insertion at %1").arg(emptyOffset));
            check(inserted.state.caret.offset == emptyOffset + 1,
                  QStringLiteral("caret advances inside empty paragraph"));
        }
    }
}

// I07：预编辑不进文档，commit 只插入一次，候选框跟随光标。
static void testIme()
{
    out << "\n=== I07: IME preedit and commit ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const ed::DocumentSnapshot& doc = fixture.document;
    const QString body = QStringLiteral("body");

    ei::InteractionState state = ei::makeInitialState(doc, layout, body, 10);
    ei::ImeInput first;
    first.preedit = QStringLiteral("ni");
    first.preeditCursor = 2;
    auto preedit = ei::applyIme(state, first, doc, layout);
    check(preedit.requests.empty(), QStringLiteral("preedit produces no document request"));
    check(preedit.state.preedit.text == QStringLiteral("ni")
              && preedit.state.preedit.cursorInPreedit == 2,
          QStringLiteral("preedit stores full composition text and cursor"));
    check(preedit.state.caret.offset == 10,
          QStringLiteral("preedit does not move the document caret"));
    check(preedit.state.preedit.rect.height() > 0,
          QStringLiteral("candidate anchor rect follows current caret"));

    ei::ImeInput commit;
    commit.commit = QStringLiteral("\u4F60\u597D");
    auto committed = ei::applyIme(preedit.state, commit, doc, layout);
    check(committed.requests.size() == 1, QStringLiteral("commit produces exactly one request"));
    if (committed.requests.size() == 1) {
        const auto& r = committed.requests[0];
        check(r.op == ei::EditOp::Insert
                  && r.range.start == 10 && r.range.end == 10
                  && r.text == commit.commit,
              QStringLiteral("commit inserts only commitString, not preedit text"));
    }
    check(committed.state.caret.offset == 12,
          QStringLiteral("caret advances by 2 UTF-16 units after Chinese commit"));
    check(committed.state.preedit.text.isEmpty(),
          QStringLiteral("preedit is cleared after commit"));

    ei::InteractionState selected = ei::makeInitialState(doc, layout, body, 20);
    selected.selection = ei::makeSelection(layout, body, ed::TextRange{10, 20});
    ei::ImeInput replaceCommit;
    replaceCommit.commit = QStringLiteral("H");
    auto replaced = ei::applyIme(selected, replaceCommit, doc, layout);
    check(replaced.requests.size() == 1, QStringLiteral("IME commit with selection produces one request"));
    if (replaced.requests.size() == 1) {
        const auto& r = replaced.requests[0];
        check(r.op == ei::EditOp::Replace
                  && r.range.start == 10 && r.range.end == 20
                  && r.text == QStringLiteral("H"),
              QStringLiteral("IME commit replaces selected range exactly once"));
    }
    check(replaced.state.caret.offset == 11 && !replaced.state.selection.has_value(),
          QStringLiteral("IME commit clears selection and sets caret after inserted text"));

    ei::ImeInput simultaneous;
    simultaneous.commit = QStringLiteral("A");
    simultaneous.preedit = QStringLiteral("B");
    simultaneous.preeditCursor = 1;
    auto both = ei::applyIme(ei::makeInitialState(doc, layout, body, 10),
                             simultaneous, doc, layout);
    check(both.requests.size() == 1, QStringLiteral("simultaneous commit + new preedit inserts once"));
    if (both.requests.size() == 1)
        check(both.requests[0].text == QStringLiteral("A"),
              QStringLiteral("only commitString reaches the document"));
    check(both.state.preedit.text == QStringLiteral("B")
              && both.state.preedit.cursorInPreedit == 1,
          QStringLiteral("new preedit remains UI-only after commit"));
    check(both.state.caret.offset == 11,
          QStringLiteral("caret advances only for committed text"));

    ei::InteractionState oldPreedit = ei::makeInitialState(doc, layout, body, 10);
    oldPreedit.preedit.text = QStringLiteral("ni");
    ei::ImeInput replacement;
    replacement.preedit = QStringLiteral("nihao");
    replacement.replacementStart = 2;
    replacement.replacementLength = 3;
    auto cursorOnly = ei::applyIme(oldPreedit, replacement, doc, layout);
    // 没有上屏文字时，替换范围只用来推导预编辑光标，确实不该编辑文档。
    check(cursorOnly.requests.empty(), QStringLiteral("preedit-only event never edits document"));
    check(cursorOnly.state.preedit.cursorInPreedit == 5,
          QStringLiteral("replacement attributes compute preedit cursor at 5"));

    // 带上屏文字时，replacementStart/Length 必须变成真正的 Replace 请求。
    // 旧断言把「忽略替换范围」当成了正确行为，正好把 bug 锁死在测试里。
    ei::ImeInput imeReplaceCommit;
    imeReplaceCommit.commit = QStringLiteral("H");
    imeReplaceCommit.replacementStart = -1;
    imeReplaceCommit.replacementLength = 1;
    auto replacedByIme = ei::applyIme(ei::makeInitialState(doc, layout, body, 10),
                                      imeReplaceCommit, doc, layout);
    check(replacedByIme.requests.size() == 1
              && replacedByIme.requests[0].op == ei::EditOp::Replace
              && replacedByIme.requests[0].range.start == 9
              && replacedByIme.requests[0].range.end == 10
              && replacedByIme.requests[0].text == QStringLiteral("H"),
          QStringLiteral("IME replacement becomes a Replace request over [9,10)"));
    check(replacedByIme.state.caret.offset == 10,
          QStringLiteral("caret stays after the replaced text"));

    // preeditCursor = 0 是「光标在最前面」，不能被当成「输入法没给」。
    ei::ImeInput zeroCursor;
    zeroCursor.preedit = QStringLiteral("abc");
    zeroCursor.preeditCursor = 0;
    auto atStart = ei::applyIme(ei::makeInitialState(doc, layout, body, 10),
                                zeroCursor, doc, layout);
    check(atStart.state.preedit.cursorInPreedit == 0,
          QStringLiteral("preeditCursor=0 is a real cursor position"));
}

// U05：工具栏字符格式与段落角色转成有序的 EditRequest。
static void testToolbarFormatting()
{
    out << "\n=== U05: toolbar format requests ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const ed::DocumentSnapshot& doc = fixture.document;
    const QString body = QStringLiteral("body");

    ei::InteractionState selected = ei::makeInitialState(doc, layout, body, 60);
    selected.selection = ei::makeSelection(layout, body, ed::TextRange{46, 60});

    ed::TextStyleOverride bold;
    bold.bold = true;
    auto bolded = ei::applyCharFormat(selected, doc, layout, bold);
    check(bolded.requests.size() == 1, QStringLiteral("selected bold produces one request"));
    if (bolded.requests.size() == 1) {
        const auto& r = bolded.requests[0];
        check(r.op == ei::EditOp::SetCharFormat
                  && r.range.start == 46 && r.range.end == 60
                  && r.charStyle.bold.has_value() && r.charStyle.bold.value(),
              QStringLiteral("bold request targets [46,60) with bold=true"));
    }
    check(bolded.state.selection.has_value() && bolded.state.pendingCharStyle.has_value()
              && bolded.state.pendingCharStyle->bold.has_value()
              && bolded.state.pendingCharStyle->bold.value(),
          QStringLiteral("selected format keeps selection and stores style for following input"));

    auto shortcut = ei::applyKey(selected, makeKey(Qt::Key_B, Qt::ControlModifier),
                                 doc, layout);
    check(shortcut.requests.size() == 1
              && shortcut.requests[0].op == ei::EditOp::SetCharFormat
              && shortcut.requests[0].charStyle.bold.has_value(),
          QStringLiteral("Ctrl+B maps to a character format request"));

    ei::InteractionState collapsed = ei::makeInitialState(doc, layout, body, 10);
    auto pendingBold = ei::applyCharFormat(collapsed, doc, layout, bold);
    check(pendingBold.requests.empty() && pendingBold.state.pendingCharStyle.has_value(),
          QStringLiteral("collapsed toolbar format stores pending style without a request"));

    ed::TextStyleOverride italic;
    italic.italic = true;
    auto pendingBoth = ei::applyCharFormat(pendingBold.state, doc, layout, italic);
    check(pendingBoth.state.pendingCharStyle.has_value(),
          QStringLiteral("two collapsed toolbar choices merge into one pending style"));
    if (pendingBoth.state.pendingCharStyle.has_value()) {
        const auto& pending = *pendingBoth.state.pendingCharStyle;
        check(pending.bold.has_value() && pending.bold.value()
                  && pending.italic.has_value() && pending.italic.value(),
              QStringLiteral("pending style keeps bold and italic"));
    }

    auto inserted = ei::applyKey(pendingBoth.state,
                                 makeKey(0, Qt::NoModifier, QStringLiteral("Z")),
                                 doc, layout);
    check(inserted.requests.size() == 2,
          QStringLiteral("typing with pending style emits Insert then SetCharFormat"));
    if (inserted.requests.size() == 2) {
        const auto& first = inserted.requests[0];
        const auto& second = inserted.requests[1];
        check(first.op == ei::EditOp::Insert
                  && first.range.start == 10 && first.range.end == 10
                  && first.text == QStringLiteral("Z"),
              QStringLiteral("first request inserts Z at [10,10)"));
        check(second.op == ei::EditOp::SetCharFormat
                  && second.range.start == 10 && second.range.end == 11
                  && second.charStyle.bold.has_value()
                  && second.charStyle.italic.has_value(),
              QStringLiteral("second request applies pending style to inserted range"));
    }
    // 折叠光标连续输入时，待用格式必须在后续字符上继续生效。
    check(inserted.state.pendingCharStyle.has_value(),
          QStringLiteral("pending style is kept after insertion for following characters"));

    auto insertedNext = ei::applyKey(inserted.state,
                                     makeKey(0, Qt::NoModifier, QStringLiteral("Y")),
                                     doc, layout);
    check(insertedNext.requests.size() == 2,
          QStringLiteral("second consecutive character still emits Insert then SetCharFormat"));
    if (insertedNext.requests.size() == 2) {
        const auto& formatReq = insertedNext.requests[1];
        check(formatReq.op == ei::EditOp::SetCharFormat
                  && formatReq.range.start == 11 && formatReq.range.end == 12
                  && formatReq.charStyle.bold.has_value()
                  && formatReq.charStyle.italic.has_value(),
              QStringLiteral("second character keeps pending bold+italic style"));
    }

    // 带着待用格式去替换选区时，格式仍要保留：替换不是"修改格式"，
    // 后续输入应继续沿用同一个格式。
    ei::InteractionState pendingWithSelection = selected;
    pendingWithSelection.pendingCharStyle = bold;
    auto replaced = ei::applyPaste(pendingWithSelection, QStringLiteral("W"), doc, layout);
    check(replaced.state.pendingCharStyle.has_value()
              && replaced.state.pendingCharStyle->bold.has_value(),
          QStringLiteral("replacing a selection keeps pending style"));

    ed::ParagraphFormat paragraphFormat;
    paragraphFormat.alignment = ed::ParagraphAlignment::Center;
    paragraphFormat.lineHeight = 30.0;
    auto paragraph = ei::applyParagraphFormat(selected, doc, layout,
                                              paragraphFormat,
                                              ed::ParagraphRole::Heading,
                                              2);
    const auto paragraphRanges = ei::paragraphRangesFor(doc, body, ed::TextRange{46, 60});
    check(!paragraph.requests.empty(), QStringLiteral("paragraph format produces requests"));
    check(paragraph.requests.size() == paragraphRanges.size(),
          QStringLiteral("one paragraph format request per overlapped paragraph"));
    bool paragraphShapeOk = !paragraph.requests.empty();
    for (const auto& r : paragraph.requests) {
        paragraphShapeOk = paragraphShapeOk
            && r.op == ei::EditOp::SetParagraphFormat
            && r.paragraphFormat.has_value()
            && r.paragraphFormat->alignment == ed::ParagraphAlignment::Center
            && qFuzzyCompare(r.paragraphFormat->lineHeight, 30.0)
            && r.role.has_value()
            && r.role.value() == ed::ParagraphRole::Heading
            && r.headingLevel.has_value()
            && r.headingLevel.value() == 2;
    }
    check(paragraphShapeOk,
          QStringLiteral("paragraph requests carry center alignment, Heading role, level 2"));
    if (!paragraphRanges.empty() && !paragraph.requests.empty()) {
        check(paragraph.requests[0].range.start == paragraphRanges[0].start
                  && paragraph.requests[0].range.end == paragraphRanges[0].end,
              QStringLiteral("paragraph request uses full paragraph range"));
    }

    const auto fixture10 = et::makeLayoutCase(QStringLiteral("10"));
    const auto layout10  = et::makeStandardLayout(QStringLiteral("10"));
    const auto* unicode = ei::findTextContent(fixture10.document, QStringLiteral("unicode"));
    if (unicode) {
        const int doubleNewline = unicode->text.indexOf(QStringLiteral("\n\n"));
        check(doubleNewline > 0, QStringLiteral("fixture has a paragraph break for multi-paragraph test"));
        if (doubleNewline > 1) {
            const ed::TextRange across{doubleNewline - 1, doubleNewline + 2};
            const auto ranges = ei::paragraphRangesFor(fixture10.document,
                                                       QStringLiteral("unicode"), across);
            check(ranges.size() >= 2,
                  QStringLiteral("selection crosses %1 paragraph ranges")
                      .arg(static_cast<int>(ranges.size())));
            ei::InteractionState cross = ei::makeInitialState(fixture10.document, layout10,
                                                             QStringLiteral("unicode"),
                                                             doubleNewline - 1);
            cross.selection = ei::makeSelection(layout10, QStringLiteral("unicode"), across);
            auto multi = ei::applyParagraphFormat(cross, fixture10.document, layout10,
                                                  paragraphFormat);
            check(multi.requests.size() == ranges.size(),
                  QStringLiteral("multi-paragraph selection emits one request per range"));
        }
    }
}

// 有状态封装保持自身状态，并用后来的版面刷新几何。
static void testController()
{
    out << "\n=== InteractionController state wrapper ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const ed::DocumentSnapshot& doc = fixture.document;
    const QString body = QStringLiteral("body");

    ei::InteractionController controller(doc, layout, body, 10);
    auto typed = controller.key(makeKey(0, Qt::NoModifier, QStringLiteral("A")),
                                doc, layout);
    check(controller.state().caret.offset == typed.state.caret.offset,
          QStringLiteral("controller state matches returned output state"));

    auto refreshed = controller.refresh(layout);
    check(refreshed.state.caret.rect.height() > 0,
          QStringLiteral("refresh recomputes a usable caret rectangle"));

    auto all = controller.selectAll(doc, layout);
    check(all.state.selection.has_value()
              && all.state.selection->range.end == ei::textLength(doc, body),
          QStringLiteral("controller selectAll covers current container"));
}

// 回归用例：把本轮修掉的每一类问题各写一条断言，防止以后被改回去。
static void testInteractionRegressions()
{
    out << "\n=== R: interaction regression cases ===\n";
    const auto fixture = et::makeLayoutCase(QStringLiteral("02"));
    const auto layout  = et::makeStandardLayout(QStringLiteral("02"));
    const ed::DocumentSnapshot& doc = fixture.document;
    const QString body = QStringLiteral("body");

    // 粘贴和输入法上屏也要过滤 XML 非法控制字符，不能只有键盘直输检查。
    QString dirty = QStringLiteral("A");
    dirty.append(QChar(0x01));
    dirty.append(QChar(0x7f));
    dirty.append(QStringLiteral("B"));
    auto pasted = ei::applyPaste(ei::makeInitialState(doc, layout, body, 10),
                                 dirty, doc, layout);
    check(pasted.requests.size() == 1
              && pasted.requests[0].text == QStringLiteral("AB"),
          QStringLiteral("paste strips XML-illegal control characters"));
    ei::ImeInput dirtyIme;
    dirtyIme.commit = dirty;
    auto imeIn = ei::applyIme(ei::makeInitialState(doc, layout, body, 10),
                              dirtyIme, doc, layout);
    check(imeIn.requests.size() == 1
              && imeIn.requests[0].text == QStringLiteral("AB"),
          QStringLiteral("IME commit strips XML-illegal control characters"));

    // 没有选区时 Ctrl+C 不应该把系统剪贴板清空。
    auto emptyCopy = ei::applyKey(ei::makeInitialState(doc, layout, body, 10),
                                  makeKey(Qt::Key_C, Qt::ControlModifier), doc, layout);
    check(!emptyCopy.clipboardText.has_value(),
          QStringLiteral("Ctrl+C without a selection leaves the clipboard untouched"));

    // 待用格式一旦设下就一直保持，直到用户再次主动修改；移动光标、删除只是
    // 编辑内容，不应让格式失效（点击回退属集成层状态重建，不在这里测）。
    ed::TextStyleOverride bold;
    bold.bold = true;
    auto pending = ei::applyCharFormat(ei::makeInitialState(doc, layout, body, 10),
                                       doc, layout, bold);
    check(pending.state.pendingCharStyle.has_value(),
          QStringLiteral("pending style is set before the caret moves"));
    auto moved = ei::applyKey(pending.state, makeKey(Qt::Key_Right), doc, layout);
    check(moved.state.pendingCharStyle.has_value()
              && moved.state.pendingCharStyle->bold.value_or(false),
          QStringLiteral("moving the caret keeps the pending character style"));
    auto deleted = ei::applyKey(pending.state, makeKey(Qt::Key_Backspace), doc, layout);
    check(deleted.state.pendingCharStyle.has_value()
              && deleted.state.pendingCharStyle->bold.value_or(false),
          QStringLiteral("deleting keeps the pending character style"));
    auto selectAllPending = ei::applyKey(pending.state, makeKey(Qt::Key_A, Qt::ControlModifier),
                                         doc, layout);
    check(selectAllPending.state.pendingCharStyle.has_value()
              && selectAllPending.state.pendingCharStyle->bold.value_or(false),
          QStringLiteral("select all keeps the pending character style"));
    auto cutBase = ei::makeInitialState(doc, layout, body, 10);
    cutBase.selection = ei::makeSelection(layout, body, {5, 12});
    cutBase.selectionAnchor = 5;
    cutBase.pendingCharStyle = bold;
    auto cutKept = ei::applyAction(cutBase, makeAction(ei::EditActionKind::Cut), doc, layout);
    check(cutKept.state.pendingCharStyle.has_value()
              && cutKept.state.pendingCharStyle->bold.value_or(false),
          QStringLiteral("cutting keeps the pending character style"));

    // 连续上下移动要一直沿用第一次记下的期望列。
    auto down1 = ei::applyKey(ei::makeInitialState(doc, layout, body, 100),
                              makeKey(Qt::Key_Down), doc, layout);
    check(down1.state.desiredCaretX.has_value(),
          QStringLiteral("first Down records the desired column"));
    auto down2 = ei::applyKey(down1.state, makeKey(Qt::Key_Down), doc, layout);
    if (down1.state.desiredCaretX && down2.state.desiredCaretX)
        check(qFuzzyCompare(*down1.state.desiredCaretX, *down2.state.desiredCaretX),
              QStringLiteral("second Down reuses the same desired column"));
    auto sideways = ei::applyKey(down2.state, makeKey(Qt::Key_Left), doc, layout);
    check(!sideways.state.desiredCaretX.has_value(),
          QStringLiteral("a horizontal move clears the desired column"));

    // 鼠标从右往左反向拖选后，Shift+方向键必须用保存的锚点。
    ei::InteractionState reverseSelection = ei::makeInitialState(doc, layout, body, 46);
    reverseSelection.selection = ei::makeSelection(layout, body, ed::TextRange{20, 46});
    reverseSelection.selectionAnchor = 46;
    auto shiftLeft = ei::applyKey(reverseSelection,
                                  makeKey(Qt::Key_Left, Qt::ShiftModifier), doc, layout);
    check(shiftLeft.state.selectionAnchor == 46
              && shiftLeft.state.selection.has_value()
              && shiftLeft.state.selection->range.start < 46,
          QStringLiteral("Shift+Left keeps the stored anchor after a reverse drag"));

    // 段落格式请求不能只覆盖前 128 段。
    ed::DocumentSnapshot many;
    ed::ContainerNode longNode;
    longNode.id = QStringLiteral("long");
    longNode.type = ed::ContainerType::Text;
    for (int i = 0; i < 200; ++i)
        longNode.text.text += QStringLiteral("a\n");
    many.root.children.push_back(longNode);
    const auto ranges = ei::paragraphRangesFor(
        many, longNode.id,
        ed::TextRange{0, static_cast<int>(longNode.text.text.size())});
    check(ranges.size() > 128,
          QStringLiteral("paragraphRangesFor covers %1 paragraphs (no 128 cap)")
              .arg(static_cast<int>(ranges.size())));

    // 边界判断不能把本来就在边界上的位置往左挪一格。
    check(ei::isGraphemeBoundary(QStringLiteral("abc"), 2),
          QStringLiteral("a plain character offset is a grapheme boundary"));
    check(ei::alignToGrapheme(QStringLiteral("abc"), 2) == 2,
          QStringLiteral("alignToGrapheme leaves an existing boundary unchanged"));
    QString emoji = QStringLiteral("a");
    emoji.append(QChar(0xD83D));
    emoji.append(QChar(0xDE00));
    emoji.append(QStringLiteral("b"));
    check(ei::alignToGrapheme(emoji, 2) != 2,
          QStringLiteral("the middle of an emoji pair is snapped to a boundary"));

    // 页边空白点击要兼顾水平距离，不能只按 y 归到左边的正文容器。
    // case 06 右侧有一个浮动的 note 文本框，点在它下方偏右的空白处。
    const auto layout6 = et::makeStandardLayout(QStringLiteral("06"));
    const ei::Hit blankRight = ei::hitTest(layout6, QPointF(650, 560));
    check(blankRight.containerId == QStringLiteral("note"),
          QStringLiteral("blank click below the right-hand box picks that container"));
}

static void testContainerDragEngine()
{
    out << "\n=== ContainerDragEngine: drag drop planning ===\n";

    // 构造测试快照：根节点包含 2 个文本容器
    ed::DocumentSnapshot doc;
    doc.root.id = QStringLiteral("root");
    doc.root.type = ed::ContainerType::Group;
    doc.page.size = QSizeF(800, 1000);
    doc.page.margins = QMarginsF(50, 50, 50, 50);

    ed::ContainerNode textA;
    textA.id = QStringLiteral("textA");
    textA.type = ed::ContainerType::Text;
    textA.placement = ed::PlacementMode::Flow;
    textA.geometry.width = 600;

    ed::ContainerNode textB;
    textB.id = QStringLiteral("textB");
    textB.type = ed::ContainerType::Text;
    textB.placement = ed::PlacementMode::Flow;
    textB.geometry.width = 600;

    doc.root.children = {textA, textB};

    // 构造基础排版：textA 在 y=50..150 (center 100), textB 在 y=160..260 (center 210)
    ed::LayoutResult baseLayout;
    baseLayout.contentRect = QRectF(50, 50, 700, 900);

    ed::ContainerLayout boxA;
    boxA.containerId = QStringLiteral("textA");
    boxA.parentId = QStringLiteral("root");
    boxA.rect = QRectF(50, 50, 600, 100);
    boxA.contentRect = boxA.rect;

    ed::ContainerLayout boxB;
    boxB.containerId = QStringLiteral("textB");
    boxB.parentId = QStringLiteral("root");
    boxB.rect = QRectF(50, 160, 600, 100);
    boxB.contentRect = boxB.rect;

    baseLayout.containers = {boxA, boxB};

    // 1. 测试容器向下移动 (textA 往下挪越过 textB)
    const QRectF initialRectA = boxA.rect;
    const QPointF deltaDown(0, 150); // y: 50 -> 200, center: 100 -> 250 (> 210)
    const QPointF paperPosDown(300, 250);
    auto planDown = ei::ContainerDragEngine::planContainerMove(
        QStringLiteral("textA"), ed::ContainerType::Text,
        initialRectA, deltaDown, paperPosDown, baseLayout, doc);

    check(planDown.targetIndex == 1, QStringLiteral("textA moving down past textB targets index 1"));
    check(planDown.targetParentId == QStringLiteral("root"), QStringLiteral("textA targets root parent"));
    check(planDown.placementMode == ed::PlacementMode::Flow, QStringLiteral("placement is Flow"));

    // 2. 测试容器向上移动 (textB 往上挪越过 textA)
    const QRectF initialRectB = boxB.rect;
    const QPointF deltaUp(0, -130); // y: 160 -> 30, center: 210 -> 80 (< 100)
    const QPointF paperPosUp(300, 80);
    auto planUp = ei::ContainerDragEngine::planContainerMove(
        QStringLiteral("textB"), ed::ContainerType::Text,
        initialRectB, deltaUp, paperPosUp, baseLayout, doc);

    check(planUp.targetIndex == 0, QStringLiteral("textB moving up past textA targets index 0"));
    check(planUp.targetParentId == QStringLiteral("root"), QStringLiteral("textB targets root parent"));

    // 3. 测试图片跨容器迁移规划
    ed::ContainerNode imgNode;
    imgNode.id = QStringLiteral("img1");
    imgNode.type = ed::ContainerType::Image;
    imgNode.placement = ed::PlacementMode::Floating;
    imgNode.image.displaySize = QSizeF(80, 80);
    doc.root.children.front().children = {imgNode};

    ed::ImageLayout imgLayout;
    imgLayout.containerId = QStringLiteral("img1");
    imgLayout.rect = QRectF(60, 60, 80, 80);
    baseLayout.images = {imgLayout};

    // 将图片拖入 textB 的内容区 (y=180, x=100)
    const QRectF imgInitialRect(60, 60, 80, 80);
    const QPointF imgDelta(40, 120); // y: 60 -> 180 (inside textB's 160..260)
    const QPointF imgPaper(100, 180);
    auto imgPlan = ei::ContainerDragEngine::planImageMove(
        QStringLiteral("img1"), QStringLiteral("textA"),
        imgInitialRect, imgDelta, imgPaper, baseLayout, doc);

    check(imgPlan.targetParentId == QStringLiteral("textB"), QStringLiteral("img1 correctly detects textB as target"));
    check(imgPlan.placementMode == ed::PlacementMode::Floating, QStringLiteral("img1 placement mode is Floating"));

    // 4. 测试候选快照变更应用 (applyPlanToCandidateSnapshot)
    auto candidate = doc;
    bool applied = ei::ContainerDragEngine::applyPlanToCandidateSnapshot(
        candidate, QStringLiteral("img1"), imgPlan, imgNode.geometry);
    check(applied, QStringLiteral("applyPlanToCandidateSnapshot succeeded"));
    check(candidate.root.children.front().children.empty(), QStringLiteral("textA no longer has img1"));
    check(!candidate.root.children.back().children.empty(), QStringLiteral("textB now has img1"));
    check(candidate.root.children.back().children.front().id == QStringLiteral("img1"), QStringLiteral("textB child is img1"));

    // 5. 测试高文本容器前沿阈值重排（无 100px+ 死区阻滞）与全宽 X/Y 夹紧
    ed::DocumentSnapshot tallDoc = doc;
    tallDoc.root.children[0].children.clear();
    tallDoc.root.children[0].geometry.width = 0; // 全宽容器
    tallDoc.root.children[1].geometry.width = 0;
    ed::LayoutResult tallLayout;
    tallLayout.contentRect = QRectF(50, 50, 700, 900);
    ed::ContainerLayout tallA{QStringLiteral("textA"), QStringLiteral("root"), QRectF(50, 50, 700, 200), QRectF(50, 50, 700, 200), QRectF(50, 50, 700, 200)};
    ed::ContainerLayout tallB{QStringLiteral("textB"), QStringLiteral("root"), QRectF(50, 250, 700, 200), QRectF(50, 250, 700, 200), QRectF(50, 250, 700, 200)};
    tallLayout.containers = {tallA, tallB};

    // 拖动 tallA 向下仅 45px（远小于中心点跨距 200px，但越过 36px 前沿阈值）且伴随向右偏移 +60px
    auto tallPlan = ei::ContainerDragEngine::planContainerMove(
        QStringLiteral("textA"), ed::ContainerType::Text,
        tallA.rect, QPointF(60, 45), QPointF(300, 150), tallLayout, tallDoc, 1.0);
    check(tallPlan.targetIndex == 1, QStringLiteral("tall container swaps downward immediately past 36px threshold"));
    check(tallPlan.localPosition.x() == 0 && tallPlan.localPosition.y() == 0,
          QStringLiteral("full-width container stays clamped at x=0 and resets y=0 on reorder"));
    check(tallPlan.insertionIndicator.has_value(), QStringLiteral("insertion indicator is computed for reorder"));

    // 6. 测试图片 8 手柄命中测试与边缘不误判为 ContainerBorder
    ed::ContainerLayout imgBox;
    imgBox.containerId = QStringLiteral("img1");
    imgBox.parentId = QStringLiteral("textA");
    imgBox.rect = QRectF(60, 60, 80, 80);
    imgBox.contentRect = imgBox.rect;
    baseLayout.containers.push_back(imgBox);

    ei::ContainerSelectionState emptySel;
    const auto edgeHit = ei::hitTestScene(QPointF(61, 100), baseLayout, doc, emptySel, 1.0);
    check(edgeHit.type == ei::ContainerHitType::FloatingImage,
          QStringLiteral("clicking near unselected image edge hits FloatingImage instead of ContainerBorder"));

    ei::ContainerSelectionState imgSel;
    imgSel.select(QStringLiteral("img1"));
    const auto cornerHit = ei::hitTestScene(QPointF(140, 140), baseLayout, doc, imgSel, 1.0);
    check(cornerHit.type == ei::ContainerHitType::ResizeHandleBottomRight &&
          cornerHit.containerId == QStringLiteral("img1"),
          QStringLiteral("hitTestScene detects ResizeHandleBottomRight on selected image"));
    const auto topHit = ei::hitTestScene(QPointF(100, 60), baseLayout, doc, imgSel, 1.0);
    check(topHit.type == ei::ContainerHitType::ResizeHandleTop &&
          topHit.containerId == QStringLiteral("img1"),
          QStringLiteral("hitTestScene detects ResizeHandleTop on selected image"));

    // 7. 测试图片等比例角缩放、单边拉伸及最小尺寸保护
    const QRectF parentContent(50, 50, 600, 400);
    auto cornerResizePlan = ei::ContainerDragEngine::planImageResize(
        QStringLiteral("img1"), ei::ContainerHitType::ResizeHandleBottomRight,
        imgBox.rect, QSizeF(80, 80), imgNode.geometry,
        QPointF(40, 20), parentContent, baseLayout, /*keepAspectRatio=*/true, 1.0);
    check(cornerResizePlan.targetDisplaySize.has_value() &&
          std::abs(cornerResizePlan.targetDisplaySize->width() - 120.0) < 0.01 &&
          std::abs(cornerResizePlan.targetDisplaySize->height() - 120.0) < 0.01,
          QStringLiteral("corner image resize scales proportionally to 120x120"));

    auto edgeResizePlan = ei::ContainerDragEngine::planImageResize(
        QStringLiteral("img1"), ei::ContainerHitType::ResizeHandleRight,
        imgBox.rect, QSizeF(80, 80), imgNode.geometry,
        QPointF(-20, 15), parentContent, baseLayout, /*keepAspectRatio=*/false, 1.0);
    check(edgeResizePlan.targetDisplaySize.has_value() &&
          std::abs(edgeResizePlan.targetDisplaySize->width() - 60.0) < 0.01 &&
          std::abs(edgeResizePlan.targetDisplaySize->height() - 80.0) < 0.01,
          QStringLiteral("right edge resize shrinks width to 60 while keeping height 80"));

    auto minClampPlan = ei::ContainerDragEngine::planImageResize(
        QStringLiteral("img1"), ei::ContainerHitType::ResizeHandleBottomRight,
        imgBox.rect, QSizeF(80, 80), imgNode.geometry,
        QPointF(-200, -200), parentContent, baseLayout, /*keepAspectRatio=*/true, 1.0);
    check(minClampPlan.targetDisplaySize.has_value() &&
          minClampPlan.targetDisplaySize->width() >= 24.0 &&
          minClampPlan.targetDisplaySize->height() >= 24.0,
          QStringLiteral("image resize clamps to minimum 24x24 display size"));

    auto imgCandidate = doc;
    check(ei::ContainerDragEngine::applyPlanToCandidateSnapshot(
              imgCandidate, QStringLiteral("img1"), cornerResizePlan, imgNode.geometry) &&
          std::abs(imgCandidate.root.children.front().children.front().image.displaySize.width() - 120.0) < 0.01,
          QStringLiteral("applyPlanToCandidateSnapshot updates image displaySize"));

    // 8. 测试 SnapEngine 拖动父容器时不会与自身子图片产生自吸附
    const QRectF selfSnapProbe(60, 50, 600, 100); // left=60 正好等于 child img1.left()
    auto selfSnap = ei::SnapEngine::snapMove(selfSnapProbe, baseLayout, QStringLiteral("textA"), 4.0, true);
    check(!selfSnap.snappedX || std::abs(selfSnap.snappedTopLeft.x() - 60.0) > 0.01,
          QStringLiteral("SnapEngine ignores child images of the dragged container"));

    // 9. 测试正文顶层容器不暴露左右改宽手柄，而 Group 与 Group 内子文本保留左右改宽手柄
    ei::ContainerSelectionState topTextSel;
    topTextSel.select(QStringLiteral("textA"));
    const auto topTextLeftHit = ei::hitTestScene(QPointF(boxA.rect.left(), boxA.rect.center().y()),
                                                 baseLayout, doc, topTextSel, 1.0);
    const auto topTextRightHit = ei::hitTestScene(QPointF(boxA.rect.right(), boxA.rect.center().y()),
                                                  baseLayout, doc, topTextSel, 1.0);
    check(topTextLeftHit.type == ei::ContainerHitType::ContainerBorder &&
          topTextRightHit.type == ei::ContainerHitType::ContainerBorder,
          QStringLiteral("top-level text container edges return ContainerBorder instead of resize handles"));

    ed::DocumentSnapshot groupDoc = doc;
    ed::ContainerNode grp;
    grp.id = QStringLiteral("grp1");
    grp.type = ed::ContainerType::Group;
    grp.placement = ed::PlacementMode::Flow;
    ed::ContainerNode subText;
    subText.id = QStringLiteral("subText");
    subText.type = ed::ContainerType::Text;
    subText.placement = ed::PlacementMode::Flow;
    grp.children = {subText};
    groupDoc.root.children.push_back(grp);

    ed::LayoutResult groupLayout = baseLayout;
    ed::ContainerLayout grpBox{QStringLiteral("grp1"), QStringLiteral("root"),
                               QRectF(50, 280, 600, 120), QRectF(50, 280, 600, 120), QRectF(50, 280, 600, 120)};
    ed::ContainerLayout subBox{QStringLiteral("subText"), QStringLiteral("grp1"),
                               QRectF(60, 290, 500, 80), QRectF(60, 290, 500, 80), QRectF(60, 290, 500, 80)};
    groupLayout.containers.push_back(grpBox);
    groupLayout.containers.push_back(subBox);

    ei::ContainerSelectionState grpSel;
    grpSel.select(QStringLiteral("grp1"));
    check(ei::hitTestScene(QPointF(grpBox.rect.left(), grpBox.rect.center().y()), groupLayout, groupDoc, grpSel, 1.0).type ==
          ei::ContainerHitType::ResizeHandleLeft &&
          ei::hitTestScene(QPointF(grpBox.rect.right(), grpBox.rect.center().y()), groupLayout, groupDoc, grpSel, 1.0).type ==
          ei::ContainerHitType::ResizeHandleRight,
          QStringLiteral("Group container retains left and right resize handles"));

    ei::ContainerSelectionState subSel;
    subSel.select(QStringLiteral("subText"));
    check(ei::hitTestScene(QPointF(subBox.rect.left(), subBox.rect.center().y()), groupLayout, groupDoc, subSel, 1.0).type ==
          ei::ContainerHitType::ResizeHandleLeft &&
          ei::hitTestScene(QPointF(subBox.rect.right(), subBox.rect.center().y()), groupLayout, groupDoc, subSel, 1.0).type ==
          ei::ContainerHitType::ResizeHandleRight,
          QStringLiteral("child text container inside Group retains left and right resize handles"));
}

void testRichClipboardAndInspection()
{
    out << "\n=== RichClipboard & Style Inspection ===\n";

    ed::DocumentSnapshot doc;
    ed::ContainerNode textNode;
    textNode.id = QStringLiteral("t1");
    textNode.type = ed::ContainerType::Text;
    textNode.placement = ed::PlacementMode::Flow;
    textNode.text.text = QStringLiteral("TitleLine\nHello World");
    ed::ParagraphInfo p0{{0, 9}, ed::ParagraphRole::Title, 1, {}, {}};
    p0.format.alignment = ed::ParagraphAlignment::Center;
    ed::ParagraphInfo p1{{10, 21}, ed::ParagraphRole::Body, 1, {}, {}};
    p1.format.alignment = ed::ParagraphAlignment::Left;
    p1.format.leftIndent = 18.0;
    textNode.text.paragraphs = {p0, p1};
    ed::TextStyleOverride bigItalic;
    bigItalic.fontSizePt = 24.0;
    bigItalic.italic = true;
    textNode.text.formatSpans = {{{10, 15}, bigItalic}}; // "Hello"

    QImage img(40, 30, QImage::Format_ARGB32);
    img.fill(Qt::red);
    doc.resources.insert(QStringLiteral("res-img"), img);
    ed::ContainerNode imgNode;
    imgNode.id = QStringLiteral("img1");
    imgNode.type = ed::ContainerType::Image;
    imgNode.placement = ed::PlacementMode::Floating;
    imgNode.geometry.position = QPointF(25, 40);
    imgNode.image = {QStringLiteral("res-img"), QSizeF(80, 60)};
    textNode.children.push_back(imgNode);
    doc.root.children.push_back(textNode);

    const auto uniformTitle = ei::inspectSelectionStyle(doc, QStringLiteral("t1"), {0, 9});
    check(uniformTitle.valid && uniformTitle.bold == true &&
          uniformTitle.fontSizePt && std::abs(*uniformTitle.fontSizePt - 19.5) < 0.01 &&
          uniformTitle.italic == false,
          QStringLiteral("inspectSelectionStyle resolves Title default bold and 19.5pt"));

    const auto mixedSpan = ei::inspectSelectionStyle(doc, QStringLiteral("t1"), {10, 21});
    check(mixedSpan.valid && !mixedSpan.fontSizePt.has_value() && !mixedSpan.italic.has_value() &&
          mixedSpan.bold == false && mixedSpan.fontFamily.has_value(),
          QStringLiteral("inspectSelectionStyle returns nullopt for mixed fontSizePt/italic while preserving consistent fields"));

    const auto paraUniform = ei::inspectParagraphSelection(doc, QStringLiteral("t1"), {0, 9});
    check(paraUniform.valid && paraUniform.alignment == ed::ParagraphAlignment::Center &&
          paraUniform.role == ed::ParagraphRole::Title,
          QStringLiteral("inspectParagraphSelection resolves single paragraph alignment and role"));

    const auto paraMixed = ei::inspectParagraphSelection(doc, QStringLiteral("t1"), {2, 14});
    check(paraMixed.valid && !paraMixed.alignment.has_value() && !paraMixed.role.has_value(),
          QStringLiteral("inspectParagraphSelection marks mixed alignment and role across paragraphs"));

    const auto rich = ei::extractRichTextPayload(doc, QStringLiteral("t1"), {0, 15}); // "TitleLine\nHello"
    check(rich.has_value() && rich->plainText == QStringLiteral("TitleLine\nHello") &&
          rich->paragraphs.size() == 2 && rich->paragraphs[0].fullParagraph && !rich->paragraphs[1].fullParagraph,
          QStringLiteral("extractRichTextPayload captures 1-to-1 paragraph meta and fullParagraph flags"));
    const auto encoded = ei::encodeRichTextPayload(*rich);
    const auto decoded = ei::decodeRichTextPayload(encoded);
    check(decoded.has_value() && decoded->plainText == rich->plainText &&
          decoded->formatSpans.size() == 2 &&
          decoded->formatSpans[0].style.bold == true &&
          decoded->formatSpans[1].style.italic == true &&
          std::abs(*decoded->formatSpans[1].style.fontSizePt - 24.0) < 0.01 &&
          decoded->paragraphs[1].format.leftIndent == 18.0,
          QStringLiteral("rich text payload JSON round-trip preserves spans and paragraph metadata"));

    const auto imgPayload = ei::extractImagePayload(doc, QStringLiteral("img1"));
    check(imgPayload.has_value() && imgPayload->sourceResourceId == QStringLiteral("res-img") &&
          imgPayload->placement == ed::PlacementMode::Floating &&
          imgPayload->displaySize == QSizeF(80, 60),
          QStringLiteral("extractImagePayload retrieves image metadata and QImage"));
    const auto imgDecoded = ei::decodeImagePayload(ei::encodeImagePayload(*imgPayload));
    check(imgDecoded.has_value() && imgDecoded->sourceParentId == QStringLiteral("t1") &&
          imgDecoded->geometry.position == QPointF(25, 40) &&
          imgDecoded->image.size() == QSize(40, 30),
          QStringLiteral("image payload JSON+PNG round-trip succeeds"));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    out.setEncoding(QStringConverter::Utf8);

    testHitCases();
    testBoundaries();
    testSelection();
    testEditRequests();
    testIme();
    testToolbarFormatting();
    testController();
    testInteractionRegressions();
    testContainerDragEngine();
    testRichClipboardAndInspection();

    out << "\n===== Interaction self-check: ";
    if (failures == 0)
        out << "ALL PASS =====\n";
    else
        out << failures << " FAIL =====\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}
