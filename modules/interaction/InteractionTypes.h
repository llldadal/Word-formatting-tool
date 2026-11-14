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
    // 选区固定不动的那一端（相对 selection->containerId）。Shift+方向键扩展
    // 选区时必须用它当锚点；只按移动方向去猜端点，反向拖选后会认错锚点。
    std::optional<int> selectionAnchor;
    // 连续按上下键时记住最初那一列的纸张 x。每行字宽不同，如果每次都用
    // 当前光标的 x 当目标，光标会一行一行往左漂。左右移动、编辑、鼠标点击后清空。
    std::optional<qreal> desiredCaretX;
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
    // 预编辑串内的光标位置。0 是合法值（光标在最前面），所以负数才表示
    // 输入法没有给出光标属性，需要按替换范围或串尾推导。
    int     preeditCursor     = -1;
    // 这两个字段描述「上屏时要替换掉光标附近的多少文字」，基准点是本次
    // 预编辑串的起点，也就是文档里的当前光标。start 为负表示往光标左侧数。
    // 本模块的预编辑不写入文档，所以 commit 时必须据此生成 Replace 请求，
    // 否则输入法要求的重选词、纠错替换会被当成纯插入，文档里多出旧字。
    int     replacementStart  = 0;
    int     replacementLength = 0;
};

} // 命名空间 editor::interaction 结束
