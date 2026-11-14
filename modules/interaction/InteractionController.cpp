#include "InteractionController.h"

#include "HitTest.h"
#include "Selection.h"
#include "TextBoundary.h"

#include <QKeyEvent>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <limits>
#include <utility>

namespace editor::interaction {
namespace {

QString normalizeNewlines(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        const unsigned int u = static_cast<unsigned int>(c.unicode());
        if (u == 0x0d) {
            out.append(QStringLiteral("\n"));
            if (i + 1 < raw.size()
                && static_cast<unsigned int>(raw.at(i + 1).unicode()) == 0x0a) {
                ++i;
            }
        } else {
            out.append(c);
        }
    }
    return out;
}

// 文档正文只接受换行、制表符和正常字符。C0 控制符、DEL、U+FFFE/U+FFFF
// 以及孤立的 UTF-16 代理项写进 DOCX 会生成非法 XML，统一在这里剔除。
// 键盘直输、粘贴、输入法上屏都走这一个入口，避免某条路径绕过检查。
QString sanitizeForInsert(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const unsigned int u = static_cast<unsigned int>(raw.at(i).unicode());
        if (u == 0x0a || u == 0x09) {
            out.append(raw.at(i));
        } else if (u < 0x20 || u == 0x7f || u == 0xfffe || u == 0xffff) {
            continue;
        } else if (u >= 0xd800 && u <= 0xdbff) {
            // 高代理项必须紧跟一个低代理项，否则丢弃。
            if (i + 1 < raw.size()) {
                const unsigned int low = static_cast<unsigned int>(raw.at(i + 1).unicode());
                if (low >= 0xdc00 && low <= 0xdfff) {
                    out.append(raw.at(i));
                    out.append(raw.at(++i));
                }
            }
        } else if (u >= 0xdc00 && u <= 0xdfff) {
            continue; // 孤立的低代理项。
        } else {
            out.append(raw.at(i));
        }
    }
    return out;
}

bool hasCharStyle(const data::TextStyleOverride& style)
{
    return style.fontFamily.has_value()
        || style.fontSizePt.has_value()
        || style.color.has_value()
        || style.bold.has_value()
        || style.italic.has_value()
        || style.underline.has_value();
}

data::TextStyleOverride mergeCharStyle(const data::TextStyleOverride& base,
                                       const data::TextStyleOverride& add)
{
    data::TextStyleOverride out = base;
    if (add.fontFamily)  out.fontFamily  = add.fontFamily;
    if (add.fontSizePt)  out.fontSizePt  = add.fontSizePt;
    if (add.color)       out.color       = add.color;
    if (add.bold)        out.bold        = add.bold;
    if (add.italic)      out.italic      = add.italic;
    if (add.underline)   out.underline   = add.underline;
    return out;
}

data::ContainerId activeContainer(const InteractionState& state)
{
    return state.selection ? state.selection->containerId : state.caret.containerId;
}

data::TextRange clampTextRange(const data::DocumentSnapshot& doc,
                               const data::ContainerId& id,
                               data::TextRange range)
{
    const int len = textLength(doc, id);
    return normalizeRange(data::TextRange{qBound(0, range.start, len),
                                          qBound(0, range.end, len)});
}

std::optional<data::TextRange> selectionRangeOf(const InteractionState& state,
                                                const data::DocumentSnapshot& doc)
{
    if (!state.selection)
        return std::nullopt;
    const data::TextRange range = clampTextRange(doc, state.selection->containerId,
                                                 state.selection->range);
    if (range.start >= range.end)
        return std::nullopt;
    return range;
}

CaretState makeCaret(const data::LayoutResult& layout,
                     const data::ContainerId& id,
                     int offset)
{
    CaretState caret;
    caret.containerId = id;
    caret.offset = qMax(0, offset);
    caret.rect = caretRect(layout, id, caret.offset);
    return caret;
}

InteractionOutput noOp(const InteractionState& state)
{
    InteractionOutput out;
    out.state = state;
    return out;
}

EditAction simpleAction(EditActionKind kind)
{
    EditAction action;
    action.kind = kind;
    return action;
}

EditAction textAction(EditActionKind kind, const QString& text)
{
    EditAction action = simpleAction(kind);
    action.text = text;
    return action;
}

data::TextRange lineRangeAt(const data::LayoutResult& layout,
                            const data::ContainerId& id,
                            int offset)
{
    const data::TextFragment* containing = nullptr;
    const data::TextFragment* nearest = nullptr;
    int nearestDistance = std::numeric_limits<int>::max();

    for (const auto& f : layout.textFragments) {
        if (f.containerId != id)
            continue;
        if (!containing && offset >= f.sourceRange.start && offset <= f.sourceRange.end)
            containing = &f;
        const int distance = qMin(qAbs(offset - f.sourceRange.start),
                                  qAbs(offset - f.sourceRange.end));
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = &f;
        }
    }
    if (containing)
        return containing->sourceRange;
    if (nearest)
        return nearest->sourceRange;
    return { offset, offset };
}

bool sameRange(data::TextRange a, data::TextRange b)
{
    return a.start == b.start && a.end == b.end;
}

bool rangeOverlapsParagraph(data::TextRange selection, data::TextRange paragraph)
{
    if (paragraph.start == paragraph.end)
        return selection.start <= paragraph.start && paragraph.start <= selection.end;
    return paragraph.start < selection.end && paragraph.end > selection.start;
}

int imeCursorPosition(const ImeInput& ime,
                      const QString& previousPreedit,
                      const QString& currentPreedit)
{
    // 0 是合法的光标位置（预编辑串最前面），只有负数才表示「输入法没给」。
    if (ime.preeditCursor >= 0)
        return qBound(0, ime.preeditCursor, static_cast<int>(currentPreedit.size()));

    int cursor = static_cast<int>(currentPreedit.size());
    if (ime.replacementStart != 0 || ime.replacementLength != 0) {
        int base = ime.replacementStart;
        if (base < 0)
            base = static_cast<int>(previousPreedit.size()) + base;
        cursor = base + ime.replacementLength;
    }
    return qBound(0, cursor, static_cast<int>(currentPreedit.size()));
}

// 在容器正文上把 UTF-16 偏移对齐到最近的字形簇边界，避免把一个 emoji
// 或「字母+组合音标」从中间切成两半。输入法给的长度按 UTF-16 单元计。
int alignInContainer(const data::DocumentSnapshot& doc,
                     const data::ContainerId& id, int pos)
{
    const data::TextContent* content = findTextContent(doc, id);
    if (!content)
        return qMax(0, pos);
    return alignToGrapheme(content->text, pos);
}

// 输入法上屏时要求替换的文档范围。空范围表示「只是插入，不删旧字」。
// 有选区时选区优先，因为用户先拖选再打字，意图是替换选区内容。
data::TextRange imeReplacementRange(const ImeInput& ime,
                                    const InteractionState& state,
                                    const data::DocumentSnapshot& doc)
{
    if (ime.replacementLength <= 0 || state.selection)
        return {};
    const data::ContainerId id = activeContainer(state);
    const int caret = qBound(0, state.caret.offset, textLength(doc, id));
    const int rawStart = caret + ime.replacementStart;
    const int start = alignInContainer(doc, id, rawStart);
    const int end = alignInContainer(doc, id, rawStart + ime.replacementLength);
    return normalizeRange(data::TextRange{start, end});
}

InteractionOutput insertText(const InteractionState& state,
                             const QString& rawText,
                             const data::DocumentSnapshot& doc,
                             const data::LayoutResult& layout,
                             const std::optional<data::TextRange>& forcedRange = std::nullopt)
{
    // 统一入口：键盘、粘贴、输入法上屏都在这里做换行归一化和非法字符清洗。
    const QString text = sanitizeForInsert(normalizeNewlines(rawText));
    InteractionOutput out = noOp(state);
    if (text.isEmpty())
        return out;

    const data::ContainerId id = activeContainer(state);
    const int len = textLength(doc, id);
    int at = qBound(0, state.caret.offset, len);
    const auto selection = selectionRangeOf(state, doc);

    EditRequest request;
    request.containerId = id;
    if (forcedRange) {
        // 输入法要求替换光标附近已有的文字（例如重新选词、纠错）。
        request.op = EditOp::Replace;
        request.range = clampTextRange(doc, id, *forcedRange);
        at = request.range.start;
    } else if (selection) {
        request.op = EditOp::Replace;
        request.range = *selection;
        at = selection->start;
    } else {
        request.op = EditOp::Insert;
        request.range = { at, at };
    }
    request.text = text;
    out.requests.push_back(request);

    const int insertedLength = static_cast<int>(text.size());
    if (state.pendingCharStyle && hasCharStyle(*state.pendingCharStyle)) {
        EditRequest format;
        format.containerId = id;
        format.op = EditOp::SetCharFormat;
        format.range = { at, at + insertedLength };
        format.charStyle = *state.pendingCharStyle;
        out.requests.push_back(format);
    }

    out.state.caret = makeCaret(layout, id, at + insertedLength);
    out.state.selection.reset();
    out.state.selectionAnchor.reset();
    out.state.preedit = PreeditState{};
    out.state.pendingCharStyle.reset();
    out.state.desiredCaretX.reset();
    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput deleteText(const InteractionState& state,
                             bool backspace,
                             const data::DocumentSnapshot& doc,
                             const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    const data::ContainerId id = activeContainer(state);
    const auto selection = selectionRangeOf(state, doc);

    data::TextRange range;
    if (selection) {
        range = *selection;
    } else {
        const int at = qBound(0, state.caret.offset, textLength(doc, id));
        range = backspace
            ? data::TextRange{prevBoundary(doc, id, at), at}
            : data::TextRange{at, nextBoundary(doc, id, at)};
    }
    if (range.start >= range.end)
        return out;

    EditRequest request;
    request.containerId = id;
    request.op = EditOp::Delete;
    request.range = range;
    out.requests.push_back(request);

    out.state.caret = makeCaret(layout, id, range.start);
    out.state.selection.reset();
    out.state.selectionAnchor.reset();
    out.state.preedit = PreeditState{};
    out.state.pendingCharStyle.reset();
    out.state.desiredCaretX.reset();
    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput copyOrCut(const InteractionState& state,
                            bool cut,
                            const data::DocumentSnapshot& doc,
                            const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    const auto selection = selectionRangeOf(state, doc);
    // 没有选区时不能写剪贴板：optional 有值哪怕是空串，UI 也会据此把系统
    // 剪贴板清掉，导致「无选中按 Ctrl+C」把之前复制的内容弄丢。
    if (selection)
        out.clipboardText = selectedText(state, doc);
    if (!cut || !selection)
        return out;

    const data::ContainerId id = activeContainer(state);
    EditRequest request;
    request.containerId = id;
    request.op = EditOp::Delete;
    request.range = *selection;
    out.requests.push_back(request);

    out.state.caret = makeCaret(layout, id, selection->start);
    out.state.selection.reset();
    out.state.selectionAnchor.reset();
    out.state.preedit = PreeditState{};
    out.state.pendingCharStyle.reset();
    out.state.desiredCaretX.reset();
    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput selectAllText(const InteractionState& state,
                                const data::DocumentSnapshot& doc,
                                const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    const data::ContainerId id = activeContainer(state);
    const data::TextRange range = selectAllRange(doc, id);
    out.state.caret = makeCaret(layout, id, range.end);
    out.state.selection = makeSelection(layout, id, range);
    out.state.selectionAnchor = range.start;
    out.state.preedit = PreeditState{};
    out.state.pendingCharStyle.reset();
    out.state.desiredCaretX.reset();
    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

int movedOffset(const EditAction& action,
                const data::DocumentSnapshot& doc,
                const data::LayoutResult& layout,
                const data::ContainerId& id,
                int from,
                qreal desiredX)
{
    const int len = textLength(doc, id);
    const int at = qBound(0, from, len);

    switch (action.kind) {
    case EditActionKind::MoveLeft:
        return action.wholeDocument ? 0 : prevBoundary(doc, id, at);
    case EditActionKind::MoveRight:
        return action.wholeDocument ? len : nextBoundary(doc, id, at);
    case EditActionKind::MoveHome:
        return action.wholeDocument ? 0 : lineRangeAt(layout, id, at).start;
    case EditActionKind::MoveEnd:
        return action.wholeDocument ? len : lineRangeAt(layout, id, at).end;
    case EditActionKind::MoveUp:
    case EditActionKind::MoveDown: {
        QRectF rect = caretRect(layout, id, at);
        if (rect.isEmpty()) {
            for (const auto& f : layout.textFragments) {
                if (f.containerId == id) {
                    rect = f.rect;
                    break;
                }
            }
        }
        const qreal step = rect.height() > 1.0 ? rect.height() : 26.0;
        const qreal dy = action.kind == EditActionKind::MoveDown ? step : -step;
        // 用记住的期望列当目标 x，而不是当前光标的 x；否则连续上下移动时
        // 每行字宽不同，光标会一行一行往左漂。
        const QPointF target(desiredX, rect.center().y() + dy);
        const Hit hit = clampFocus(layout, Hit{id, at}, target);
        return qBound(0, hit.offset, len);
    }
    default:
        return at;
    }
}

bool movesBackward(EditActionKind kind)
{
    return kind == EditActionKind::MoveLeft
        || kind == EditActionKind::MoveUp
        || kind == EditActionKind::MoveHome;
}

InteractionOutput moveCaret(const InteractionState& state,
                            const EditAction& action,
                            const data::DocumentSnapshot& doc,
                            const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    const data::ContainerId id = activeContainer(state);
    int from = state.caret.offset;

    if (state.selection && !action.extendSelection) {
        from = movesBackward(action.kind)
            ? state.selection->range.start
            : state.selection->range.end;
    }

    const bool vertical = action.kind == EditActionKind::MoveUp
                       || action.kind == EditActionKind::MoveDown;
    qreal targetX = state.caret.rect.center().x();
    if (vertical) {
        // 第一次按上下键时记下当前列；连续按上下键时一直沿用这一列。
        targetX = state.desiredCaretX.value_or(caretRect(layout, id, from).center().x());
    }

    const int to = movedOffset(action, doc, layout, id, from, targetX);
    out.state.caret = makeCaret(layout, id, to);
    out.state.caret.containerId = id;

    if (action.extendSelection) {
        // 锚点是选区固定不动的那一端，必须显式保存。仅靠「移动方向的反方向」
        // 去猜端点，在鼠标反向拖选后再按 Shift+方向键时就会把锚点认错。
        const int anchor = (state.selection && state.selectionAnchor)
                         ? *state.selectionAnchor
                         : from;
        if (anchor == to) {
            out.state.selection.reset();
            out.state.selectionAnchor.reset();
        } else {
            out.state.selection = makeSelection(layout, Hit{id, anchor}, Hit{id, to});
            out.state.selectionAnchor = anchor;
        }
    } else {
        out.state.selection.reset();
        out.state.selectionAnchor.reset();
    }

    // 光标位置一变，之前在工具栏设的待用格式就不该继续使用；期望列只在
    // 连续上下移动时保留，其它移动一律清空。
    out.state.pendingCharStyle.reset();
    if (vertical)
        out.state.desiredCaretX = targetX;
    else
        out.state.desiredCaretX.reset();

    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput setCharFormat(const InteractionState& state,
                                const EditAction& action,
                                const data::DocumentSnapshot& doc,
                                const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    if (!hasCharStyle(action.charStyle))
        return out;

    if (!state.preedit.text.isEmpty()) {
        out.state.pendingCharStyle = state.pendingCharStyle
            ? mergeCharStyle(*state.pendingCharStyle, action.charStyle)
            : action.charStyle;
        out.state = refreshStateGeometry(out.state, layout);
        return out;
    }

    const auto selection = selectionRangeOf(state, doc);
    if (selection) {
        EditRequest request;
        request.containerId = activeContainer(state);
        request.op = EditOp::SetCharFormat;
        request.range = *selection;
        request.charStyle = action.charStyle;
        out.requests.push_back(request);
        out.state.pendingCharStyle.reset();
    } else {
        out.state.pendingCharStyle = state.pendingCharStyle
            ? mergeCharStyle(*state.pendingCharStyle, action.charStyle)
            : action.charStyle;
    }

    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput setParagraphFormat(const InteractionState& state,
                                     const EditAction& action,
                                     const data::DocumentSnapshot& doc,
                                     const data::LayoutResult& layout)
{
    InteractionOutput out = noOp(state);
    if (!action.paragraphFormat && !action.role && !action.headingLevel)
        return out;

    const data::ContainerId id = activeContainer(state);
    const auto selection = selectionRangeOf(state, doc);
    const data::TextRange focus = selection
        ? *selection
        : data::TextRange{state.caret.offset, state.caret.offset};
    const auto ranges = paragraphRangesFor(doc, id, focus);

    std::optional<int> headingLevel = action.headingLevel;
    if (action.role && *action.role == data::ParagraphRole::Heading)
        headingLevel = qBound(1, headingLevel.value_or(1), 9);
    else if (action.role)
        headingLevel.reset();
    else if (headingLevel)
        headingLevel = qBound(1, *headingLevel, 9);

    for (const auto& range : ranges) {
        EditRequest request;
        request.containerId = id;
        request.op = EditOp::SetParagraphFormat;
        request.range = range;
        request.paragraphFormat = action.paragraphFormat;
        request.role = action.role;
        request.headingLevel = headingLevel;
        out.requests.push_back(request);
    }

    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

} // 匿名命名空间结束

data::TextRange paragraphRangeAt(const data::DocumentSnapshot& doc,
                                 const data::ContainerId& id,
                                 int offset)
{
    const data::TextContent* content = findTextContent(doc, id);
    if (!content)
        return { 0, 0 };

    const int len = static_cast<int>(content->text.size());
    const int at = qBound(0, offset, len);
    for (const auto& paragraph : content->paragraphs) {
        if (paragraph.range.start > paragraph.range.end)
            continue;
        if (at >= paragraph.range.start && at <= paragraph.range.end) {
            const int start = qBound(0, paragraph.range.start, len);
            const int end = qBound(start, paragraph.range.end, len);
            return { start, end };
        }
    }

    int start = 0;
    if (at > 0)
        start = content->text.lastIndexOf(QStringLiteral("\n"), at - 1) + 1;
    int end = content->text.indexOf(QStringLiteral("\n"), at);
    if (end < 0)
        end = len;
    if (end < start)
        end = start;
    return { start, end };
}

std::vector<data::TextRange> paragraphRangesFor(const data::DocumentSnapshot& doc,
                                                const data::ContainerId& id,
                                                data::TextRange range)
{
    const data::TextContent* content = findTextContent(doc, id);
    if (!content)
        return {};

    const data::TextRange clamped = clampTextRange(doc, id, range);
    if (clamped.start >= clamped.end)
        return { paragraphRangeAt(doc, id, clamped.start) };

    std::vector<data::TextRange> result;
    const int len = static_cast<int>(content->text.size());
    for (const auto& paragraph : content->paragraphs) {
        if (paragraph.range.start > paragraph.range.end)
            continue;
        const data::TextRange paragraphRange{
            qBound(0, paragraph.range.start, len),
            qBound(0, paragraph.range.end, len)
        };
        if (!rangeOverlapsParagraph(clamped, paragraphRange))
            continue;
        if (result.empty() || !sameRange(result.back(), paragraphRange))
            result.push_back(paragraphRange);
    }
    if (!result.empty())
        return result;

    int cursor = clamped.start;
    // 不再用固定的 128 段上限截断：超过 128 段时后面的段落会收不到格式请求。
    // 循环仍保证每次 cursor 至少前进 1，空段落也不会造成死循环。
    while (cursor <= clamped.end) {
        const data::TextRange current = paragraphRangeAt(doc, id, cursor);
        result.push_back(current);
        if (current.end >= clamped.end)
            break;
        const int next = qMin(clamped.end, current.end + 1);
        if (next <= cursor)
            break;
        cursor = next;
    }
    return result;
}

QString selectedText(const InteractionState& state, const data::DocumentSnapshot& doc)
{
    const auto selection = selectionRangeOf(state, doc);
    if (!selection)
        return QString();
    const data::TextContent* content = findTextContent(doc, activeContainer(state));
    if (!content)
        return QString();
    return content->text.mid(selection->start, selection->end - selection->start);
}

InteractionState refreshStateGeometry(const InteractionState& state,
                                      const data::LayoutResult& layout)
{
    InteractionState out = state;
    out.caret.rect = caretRect(layout, out.caret.containerId, out.caret.offset);
    if (out.selection) {
        out.selection = makeSelection(layout, out.selection->containerId,
                                      out.selection->range);
    }
    if (!out.preedit.text.isEmpty()) {
        out.preedit.rect = caretRect(layout, out.caret.containerId, out.caret.offset);
    } else {
        out.preedit.rect = QRectF();
    }
    return out;
}

InteractionState makeInitialState(const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::ContainerId& id,
                                  int offset)
{
    InteractionState state;
    state.caret = makeCaret(layout, id, qBound(0, offset, textLength(doc, id)));
    state.caret.containerId = id;
    return refreshStateGeometry(state, layout);
}

InteractionOutput applyAction(const InteractionState& state,
                              const EditAction& action,
                              const data::DocumentSnapshot& doc,
                              const data::LayoutResult& layout)
{
    const bool preeditActive = !state.preedit.text.isEmpty();
    if (preeditActive
        && action.kind != EditActionKind::CancelPreedit
        && action.kind != EditActionKind::SetCharFormat) {
        return noOp(state);
    }

    switch (action.kind) {
    case EditActionKind::InsertText:
    case EditActionKind::Paste:
        return insertText(state, action.text, doc, layout);
    case EditActionKind::Enter:
        return insertText(state, QStringLiteral("\n"), doc, layout);
    case EditActionKind::Backspace:
        return deleteText(state, true, doc, layout);
    case EditActionKind::Delete:
        return deleteText(state, false, doc, layout);
    case EditActionKind::Copy:
        return copyOrCut(state, false, doc, layout);
    case EditActionKind::Cut:
        return copyOrCut(state, true, doc, layout);
    case EditActionKind::SelectAll:
        return selectAllText(state, doc, layout);
    case EditActionKind::MoveLeft:
    case EditActionKind::MoveRight:
    case EditActionKind::MoveUp:
    case EditActionKind::MoveDown:
    case EditActionKind::MoveHome:
    case EditActionKind::MoveEnd:
        return moveCaret(state, action, doc, layout);
    case EditActionKind::SetCharFormat:
        return setCharFormat(state, action, doc, layout);
    case EditActionKind::SetParagraphFormat:
        return setParagraphFormat(state, action, doc, layout);
    case EditActionKind::CancelPreedit: {
        InteractionOutput out = noOp(state);
        out.state.preedit = PreeditState{};
        out.state = refreshStateGeometry(out.state, layout);
        return out;
    }
    }
    return noOp(state);
}

InteractionOutput applyKey(const InteractionState& state,
                           const KeyInput& key,
                           const data::DocumentSnapshot& doc,
                           const data::LayoutResult& layout,
                           const QString& clipboardText)
{
    const bool control = key.mods.testFlag(Qt::ControlModifier);
    const bool shift = key.mods.testFlag(Qt::ShiftModifier);
    const bool alt = key.mods.testFlag(Qt::AltModifier);
    const bool meta = key.mods.testFlag(Qt::MetaModifier);
    if (alt || meta)
        return noOp(state);

    EditAction action;
    action.extendSelection = shift;

    if (control) {
        switch (key.key) {
        case Qt::Key_A:
            action.kind = EditActionKind::SelectAll;
            return applyAction(state, action, doc, layout);
        case Qt::Key_C:
            action.kind = EditActionKind::Copy;
            return applyAction(state, action, doc, layout);
        case Qt::Key_X:
            action.kind = EditActionKind::Cut;
            return applyAction(state, action, doc, layout);
        case Qt::Key_V:
            action.kind = EditActionKind::Paste;
            action.text = normalizeNewlines(clipboardText);
            return applyAction(state, action, doc, layout);
        case Qt::Key_Insert:
            action.kind = EditActionKind::Copy;
            return applyAction(state, action, doc, layout);
        case Qt::Key_B:
            action.kind = EditActionKind::SetCharFormat;
            action.charStyle.bold = true;
            return applyAction(state, action, doc, layout);
        case Qt::Key_I:
            action.kind = EditActionKind::SetCharFormat;
            action.charStyle.italic = true;
            return applyAction(state, action, doc, layout);
        case Qt::Key_U:
            action.kind = EditActionKind::SetCharFormat;
            action.charStyle.underline = true;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Home:
            action.kind = EditActionKind::MoveHome;
            action.wholeDocument = true;
            return applyAction(state, action, doc, layout);
        case Qt::Key_End:
            action.kind = EditActionKind::MoveEnd;
            action.wholeDocument = true;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Left:
            action.kind = EditActionKind::MoveLeft;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Right:
            action.kind = EditActionKind::MoveRight;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Up:
            action.kind = EditActionKind::MoveUp;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Down:
            action.kind = EditActionKind::MoveDown;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Backspace:
            action.kind = EditActionKind::Backspace;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Delete:
            action.kind = EditActionKind::Delete;
            return applyAction(state, action, doc, layout);
        case Qt::Key_Return:
        case Qt::Key_Enter:
            action.kind = EditActionKind::Enter;
            return applyAction(state, action, doc, layout);
        default:
            return noOp(state);
        }
    }

    if (shift && key.key == Qt::Key_Insert) {
        action.kind = EditActionKind::Paste;
        action.text = normalizeNewlines(clipboardText);
        return applyAction(state, action, doc, layout);
    }

    switch (key.key) {
    case Qt::Key_Backspace:
        action.kind = EditActionKind::Backspace;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Delete:
        action.kind = EditActionKind::Delete;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Return:
    case Qt::Key_Enter:
        action.kind = EditActionKind::Enter;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Left:
        action.kind = EditActionKind::MoveLeft;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Right:
        action.kind = EditActionKind::MoveRight;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Up:
    case Qt::Key_PageUp:
        action.kind = EditActionKind::MoveUp;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Down:
    case Qt::Key_PageDown:
        action.kind = EditActionKind::MoveDown;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Home:
        action.kind = EditActionKind::MoveHome;
        return applyAction(state, action, doc, layout);
    case Qt::Key_End:
        action.kind = EditActionKind::MoveEnd;
        return applyAction(state, action, doc, layout);
    case Qt::Key_Escape:
        action.kind = EditActionKind::CancelPreedit;
        return applyAction(state, action, doc, layout);
    default:
        break;
    }

    QString typed = key.text;
    if (typed.isEmpty() && key.key == Qt::Key_Space)
        typed = QStringLiteral(" ");
    if (typed.isEmpty() && key.key == Qt::Key_Tab)
        typed = QStringLiteral("\t");
    // 键盘直输也走和粘贴、输入法上屏相同的清洗，避免只有一条路径被检查。
    typed = sanitizeForInsert(normalizeNewlines(typed));
    if (typed.isEmpty())
        return noOp(state);

    action.kind = EditActionKind::InsertText;
    action.text = typed;
    return applyAction(state, action, doc, layout);
}

InteractionOutput applyPaste(const InteractionState& state,
                             const QString& text,
                             const data::DocumentSnapshot& doc,
                             const data::LayoutResult& layout)
{
    return applyAction(state, textAction(EditActionKind::Paste, text), doc, layout);
}

InteractionOutput applyIme(const InteractionState& state,
                           const ImeInput& ime,
                           const data::DocumentSnapshot& doc,
                           const data::LayoutResult& layout)
{
    const QString commit = sanitizeForInsert(normalizeNewlines(ime.commit));
    const QString preedit = normalizeNewlines(ime.preedit);
    const QString previousPreedit = state.preedit.text;

    InteractionOutput out = noOp(state);
    if (!commit.isEmpty()) {
        // 输入法可能要求用上屏文字替换掉光标附近已有的字（重选词、纠错）。
        // 忽略 replacementStart/Length 会把这类事件当成纯插入，文档里留下旧字。
        const data::TextRange replacement = imeReplacementRange(ime, state, doc);
        if (replacement.start < replacement.end)
            out = insertText(state, commit, doc, layout, replacement);
        else
            out = insertText(state, commit, doc, layout);
    }

    if (!preedit.isEmpty()) {
        PreeditState next;
        next.text = preedit;
        next.cursorInPreedit = imeCursorPosition(ime, previousPreedit, preedit);
        next.rect = caretRect(layout, out.state.caret.containerId,
                              out.state.caret.offset);
        out.state.preedit = next;
    } else {
        out.state.preedit = PreeditState{};
    }

    out.state = refreshStateGeometry(out.state, layout);
    return out;
}

InteractionOutput applyCharFormat(const InteractionState& state,
                                  const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::TextStyleOverride& style)
{
    EditAction action = simpleAction(EditActionKind::SetCharFormat);
    action.charStyle = style;
    return applyAction(state, action, doc, layout);
}

InteractionOutput applyParagraphFormat(const InteractionState& state,
                                       const data::DocumentSnapshot& doc,
                                       const data::LayoutResult& layout,
                                       std::optional<data::ParagraphFormat> format,
                                       std::optional<data::ParagraphRole> role,
                                       std::optional<int> headingLevel)
{
    EditAction action = simpleAction(EditActionKind::SetParagraphFormat);
    action.paragraphFormat = format;
    action.role = role;
    action.headingLevel = headingLevel;
    return applyAction(state, action, doc, layout);
}

InteractionController::InteractionController(const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout,
                                             const data::ContainerId& id,
                                             int offset)
{
    reset(doc, layout, id, offset);
}

void InteractionController::reset(const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::ContainerId& id,
                                  int offset)
{
    state_ = makeInitialState(doc, layout, id, offset);
}

void InteractionController::setState(const InteractionState& state)
{
    state_ = state;
}

const InteractionState& InteractionController::state() const
{
    return state_;
}

InteractionOutput InteractionController::accept(InteractionOutput output)
{
    state_ = output.state;
    return output;
}

InteractionOutput InteractionController::action(const EditAction& action,
                                                const data::DocumentSnapshot& doc,
                                                const data::LayoutResult& layout)
{
    return accept(applyAction(state_, action, doc, layout));
}

InteractionOutput InteractionController::key(const KeyInput& key,
                                             const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout,
                                             const QString& clipboardText)
{
    return accept(applyKey(state_, key, doc, layout, clipboardText));
}

InteractionOutput InteractionController::paste(const QString& text,
                                               const data::DocumentSnapshot& doc,
                                               const data::LayoutResult& layout)
{
    return accept(applyPaste(state_, text, doc, layout));
}

InteractionOutput InteractionController::ime(const ImeInput& ime,
                                             const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout)
{
    return accept(applyIme(state_, ime, doc, layout));
}

InteractionOutput InteractionController::charFormat(const data::DocumentSnapshot& doc,
                                                    const data::LayoutResult& layout,
                                                    const data::TextStyleOverride& style)
{
    return accept(applyCharFormat(state_, doc, layout, style));
}

InteractionOutput InteractionController::paragraphFormat(
    const data::DocumentSnapshot& doc,
    const data::LayoutResult& layout,
    std::optional<data::ParagraphFormat> format,
    std::optional<data::ParagraphRole> role,
    std::optional<int> headingLevel)
{
    return accept(applyParagraphFormat(state_, doc, layout, format, role, headingLevel));
}

InteractionOutput InteractionController::copy(const data::DocumentSnapshot& doc,
                                              const data::LayoutResult& layout)
{
    return accept(applyAction(state_, simpleAction(EditActionKind::Copy), doc, layout));
}

InteractionOutput InteractionController::cut(const data::DocumentSnapshot& doc,
                                             const data::LayoutResult& layout)
{
    return accept(applyAction(state_, simpleAction(EditActionKind::Cut), doc, layout));
}

InteractionOutput InteractionController::selectAll(const data::DocumentSnapshot& doc,
                                                   const data::LayoutResult& layout)
{
    return accept(applyAction(state_, simpleAction(EditActionKind::SelectAll), doc, layout));
}

InteractionOutput InteractionController::cancelPreedit(const data::DocumentSnapshot& doc,
                                                       const data::LayoutResult& layout)
{
    return accept(applyAction(state_, simpleAction(EditActionKind::CancelPreedit),
                              doc, layout));
}

InteractionOutput InteractionController::refresh(const data::LayoutResult& layout)
{
    InteractionOutput out;
    out.state = refreshStateGeometry(state_, layout);
    state_ = out.state;
    return out;
}

} // 命名空间 editor::interaction 结束
