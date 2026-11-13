// Interaction module self-check.
// Uses only the repository fixtures and standard layouts; no other team module is linked.
// Covers I01 / I02 / I03 / I04 / I05 / I06 / I07 / I08 and U05 interaction-side behavior.

#include "HitTest.h"
#include "InteractionController.h"
#include "Selection.h"
#include "TextBoundary.h"
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

// I01 / I02 / I05: click positions, container ownership, wrap-around fragments, blank paper.
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

// I08: grapheme boundaries. Emoji surrogate pairs, combining marks, and empty paragraphs.
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

// I04 / I05: normalized ranges, split selection rectangles, Ctrl+A, clamped drag, caret rect.
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

// I03 / I06 / I08: typing, paste, Enter, Backspace, Delete, copy, cut, shortcuts, movement.
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

// I07: preedit never enters the document; commit inserts exactly once; candidate rect follows caret.
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
    check(cursorOnly.requests.empty(), QStringLiteral("replacement attributes do not edit document"));
    check(cursorOnly.state.preedit.cursorInPreedit == 5,
          QStringLiteral("replacement attributes compute preedit cursor at 5"));
}

// U05: toolbar character formats and paragraph roles become ordered EditRequest objects.
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
    check(bolded.state.selection.has_value() && !bolded.state.pendingCharStyle.has_value(),
          QStringLiteral("selected format keeps selection and clears pending style"));

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
    check(!inserted.state.pendingCharStyle.has_value(),
          QStringLiteral("pending style is consumed after insertion"));

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

// Stateful wrapper keeps its state and refreshes geometry from a later layout.
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

    out << "\n===== Interaction self-check: ";
    if (failures == 0)
        out << "ALL PASS =====\n";
    else
        out << failures << " FAIL =====\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}
