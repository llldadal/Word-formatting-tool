#pragma once

#include "InteractionTypes.h"
#include "DocumentSnapshot.h"

#include <QString>

namespace editor::interaction {

// 按容器 ID 找正文。v0.1 只有一层子容器，递归两层足够。
const data::TextContent* findTextContent(const data::DocumentSnapshot& doc,
                                         const data::ContainerId& id);

// 容器正文长度，UTF-16 code unit 数。找不到返回 0。
int textLength(const data::DocumentSnapshot& doc, const data::ContainerId& id);

// 字形簇边界。emoji 代理对、组合字符、ZWJ 序列交给 Qt 处理，
// 调用方不要自己按 code unit 或 UTF-8 字节数加减。
int nextBoundary(const QString& text, int pos);
int prevBoundary(const QString& text, int pos);

// 在指定容器正文上移动，越界自动夹住。
int nextBoundary(const data::DocumentSnapshot& doc, const data::ContainerId& id, int pos);
int prevBoundary(const data::DocumentSnapshot& doc, const data::ContainerId& id, int pos);

// 判断 UTF-16 偏移是否正好是字形簇边界。注意 Qt 的 toPreviousBoundary()
// 在当前位置已经是边界时会继续往前走，不能用「prevBoundary(pos)==pos」判断。
bool isGraphemeBoundary(const QString& text, int pos);

// 把偏移对齐到最近的字形簇边界；本来就在边界上则原样返回。
// 点击命中、输入法替换范围都要用它，避免把一个字从中间切开。
int alignToGrapheme(const QString& text, int pos);

} // 命名空间 editor::interaction 结束
