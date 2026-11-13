#include "ZipWriter.h"

#include <QtGlobal>

#include <array>

namespace editor::io::zip {
namespace {

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> generated{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            generated[i] = c;
        }
        return generated;
    }();
    return table;
}

quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : data)
        crc = crcTable()[(crc ^ static_cast<quint8>(c)) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void put16(QByteArray* out, quint16 v)
{
    out->append(char(quint8(v)));
    out->append(char(quint8(v >> 8)));
}

void put32(QByteArray* out, quint32 v)
{
    put16(out, quint16(v & 0xFFFFu));
    put16(out, quint16(v >> 16));
}

quint16 get16(const QByteArray& b, qsizetype pos)
{
    return quint16(quint8(b.at(pos)) | (quint8(b.at(pos + 1)) << 8));
}

quint32 get32(const QByteArray& b, qsizetype pos)
{
    return quint32(get16(b, pos)) | (quint32(get16(b, pos + 2)) << 16);
}

// 固定时间戳 2026-01-01 00:00，保证同样输入生成同样的字节流。
constexpr quint16 kDosTime = 0;
constexpr quint16 kDosDate = quint16(((2026 - 1980) << 9) | (1 << 5) | 1);

constexpr quint32 kLocalHeaderSig = 0x04034b50;
constexpr quint32 kCentralDirSig = 0x02014b50;
constexpr quint32 kEndOfDirSig = 0x06054b50;
constexpr quint16 kUtf8Flag = 0x0800;

struct CentralRecord {
    QString name;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localHeaderOffset = 0;
};

// 遍历中央目录。EOCD 有问题时返回 false 并写入 issues。
bool walkCentral(const QByteArray& bytes, std::vector<CentralRecord>* records,
                 QStringList* issues)
{
    const qsizetype scanFrom = qMax<qsizetype>(0, bytes.size() - 65558);
    qsizetype eocd = -1;
    for (qsizetype i = bytes.size() - 22; i >= scanFrom; --i) {
        if (i < 0) break;
        if (get32(bytes, i) == kEndOfDirSig) { eocd = i; break; }
    }
    if (eocd < 0) {
        *issues << QStringLiteral("EOCD signature not found");
        return false;
    }
    const quint16 count = get16(bytes, eocd + 10);
    const quint32 centralSize = get32(bytes, eocd + 12);
    const quint32 centralOffset = get32(bytes, eocd + 16);
    if (qsizetype(centralOffset) + qsizetype(centralSize) != eocd) {
        *issues << QStringLiteral("central directory does not end at EOCD");
        return false;
    }
    qsizetype pos = centralOffset;
    for (quint16 i = 0; i < count; ++i) {
        if (pos + 46 > bytes.size()) {
            *issues << QStringLiteral("central directory entry %1 truncated").arg(i);
            return false;
        }
        if (get32(bytes, pos) != kCentralDirSig) {
            *issues << QStringLiteral("central directory entry %1 has bad signature").arg(i);
            return false;
        }
        CentralRecord rec;
        rec.crc = get32(bytes, pos + 16);
        rec.compressedSize = get32(bytes, pos + 20);
        rec.uncompressedSize = get32(bytes, pos + 24);
        const quint16 nameLen = get16(bytes, pos + 28);
        const quint16 extraLen = get16(bytes, pos + 30);
        const quint16 commentLen = get16(bytes, pos + 32);
        rec.localHeaderOffset = get32(bytes, pos + 42);
        if (pos + 46 + qsizetype(nameLen) + extraLen + commentLen > bytes.size()) {
            *issues << QStringLiteral("central directory entry %1 overruns archive").arg(i);
            return false;
        }
        rec.name = QString::fromUtf8(bytes.mid(pos + 46, nameLen));
        records->push_back(rec);
        pos += 46 + qsizetype(nameLen) + extraLen + commentLen;
    }
    if (pos != eocd) {
        *issues << QStringLiteral("central directory size mismatch");
        return false;
    }
    return true;
}

} // namespace

QByteArray buildStoreArchive(const std::vector<ZipEntry>& entries)
{
    QByteArray out;
    std::vector<CentralRecord> central;
    central.reserve(entries.size());
    for (const ZipEntry& entry : entries) {
        const QByteArray name = entry.name.toUtf8();
        const quint32 crc = crc32(entry.content);
        const quint32 size = quint32(entry.content.size());
        CentralRecord rec{entry.name, crc, size, size, quint32(out.size())};
        put32(&out, kLocalHeaderSig);
        put16(&out, 20);            // version needed
        put16(&out, kUtf8Flag);     // 文件名按 UTF-8 解释
        put16(&out, 0);             // method: store
        put16(&out, kDosTime);
        put16(&out, kDosDate);
        put32(&out, crc);
        put32(&out, size);          // compressed size
        put32(&out, size);          // uncompressed size
        put16(&out, quint16(name.size()));
        put16(&out, 0);             // extra length
        out.append(name);
        out.append(entry.content);
        central.push_back(rec);
    }
    const quint32 centralOffset = quint32(out.size());
    for (const CentralRecord& rec : central) {
        const QByteArray name = rec.name.toUtf8();
        put32(&out, kCentralDirSig);
        put16(&out, 20);            // version made by
        put16(&out, 20);            // version needed
        put16(&out, kUtf8Flag);
        put16(&out, 0);             // method
        put16(&out, kDosTime);
        put16(&out, kDosDate);
        put32(&out, rec.crc);
        put32(&out, rec.compressedSize);
        put32(&out, rec.uncompressedSize);
        put16(&out, quint16(name.size()));
        put16(&out, 0);             // extra
        put16(&out, 0);             // comment
        put16(&out, 0);             // disk start
        put16(&out, 0);             // internal attributes
        put32(&out, 0);             // external attributes
        put32(&out, rec.localHeaderOffset);
        out.append(name);
    }
    const quint32 centralSize = quint32(out.size()) - centralOffset;
    put32(&out, kEndOfDirSig);
    put16(&out, 0);                 // this disk
    put16(&out, 0);                 // central directory disk
    put16(&out, quint16(central.size()));
    put16(&out, quint16(central.size()));
    put32(&out, centralSize);
    put32(&out, centralOffset);
    put16(&out, 0);                 // comment length
    return out;
}

QStringList verifyArchive(const QByteArray& bytes)
{
    QStringList issues;
    std::vector<CentralRecord> records;
    if (!walkCentral(bytes, &records, &issues))
        return issues;
    for (size_t i = 0; i < records.size(); ++i) {
        const CentralRecord& rec = records[i];
        const QString tag = QStringLiteral("entry %1 (%2)")
                                 .arg(static_cast<int>(i))
                                 .arg(rec.name);
        if (rec.compressedSize != rec.uncompressedSize) {
            issues << tag + QStringLiteral(": store mode sizes differ");
            continue;
        }
        const qsizetype off = rec.localHeaderOffset;
        if (off < 0 || off + 30 > bytes.size()) {
            issues << tag + QStringLiteral(": local header out of range");
            continue;
        }
        if (get32(bytes, off) != kLocalHeaderSig) {
            issues << tag + QStringLiteral(": bad local header signature");
            continue;
        }
        const quint32 crc = get32(bytes, off + 14);
        const quint32 size = get32(bytes, off + 18);
        const quint16 nameLen = get16(bytes, off + 26);
        const quint16 extraLen = get16(bytes, off + 28);
        const qsizetype dataPos = off + 30 + qsizetype(nameLen) + extraLen;
        if (dataPos + qsizetype(size) > bytes.size()) {
            issues << tag + QStringLiteral(": data overruns archive");
            continue;
        }
        const QByteArray name = bytes.mid(off + 30, nameLen);
        if (QString::fromUtf8(name) != rec.name) {
            issues << tag + QStringLiteral(": local name mismatch");
            continue;
        }
        if (size != rec.uncompressedSize) {
            issues << tag + QStringLiteral(": local size mismatch");
            continue;
        }
        const QByteArray data = bytes.mid(dataPos, qsizetype(size));
        if (crc32(data) != crc || crc != rec.crc) {
            issues << tag + QStringLiteral(": CRC mismatch");
            continue;
        }
    }
    return issues;
}

bool extractEntry(const QByteArray& bytes, const QString& name,
                  QByteArray* out, QStringList* issues)
{
    std::vector<CentralRecord> records;
    QStringList walkIssues;
    if (!walkCentral(bytes, &records, &walkIssues)) {
        if (issues) *issues << walkIssues;
        return false;
    }
    for (const CentralRecord& rec : records) {
        if (rec.name != name) continue;
        const qsizetype off = rec.localHeaderOffset;
        if (off < 0 || off + 30 > bytes.size()) {
            if (issues) *issues << QStringLiteral("local header out of range");
            return false;
        }
        const quint16 nameLen = get16(bytes, off + 26);
        const quint16 extraLen = get16(bytes, off + 28);
        const qsizetype dataPos = off + 30 + qsizetype(nameLen) + extraLen;
        if (dataPos + qsizetype(rec.uncompressedSize) > bytes.size()) {
            if (issues) *issues << QStringLiteral("entry data overruns archive");
            return false;
        }
        if (out) *out = bytes.mid(dataPos, qsizetype(rec.uncompressedSize));
        return true;
    }
    if (issues) *issues << QStringLiteral("entry not found: %1").arg(name);
    return false;
}

} // namespace editor::io::zip
