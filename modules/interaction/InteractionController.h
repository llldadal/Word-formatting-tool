#pragma once

#include "InteractionTypes.h"
#include "DocumentSnapshot.h"
#include "LayoutResult.h"

#include <QString>
#include <optional>
#include <vector>

namespace editor::interaction {

// 在合法的 UTF-16 偏移处构造折叠光标状态。
InteractionState makeInitialState(const data::DocumentSnapshot& doc,
                                  const data::LayoutResult& layout,
                                  const data::ContainerId& id,
                                  int offset = 0);

// 新版面到达后重算光标、选区与预编辑矩形。
InteractionState refreshStateGeometry(const InteractionState& state,
                                      const data::LayoutResult& layout);

// 包含 offset 的整段范围，不含段末换行符。
data::TextRange paragraphRangeAt(const data::DocumentSnapshot& doc,
                                 const data::ContainerId& id,
                                 int offset);

// 与 range 相交的所有整段范围，供段落级格式请求使用。
std::vector<data::TextRange> paragraphRangesFor(const data::DocumentSnapshot& doc,
                                                const data::ContainerId& id,
                                                data::TextRange range);

// 当前选中文字；没有非空选区时返回空串。
QString selectedText(const InteractionState& state, const data::DocumentSnapshot& doc);

// 纯事件归约函数，不修改文档，也不修改排版结果。
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

// 主窗口用的轻量有状态封装，自身不持有文档或版面。
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

} // 命名空间 editor::interaction 结束
