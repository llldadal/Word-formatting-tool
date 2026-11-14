#pragma once

#include <QString>

namespace editor::io {

enum class DocxExportStatus {
    Ok,                 // 成功生成或写出。
    CompatibilityError, // 输入超出 v0.1 的 DOCX 映射范围，containerId 指出涉及的容器。
    IoError             // 数据不一致或文件系统错误。
};

struct DocxExportResult {
    DocxExportStatus status = DocxExportStatus::Ok;
    QString errorMessage; // 失败原因，写给人看的说明。
    QString containerId;  // 兼容性错误涉及的容器 ID，其余情况为空。
    bool ok() const { return status == DocxExportStatus::Ok; }
};

} // 命名空间 editor::io 结束
