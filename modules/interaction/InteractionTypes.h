#pragma once

#include "DocumentSnapshot.h"

#include <QRectF>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <vector>

namespace editor::interaction {

// One edit action category. Interaction creates requests; it never mutates the document.
enum class EditOp {
    Insert,             // Insert text at range.start.
    Delete,             // Delete range.
    Replace,            // Replace range with text.
    SetCharFormat,      // Apply charStyle to range.
    SetParagraphFormat  // Apply paragraph settings to whole paragraphs.
};

// The "small note" handed to the document module. range is UTF-16 and relative to containerId.
struct EditRequest {
    data::ContainerId containerId;
    data::TextRange   range;
    EditOp            op = EditOp::Insert;
    QString           text;              // Insert/Replace body; Enter uses "\n".

    data::TextStyleOverride             charStyle;        // SetCharFormat.
    std::optional<data::ParagraphFormat> paragraphFormat; // SetParagraphFormat.
    std::optional<data::ParagraphRole>   role;
    std::optional<int>                   headingLevel;
};

// Caret position. offset is relative to this container's text; rect is in paper coordinates.
struct CaretState {
    data::ContainerId containerId;
    int               offset = 0;
    QRectF            rect;
};

// Selection. rects are per-fragment highlight rectangles; image gaps naturally have none.
struct SelectionState {
    data::ContainerId   containerId;
    data::TextRange     range;   // Normalized [start, end).
    std::vector<QRectF> rects;
};

// IME preedit string. It is never written to the document and never counts toward offsets.
struct PreeditState {
    QString text;
    int     cursorInPreedit = 0;
    QRectF  rect;                // Candidate-window anchor in paper coordinates.
};

// High-level editor actions. UI maps keys, toolbar buttons, and menu items to these.
enum class EditActionKind {
    InsertText,
    Paste,
    Enter,
    Backspace,
    Delete,
    Copy,
    Cut,
    SelectAll,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveHome,
    MoveEnd,
    SetCharFormat,
    SetParagraphFormat,
    CancelPreedit
};

struct EditAction {
    EditActionKind kind = EditActionKind::InsertText;
    QString        text;              // InsertText / Paste payload.
    bool           extendSelection = false;
    bool           wholeDocument = false; // Ctrl+Home / Ctrl+End.

    data::TextStyleOverride              charStyle;
    std::optional<data::ParagraphFormat> paragraphFormat;
    std::optional<data::ParagraphRole>   role;
    std::optional<int>                   headingLevel;
};

// Everything the UI keeps between two events.
struct InteractionState {
    CaretState                     caret;
    std::optional<SelectionState>  selection;
    PreeditState                   preedit;
    std::optional<data::TextStyleOverride> pendingCharStyle; // Collapsed-caret toolbar choice.
};

// Result of one event. UI executes requests, then calls refreshStateGeometry with the new layout.
struct InteractionOutput {
    InteractionState         state;
    std::vector<EditRequest> requests;
    std::optional<QString>   clipboardText; // Set by Copy and Cut.
};

// UI converts Qt events into these PODs. Window-to-paper conversion belongs to UI.
struct KeyInput {
    int                   key = 0;
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    QString               text;
};

struct ImeInput {
    QString preedit;              // Full current preedit string, not a document insertion.
    QString commit;               // Text to commit exactly once.
    int     preeditCursor     = 0;
    int     replacementStart  = 0; // Relative to the previous preedit string; never a document edit.
    int     replacementLength = 0;
};

} // namespace editor::interaction
