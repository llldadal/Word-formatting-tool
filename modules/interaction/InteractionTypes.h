#pragma once

#include "DocumentSnapshot.h"

#include <QRectF>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <vector>

namespace editor::interaction {

// 一种编辑动作类别。交互层只生成请求，从不直接修改文档。
enum class EditOp {
    Insert,             // 在 range.start 处插入文字。
    Delete,             // 删除 range 区间。
    Replace,            // 用 text 替换 range 区间。
    SetCharFormat,      // 把 charStyle 应用到 range 区间。
    SetParagraphFormat  // 把段落设置应用到涉及的整段。
};

// 交给文档模块的「小纸条」。range 为 UTF-16 偏移，相对 containerId 的正文。
struct EditRequest {
    data::ContainerId containerId;
    data::TextRange   range;
    EditOp            op = EditOp::Insert;
    QString           text;              // Insert/Replace 的正文；回车用 "\n" 表示。

    data::TextStyleOverride             charStyle;        // SetCharFormat 使用。
    std::optional<data::ParagraphFormat> paragraphFormat; // SetParagraphFormat 使用。
    std::optional<data::ParagraphRole>   role;
    std::optional<int>                   headingLevel;
};

// 光标位置。offset 相对本容器正文，rect 为纸张坐标。
struct CaretState {
    data::ContainerId containerId;
    int               offset = 0;
    QRectF            rect;
};

// 选区。rects 是逐片段的高亮矩形，图片让出的空隙天然没有高亮。
struct SelectionState {
    data::ContainerId   containerId;
    data::TextRange     range;   // 归一化后的 [start, end)。
    std::vector<QRectF> rects;
};

// 输入法预编辑串。它不写入文档，也不计入偏移。
struct PreeditState {
    QString text;
    int     cursorInPreedit = 0;
    QRectF  rect;                // 候选窗口的锚点，纸张坐标。
};

// 高层编辑动作。UI 把按键、工具栏按钮和菜单项映射到这里。
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
    QString        text;              // InsertText / Paste 的载荷。
    bool           extendSelection = false;
    bool           wholeDocument = false; // Ctrl+Home / Ctrl+End 使用。

    data::TextStyleOverride              charStyle;
    std::optional<data::ParagraphFormat> paragraphFormat;
    std::optional<data::ParagraphRole>   role;
    std::optional<int>                   headingLevel;
};

// UI 在两次事件之间需要保留的全部状态。
struct InteractionState {
    CaretState                     caret;
    std::optional<SelectionState>  selection;
    PreeditState                   preedit;
    std::optional<data::TextStyleOverride> pendingCharStyle; // 折叠光标时工具栏待用的字符格式。
};

// 一次事件的结果。UI 先执行请求，再用新版面调用 refreshStateGeometry。
struct InteractionOutput {
    InteractionState         state;
    std::vector<EditRequest> requests;
    std::optional<QString>   clipboardText; // 由 Copy 和 Cut 写入。
};

// UI 把 Qt 事件转换成这些简单数据结构；窗口到纸张的坐标换算也归 UI。
struct KeyInput {
    int                   key = 0;
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    QString               text;
};

struct ImeInput {
    QString preedit;              // 当前完整预编辑串，不是文档插入内容。
    QString commit;               // 只上屏一次的文字。
    int     preeditCursor     = 0;
    int     replacementStart  = 0; // 相对上一次预编辑串，不构成文档编辑。
    int     replacementLength = 0;
};

} // 命名空间 editor::interaction 结束
