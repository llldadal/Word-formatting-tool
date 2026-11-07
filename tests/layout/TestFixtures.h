#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"
#include <QStringList>

namespace editor::testing {

// 一道测试题。输入在 Cases.cpp 中用 C++ 建树，标准结果来自固定数值记录。
struct LayoutCase {
    QString id;                      // 稳定题号，05 双栏题在 v0.1 中不启用。
    QString title;                   // 题目中文名。
    data::DocumentSnapshot document; // 交给排版模块的完整只读输入，包含图片资源。
};

// 建立全部固定输入。可以独立用于文档、绘图、交互和导出的开发。
std::vector<LayoutCase> makeLayoutCases();

// 取得某一道固定输入。未知题号抛出 std::invalid_argument。
LayoutCase makeLayoutCase(const QString& caseId);

// 恢复标准排版。只按 StandardRecords.inc 中的数值恢复 Qt 行，不排容器树。
// 字体或 Qt 排字行为与标准不兼容时抛出异常，不能据此认定排版代码失败。
data::LayoutResult makeStandardLayout(const QString& caseId);

// 比较结构、坐标、字符范围、格式和行对象。忽略 vector 顺序及布局池索引编号。
QStringList compareLayout(const data::DocumentSnapshot& source,
                          const data::LayoutResult& expected,
                          const data::LayoutResult& actual,
                          qreal coordinateTolerance = 0.5);

// 检查测试输入与标准本身是否满足公共约定，不能代替文档模块的增删改查测试。
QStringList validateSnapshot(const data::DocumentSnapshot& source);

// 独立检查布局与源树的一致性，包括段落覆盖、盒模型、障碍避让和完整范围。
QStringList validateLayoutStructure(const data::DocumentSnapshot& source,
                                    const data::LayoutResult& result,
                                    qreal coordinateTolerance = 0.5);

} // namespace editor::testing
