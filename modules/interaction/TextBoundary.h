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

} // namespace editor::interaction
