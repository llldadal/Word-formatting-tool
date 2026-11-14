#pragma once

#include "DocxExportTypes.h"
#include "DocumentSnapshot.h"
#include "LayoutResult.h"

#include <QByteArray>
#include <QString>

namespace editor::io {

struct DocxBuildResult {
    DocxExportResult result; // 状态与原因；Ok 时 package 有效。
    QByteArray package;      // 完整 DOCX（ZIP）字节流，可直接写盘或比对。
};

// 只在内存中生成完整 DOCX 包。同一版本的快照与排版结果输出完全一致。
DocxBuildResult buildDocx(const data::DocumentSnapshot& doc,
                          const data::LayoutResult& layout);

// 生成并写出文件。路径由 UI 提供，写盘失败返回 IoError 及原因。
DocxExportResult exportDocx(const data::DocumentSnapshot& doc,
                            const data::LayoutResult& layout,
                            const QString& filePath);

} // 命名空间 editor::io 结束
