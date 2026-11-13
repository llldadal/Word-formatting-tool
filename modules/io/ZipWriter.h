#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <vector>

namespace editor::io::zip {

// 一个待写入包内的文件条目。内容不压缩（store 模式），Word 可以直接打开。
struct ZipEntry {
    QString name;      // 包内路径，使用正斜杠。
    QByteArray content;
};

// 生成完整的 ZIP 字节流：本地文件头、中央目录和结尾记录。
// 时间戳固定，同样的输入产生完全一致的文件，便于测试比对。
QByteArray buildStoreArchive(const std::vector<ZipEntry>& entries);

// 只读校验：按结尾记录遍历中央目录，核对每个条目的名字、大小和 CRC。
// 返回空列表表示包结构有效；每条问题一行说明。
QStringList verifyArchive(const QByteArray& bytes);

// 取出指定条目的原始内容。成功返回 true 并填充 out；失败返回 false 并写入 issues。
bool extractEntry(const QByteArray& bytes, const QString& name,
                  QByteArray* out, QStringList* issues);

} // 命名空间 editor::io::zip 结束
