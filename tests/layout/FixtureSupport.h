#pragma once

#include "TestFixtures.h"
#include <QTextCharFormat>

namespace editor::testing::detail {

// 仅供固定标准恢复和检查使用，不是正式排版模块的实现。
const data::ContainerNode& findNode(const data::ContainerNode& root, const QString& id);
data::TextStyle mergeStyle(data::TextStyle base, const data::TextStyleOverride& overrideStyle);
QFont makeFont(const data::TextStyle& style);
QTextCharFormat makeCharacterFormat(const data::TextStyle& style);
std::shared_ptr<QTextLayout> makeParagraphLayout(const data::ContainerNode& node, int paragraphIndex);

} // namespace editor::testing::detail
