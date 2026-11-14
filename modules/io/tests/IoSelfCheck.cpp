// 导入导出模块自测：用仓库自带的固定输入和标准排版，把每一道题导出成 DOCX，
// 校验包结构、XML 内容和错误分支。不依赖文档、排版或界面组的实现。
// 输出全部使用 ASCII，避免 Qt Creator 控制台按本地编码显示成乱码。

#include "DocxExporter.h"
#include "ZipWriter.h"
#include "TestFixtures.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMarginsF>
#include <QRectF>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <cstdio>

namespace ed = editor::data;

using editor::io::buildDocx;
using editor::io::exportDocx;
using editor::io::DocxBuildResult;
using editor::io::DocxExportStatus;
using editor::testing::LayoutCase;
using editor::testing::makeLayoutCase;
using editor::testing::makeStandardLayout;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const QString& what)
{
    if (ok) {
        ++g_passed;
        std::printf("[PASS] %s\n", what.toUtf8().constData());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.toUtf8().constData());
    }
}

int occurrences(const QString& hay, const QString& needle)
{
    return hay.count(needle);
}

bool zipEntry(const QByteArray& package, const QString& name, QByteArray* out)
{
    QStringList issues;
    return editor::io::zip::extractEntry(package, name, out, &issues) && out->size() > 0;
}

struct ExportedCase {
    DocxBuildResult build;
    QString documentXml;
    QString relsXml;
    QString stylesXml;
};

// 导出一道题并检查基础包结构。expectedOk=false 时只返回状态。
bool exportCase(const char* caseId, ExportedCase* out, bool* structureOk)
{
    const LayoutCase fixture = makeLayoutCase(QString::fromLatin1(caseId));
    const auto layout = makeStandardLayout(QString::fromLatin1(caseId));
    out->build = buildDocx(fixture.document, layout);

    if (out->build.result.status != DocxExportStatus::Ok) {
        *structureOk = false;
        return false;
    }
    const QByteArray& package = out->build.package;
    QStringList zipIssues = editor::io::zip::verifyArchive(package);
    QByteArray documentBytes, relsBytes, stylesBytes, typesBytes;
    const bool hasParts = zipEntry(package, QStringLiteral("word/document.xml"), &documentBytes)
        && zipEntry(package, QStringLiteral("word/_rels/document.xml.rels"), &relsBytes)
        && zipEntry(package, QStringLiteral("word/styles.xml"), &stylesBytes);
    const bool hasTypes = editor::io::zip::extractEntry(
        package, QStringLiteral("[Content_Types].xml"), &typesBytes, nullptr)
        && typesBytes.size() > 0;
    out->documentXml = QString::fromUtf8(documentBytes);
    out->relsXml = QString::fromUtf8(relsBytes);
    out->stylesXml = QString::fromUtf8(stylesBytes);
    *structureOk = zipIssues.isEmpty() && hasParts && hasTypes;
    return true;
}

} // 匿名命名空间结束

int main(int argc, char** argv)
{
    // 标准排版里的 QTextLayout 需要字体库，必须用 QGuiApplication。
    QGuiApplication app(argc, argv);
    std::printf("===== IO self-check: DOCX export =====\n");

    // ---- 用例 01：标题 + 两段正文 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("01", &c, &structureOk);
        check(ok, "case 01 exports");
        check(structureOk, "case 01 package structure valid");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 3,
              "case 01 has 3 paragraphs");
        check(ok && c.documentXml.contains(QStringLiteral("w:val=\"Title\"")),
              "case 01 title uses Title style");
        check(ok && c.documentXml.contains(QStringLiteral("w:sz w:val=\"39\"")),
              "case 01 title font 19.5pt -> half-point 39");
        check(ok && c.documentXml.contains(QStringLiteral("w:line=\"600\"")),
              "case 01 title line height 40px -> 600 twip");
        check(ok && c.documentXml.contains(QStringLiteral("w:before=\"270\"")),
              "case 01 title bottom margin 18px -> 270 twip block gap");
        check(ok && c.documentXml.contains(QStringLiteral("雨后观察记录")),
              "case 01 body text exported");
        check(ok && c.stylesXml.contains(QStringLiteral("w:styleId=\"Heading1\""))
              && c.stylesXml.contains(QStringLiteral("w:styleId=\"Heading9\"")),
              "case 01 styles define Heading1..Heading9");
        check(ok && c.documentXml.contains(QStringLiteral("<w:sectPr><w:pgSz w:w=\"11400\" w:h=\"15000\"/>")),
              "case 01 page size 760x1000px -> 11400x15000 twip");
        check(ok && c.documentXml.contains(QStringLiteral("w:top=\"660\"")),
              "case 01 page margin 44px -> 660 twip");
    }

    // ---- 用例 02：单张浮动图片 + 四周型绕排 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("02", &c, &structureOk);
        check(ok, "case 02 exports");
        check(structureOk, "case 02 package structure valid");
        check(ok && occurrences(c.documentXml, QStringLiteral("<wp:anchor")) == 1,
              "case 02 has one floating anchor");
        check(ok && c.documentXml.contains(QStringLiteral("<wp:wrapSquare wrapText=\"bothSides\"/>")),
              "case 02 uses square wrap both sides");
        check(ok && c.documentXml.contains(QStringLiteral("cx=\"1866900\""))
              && c.documentXml.contains(QStringLiteral("cy=\"1333500\"")),
              "case 02 image extent 196x140px in EMU");
        check(ok && c.documentXml.contains(QStringLiteral("distL=\"114300\""))
              && c.documentXml.contains(QStringLiteral("distT=\"66675\"")),
              "case 02 wrap distances 12px/7px in EMU");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 2,
              "case 02 anchor does not add paragraphs");
        QByteArray media;
        check(ok && zipEntry(c.build.package, QStringLiteral("word/media/image1.png"), &media)
              && !QImage::fromData(media).isNull(),
              "case 02 media image decodes");
        check(ok && c.relsXml.contains(QStringLiteral("Target=\"media/image1.png\"")),
              "case 02 image relationship registered");
    }

    // ---- 用例 03：两张浮动图片 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("03", &c, &structureOk);
        check(ok, "case 03 exports");
        check(structureOk, "case 03 package structure valid");
        check(ok && occurrences(c.documentXml, QStringLiteral("<wp:anchor")) == 2,
              "case 03 has two floating anchors");
        check(ok && occurrences(c.relsXml, QStringLiteral("Target=\"media/")) == 2,
              "case 03 has two image relationships");
        QByteArray a, b;
        check(ok && zipEntry(c.build.package, QStringLiteral("word/media/image1.png"), &a)
              && zipEntry(c.build.package, QStringLiteral("word/media/image2.png"), &b)
              && a != b,
              "case 03 stores two distinct media files");
    }

    // ---- 用例 04：正文后的行内图片 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("04", &c, &structureOk);
        check(ok, "case 04 exports");
        check(structureOk, "case 04 package structure valid");
        check(ok && c.documentXml.contains(QStringLiteral("<wp:inline")),
              "case 04 uses inline drawing");
        check(ok && !c.documentXml.contains(QStringLiteral("<wp:anchor")),
              "case 04 has no floating anchor");
        check(ok && c.documentXml.contains(QStringLiteral("cx=\"1714500\""))
              && c.documentXml.contains(QStringLiteral("cy=\"1143000\"")),
              "case 04 image extent 180x120px in EMU");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 4,
              "case 04 has 4 paragraphs (title, p1, p2, image)");
    }

    // ---- 用例 06：浮动文本框 + 四周型绕排 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("06", &c, &structureOk);
        check(ok, "case 06 exports");
        check(structureOk, "case 06 package structure valid");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:txbxContent>")) == 2,
              "case 06 textbox content in choice and fallback");
        check(ok && c.documentXml.contains(QStringLiteral("mc:Fallback"))
              && c.documentXml.contains(QStringLiteral("<v:shape")),
              "case 06 VML fallback present");
        check(ok && c.documentXml.contains(QStringLiteral("distL=\"114300\""))
              && c.documentXml.contains(QStringLiteral("distT=\"66675\"")),
              "case 06 wrap distances 12px/7px in EMU");
        check(ok && occurrences(c.documentXml,
                                QStringLiteral("现场补记：本次三处读数")) == 2,
              "case 06 note text appears in both branches");
    }

    // ---- 用例 07：标题、段落格式与字符格式覆盖 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("07", &c, &structureOk);
        check(ok, "case 07 exports");
        check(structureOk, "case 07 package structure valid");
        check(ok && c.documentXml.contains(QStringLiteral("w:val=\"Heading2\"")),
              "case 07 heading level 2 style");
        check(ok && c.documentXml.contains(QStringLiteral("w:val=\"253746\"")),
              "case 07 default color 253746");
        check(ok && c.documentXml.contains(QStringLiteral("w:val=\"B63D2E\"")),
              "case 07 highlighted span color B63D2E");
        check(ok && c.documentXml.contains(QStringLiteral("<w:u w:val=\"single\"/>")),
              "case 07 underlined span");
        check(ok && c.documentXml.contains(QStringLiteral("<w:i/>")),
              "case 07 italic paragraph default");
        check(ok && c.documentXml.contains(QStringLiteral("w:jc w:val=\"right\"")),
              "case 07 right-aligned paragraph");
        check(ok && c.documentXml.contains(QStringLiteral("w:left=\"240\"")),
              "case 07 left padding 16px -> 240 twip indent");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 3,
              "case 07 has 3 paragraphs");
    }

    // ---- 用例 08：浮动对象越出固定纸面，应报兼容性错误 ----
    {
        const LayoutCase fixture = makeLayoutCase(QStringLiteral("08"));
        const auto layout = makeStandardLayout(QStringLiteral("08"));
        const DocxBuildResult r = buildDocx(fixture.document, layout);
        check(r.result.status == DocxExportStatus::CompatibilityError,
              "case 08 rejected as compatibility error");
        check(r.result.containerId == QStringLiteral("photo1"),
              "case 08 error names container photo1");
    }

    // ---- 用例 09：分组盒模型与边距累加 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("09", &c, &structureOk);
        check(ok, "case 09 exports");
        check(structureOk, "case 09 package structure valid");
        check(ok && c.documentXml.contains(QStringLiteral("w:left=\"660\"")),
              "case 09 nested indent 44px -> 660 twip");
        check(ok && c.documentXml.contains(QStringLiteral("<wp:inline")),
              "case 09 flow image inside group is inline");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 3,
              "case 09 has 3 paragraphs");
    }

    // ---- 用例 10：UTF-16 偏移、emoji、空段落 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("10", &c, &structureOk);
        check(ok, "case 10 exports");
        check(structureOk, "case 10 package structure valid");
        check(ok && c.documentXml.contains(QString::fromUtf8("\xF0\x9F\x98\x80")),
              "case 10 emoji preserved");
        check(ok && c.documentXml.contains(QStringLiteral("e\u0301")),
              "case 10 combining accent preserved");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 4,
              "case 10 has 4 paragraphs including empties");
    }

    // ---- 用例 11：空文档 ----
    {
        ExportedCase c;
        bool structureOk = false;
        const bool ok = exportCase("11", &c, &structureOk);
        check(ok, "case 11 exports");
        check(structureOk, "case 11 package structure valid");
        check(ok && occurrences(c.documentXml, QStringLiteral("<w:p>")) == 1,
              "case 11 keeps one editable paragraph");
        check(ok && c.documentXml.contains(QStringLiteral("<w:sectPr")),
              "case 11 has section properties");
    }

    // ---- 确定性：相同输入生成完全相同的字节 ----
    {
        const LayoutCase fixture = makeLayoutCase(QStringLiteral("02"));
        const auto layout = makeStandardLayout(QStringLiteral("02"));
        const DocxBuildResult a = buildDocx(fixture.document, layout);
        const DocxBuildResult b = buildDocx(fixture.document, layout);
        check(a.result.ok() && b.result.ok() && a.package == b.package,
              "identical input builds identical package");
    }

    // ---- 版本不一致应返回 IO 错误 ----
    {
        const LayoutCase fixture = makeLayoutCase(QStringLiteral("01"));
        auto layout = makeStandardLayout(QStringLiteral("01"));
        layout.documentRevision += 1;
        const DocxBuildResult r = buildDocx(fixture.document, layout);
        check(r.result.status == DocxExportStatus::IoError,
              "revision mismatch rejected");
        check(!r.result.errorMessage.isEmpty(),
              "revision mismatch carries a reason");
    }

    // ---- 文件写出与非法路径 ----
    {
        const LayoutCase fixture = makeLayoutCase(QStringLiteral("01"));
        const auto layout = makeStandardLayout(QStringLiteral("01"));
        const DocxBuildResult build = buildDocx(fixture.document, layout);

        QTemporaryDir temp;
        const QString path = temp.filePath(QStringLiteral("roundtrip.docx"));
        const auto exported = exportDocx(fixture.document, layout, path);
        QFile file(path);
        const bool readable = exported.ok() && file.open(QIODevice::ReadOnly);
        QByteArray read;
        if (readable) read = file.readAll();
        check(readable && read == build.package,
              "exportDocx writes byte-identical file");

        const auto bad = exportDocx(fixture.document, layout,
                                    QStringLiteral("Z:/no-such-dir/x.docx"));
        check(bad.status == DocxExportStatus::IoError,
              "invalid path returns IO error");
    }

    // ---- 在可执行文件旁写出可用 Word 打开的产物 ----
    {
        const QString outDir = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/io-export");
        QDir().mkpath(outDir);
        int written = 0;
        for (const char* id : {"01", "02", "03", "04", "06", "07", "09", "10", "11"}) {
            const LayoutCase fixture = makeLayoutCase(QString::fromLatin1(id));
            const auto layout = makeStandardLayout(QString::fromLatin1(id));
            const QString path = outDir + QStringLiteral("/case-%1.docx").arg(QLatin1String(id));
            if (exportDocx(fixture.document, layout, path).ok()) ++written;
        }
        check(written == 9, QString("wrote %1/9 sample DOCX files to io-export").arg(written));
    }

    // ---- 回归：本轮修掉的导出问题，逐条用真实 DOCX 内容验证 ----
    {
        const LayoutCase f01 = makeLayoutCase(QStringLiteral("01"));
        const auto l01 = makeStandardLayout(QStringLiteral("01"));

        // 页边距四个方向相互独立。
        ed::DocumentSnapshot marginsDoc = f01.document;
        marginsDoc.page.margins.setTop(10);
        marginsDoc.page.margins.setRight(30);
        marginsDoc.page.margins.setBottom(20);
        marginsDoc.page.margins.setLeft(40);
        const DocxBuildResult rm = buildDocx(marginsDoc, l01);
        QByteArray marginsBytes, stylesBytes;
        zipEntry(rm.package, QStringLiteral("word/document.xml"), &marginsBytes);
        zipEntry(rm.package, QStringLiteral("word/styles.xml"), &stylesBytes);
        const QString marginsXml = QString::fromUtf8(marginsBytes);
        check(rm.result.ok()
                  && marginsXml.contains(QStringLiteral(
                         "w:top=\"150\" w:right=\"450\" w:bottom=\"300\" w:left=\"600\"")),
              "page margins map to four independent values");
        check(QString::fromUtf8(stylesBytes).contains(
                  QStringLiteral("<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>")),
              "outlineLvl lives inside the style's w:pPr");

        // 负首行缩进用 w:hanging，lineHeight=0 不生成 w:line="0"。
        ed::DocumentSnapshot indentDoc = f01.document;
        for (auto& node : indentDoc.root.children) {
            if (node.type == ed::ContainerType::Text && !node.text.paragraphs.empty()) {
                node.text.paragraphs[0].format.firstLineIndent = -20;
                node.text.paragraphs[0].format.lineHeight = 0;
                node.text.paragraphs[0].format.spaceBefore = 10;
                break;
            }
        }
        const DocxBuildResult ri = buildDocx(indentDoc, l01);
        QByteArray indentBytes;
        zipEntry(ri.package, QStringLiteral("word/document.xml"), &indentBytes);
        const QString indentXml = QString::fromUtf8(indentBytes);
        check(indentXml.contains(QStringLiteral("w:hanging=\"300\"")),
              "negative first-line indent becomes w:hanging");
        check(!indentXml.contains(QStringLiteral("w:line=\"0\"")),
              "zero line height does not emit w:line=0");

        // 标题级别超出 1..9 时收敛到已定义的样式。
        ed::DocumentSnapshot headingDoc = f01.document;
        for (auto& node : headingDoc.root.children) {
            if (node.type == ed::ContainerType::Text && !node.text.paragraphs.empty()) {
                node.text.paragraphs[0].role = ed::ParagraphRole::Heading;
                node.text.paragraphs[0].headingLevel = 12;
                break;
            }
        }
        const DocxBuildResult rh = buildDocx(headingDoc, l01);
        QByteArray headingBytes;
        zipEntry(rh.package, QStringLiteral("word/document.xml"), &headingBytes);
        const QString headingXml = QString::fromUtf8(headingBytes);
        check(headingXml.contains(QStringLiteral("w:val=\"Heading9\""))
                  && !headingXml.contains(QStringLiteral("Heading12")),
              "out-of-range heading level clamps to Heading9");

        // CT_RPr 子元素顺序：color / sz 都要排在 u 前面。
        const LayoutCase f07 = makeLayoutCase(QStringLiteral("07"));
        const auto l07 = makeStandardLayout(QStringLiteral("07"));
        const DocxBuildResult r07 = buildDocx(f07.document, l07);
        QByteArray rprBytes;
        zipEntry(r07.package, QStringLiteral("word/document.xml"), &rprBytes);
        const QString rprXml = QString::fromUtf8(rprBytes);
        check(rprXml.indexOf(QStringLiteral("<w:color ")) >= 0
                  && rprXml.indexOf(QStringLiteral("<w:u ")) > rprXml.indexOf(QStringLiteral("<w:color ")),
              "CT_RPr order: w:color before w:u");
        check(rprXml.indexOf(QStringLiteral("<w:sz ")) < rprXml.indexOf(QStringLiteral("<w:u ")),
              "CT_RPr order: w:sz before w:u");

        // 浮动图片：effectExtent 不再混用绕排距离。
        const LayoutCase f02 = makeLayoutCase(QStringLiteral("02"));
        const auto l02 = makeStandardLayout(QStringLiteral("02"));
        const DocxBuildResult r02 = buildDocx(f02.document, l02);
        QByteArray anchorBytes;
        zipEntry(r02.package, QStringLiteral("word/document.xml"), &anchorBytes);
        check(QString::fromUtf8(anchorBytes).contains(
                  QStringLiteral("<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>")),
              "effectExtent stays zero when the shape has no visual effect");

        // 行内图片：段落行高覆盖 + 裁剪区域真正写进 a:srcRect。
        const LayoutCase f04 = makeLayoutCase(QStringLiteral("04"));
        const auto l04 = makeStandardLayout(QStringLiteral("04"));
        const DocxBuildResult r04 = buildDocx(f04.document, l04);
        QByteArray inlineBytes;
        zipEntry(r04.package, QStringLiteral("word/document.xml"), &inlineBytes);
        const QString inlineXml = QString::fromUtf8(inlineBytes);
        check(inlineXml.contains(QStringLiteral("w:lineRule=\"atLeast\"")),
              "image paragraph overrides the fixed line height with atLeast");
        check(!inlineXml.contains(QStringLiteral("<a:srcRect")),
              "full-image sourceRect does not emit a redundant a:srcRect");

        auto cropped = l04;
        cropped.images[0].sourceRect = QRectF(100, 50, 300, 200);
        const DocxBuildResult rc = buildDocx(f04.document, cropped);
        QByteArray cropBytes;
        zipEntry(rc.package, QStringLiteral("word/document.xml"), &cropBytes);
        check(QString::fromUtf8(cropBytes).contains(QStringLiteral(
                  "<a:srcRect l=\"15625\" t=\"12500\" r=\"37500\" b=\"37500\"/>")),
              "cropped sourceRect becomes a:srcRect percentages");

        // 少了 ImageLayout 必须报错，不能静默输出空段落还返回 Ok。
        auto missing = l04;
        missing.images.clear();
        const DocxBuildResult rmiss = buildDocx(f04.document, missing);
        check(rmiss.result.status == DocxExportStatus::IoError,
              "missing ImageLayout is rejected instead of silently exporting an empty paragraph");

        // 非法字符过滤后，document.xml 仍然是合法 XML。
        ed::DocumentSnapshot badDoc = f01.document;
        for (auto& node : badDoc.root.children) {
            if (node.type != ed::ContainerType::Text || node.text.paragraphs.empty())
                continue;
            QString bad = QStringLiteral("A");
            bad.append(QChar(0x01));
            bad.append(QChar(0xD800)); // 孤立高代理项。
            bad.append(QStringLiteral("BC"));
            node.text.text = bad;
            node.text.paragraphs.erase(node.text.paragraphs.begin() + 1,
                                       node.text.paragraphs.end());
            node.text.paragraphs[0].range = {0, static_cast<int>(bad.size())};
            node.text.formatSpans.clear();
            break;
        }
        const DocxBuildResult rbad = buildDocx(badDoc, l01);
        QByteArray badBytes;
        zipEntry(rbad.package, QStringLiteral("word/document.xml"), &badBytes);
        QXmlStreamReader reader(QString::fromUtf8(badBytes));
        while (!reader.atEnd())
            reader.readNext();
        check(!reader.hasError() && !QString::fromUtf8(badBytes).contains(QChar(0x01)),
              "illegal XML characters are stripped and the part stays well-formed");

        // 所有真实样例的 document.xml 都能被 XML 解析器完整读完。
        for (const char* id : {"01", "02", "03", "04", "06", "07", "09", "10", "11"}) {
            const LayoutCase fx = makeLayoutCase(QString::fromLatin1(id));
            const auto ly = makeStandardLayout(QString::fromLatin1(id));
            const DocxBuildResult r = buildDocx(fx.document, ly);
            QByteArray part;
            bool ok = r.result.ok()
                   && zipEntry(r.package, QStringLiteral("word/document.xml"), &part);
            if (ok) {
                QXmlStreamReader xmlReader(QString::fromUtf8(part));
                while (!xmlReader.atEnd())
                    xmlReader.readNext();
                ok = !xmlReader.hasError();
            }
            check(ok, QString("case %1 document.xml is well-formed XML")
                          .arg(QLatin1String(id)));
        }
    }

    std::printf("===== IO self-check: %s =====\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    std::printf("[OK] IO self-check %s.\n",
                g_failed == 0 ? "passed" : "failed");
    return g_failed == 0 ? 0 : 1;
}
