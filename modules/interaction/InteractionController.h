#pragma once

#include "InteractionTypes.h"
#include "DocumentSnapshot.h"
#include "LayoutResult.h"

#include <QString>
#include <optional>
#include <vector>

namespace editor::interaction {

// Build a collapsed-caret state at a legal UTF-16 offset.
InteractionState makeInitialState(const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::ContainerId& id,
                                  int offset = 0);

// Recompute caret, selection, and preedit rectangles after a new layout arrives.
InteractionState refreshStateGeometry(const InteractionState& state,
                                      const data::LayoutResult& layout);

// Paragraph range containing offset. The range excludes the trailing newline.
data::TextRange paragraphRangeAt(const data::DocumentSnapshot& doc,
                                 const data::ContainerId& id,
                                 int offset);

// Full paragraph ranges overlapped by range. Used for paragraph-level format requests.
std::vector<data::TextRange> paragraphRangesFor(const data::DocumentSnapshot& doc,
                                                const data::ContainerId& id,
                                                data::TextRange range);

// Current selected text, or an empty string when there is no non-empty selection.
QString selectedText(const InteractionState& state, const data::DocumentSnapshot& doc);

// Pure event reducers. They do not mutate the document or the layout.
InteractionOutput applyAction(const InteractionState& state,
                              const EditAction& action,
                              const data::DocumentSnapshot& doc,
                              const data::LayoutResult& layout);

InteractionOutput applyKey(const InteractionState& state,
                           const KeyInput& key,
                           const data::DocumentSnapshot& doc,
                           const data::LayoutResult& layout,
                           const QString& clipboardText = QString());

InteractionOutput applyPaste(const InteractionState& state,
                             const QString& text,
                             const data::DocumentSnapshot& doc,
                             const data::LayoutResult& layout);

InteractionOutput applyIme(const InteractionState& state,
                           const ImeInput& ime,
                           const data::DocumentSnapshot& doc,
                           const data::LayoutResult& layout);

InteractionOutput applyCharFormat(const InteractionState& state,
                                  const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::TextStyleOverride& style);

InteractionOutput applyParagraphFormat(const InteractionState& state,
                                       const data::DocumentSnapshot& doc,
                                       const data::LayoutResult& layout,
                                       std::optional<data::ParagraphFormat> format = std::nullopt,
                                       std::optional<data::ParagraphRole> role = std::nullopt,
                                       std::optional<int> headingLevel = std::nullopt);

// Thin stateful wrapper for the main window. It owns no document or layout.
class InteractionController {
public:
    InteractionController() = default;
    InteractionController(const data::DocumentSnapshot& doc,
                          const data::LayoutResult& layout,
                          const data::ContainerId& id,
                          int offset = 0);

    void reset(const data::DocumentSnapshot& doc,
               const data::LayoutResult& layout,
               const data::ContainerId& id,
               int offset = 0);
    void setState(const InteractionState& state);
    const InteractionState& state() const;

    InteractionOutput action(const EditAction& action,
                             const data::DocumentSnapshot& doc,
                             const data::LayoutResult& layout);
    InteractionOutput key(const KeyInput& key,
                          const data::DocumentSnapshot& doc,
                          const data::LayoutResult& layout,
                          const QString& clipboardText = QString());
    InteractionOutput paste(const QString& text,
                            const data::DocumentSnapshot& doc,
                            const data::LayoutResult& layout);
    InteractionOutput ime(const ImeInput& ime,
                          const data::DocumentSnapshot& doc,
                          const data::LayoutResult& layout);
    InteractionOutput charFormat(const data::DocumentSnapshot& doc,
                                 const data::LayoutResult& layout,
                                 const data::TextStyleOverride& style);
    InteractionOutput paragraphFormat(const data::DocumentSnapshot& doc,
                                      const data::LayoutResult& layout,
                                      std::optional<data::ParagraphFormat> format = std::nullopt,
                                      std::optional<data::ParagraphRole> role = std::nullopt,
                                      std::optional<int> headingLevel = std::nullopt);

    InteractionOutput copy(const data::DocumentSnapshot& doc,
                           const data::LayoutResult& layout);
    InteractionOutput cut(const data::DocumentSnapshot& doc,
                          const data::LayoutResult& layout);
    InteractionOutput selectAll(const data::DocumentSnapshot& doc,
                                const data::LayoutResult& layout);
    InteractionOutput cancelPreedit(const data::DocumentSnapshot& doc,
                                    const data::LayoutResult& layout);
    InteractionOutput refresh(const data::LayoutResult& layout);

private:
    InteractionOutput accept(InteractionOutput output);

    InteractionState state_;
};

} // namespace editor::interaction
