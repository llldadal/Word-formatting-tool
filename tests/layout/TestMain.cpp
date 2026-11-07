#include "TestFixtures.h"
#include "LayoutAdapter.h"
#include <QGuiApplication>
#include <QTextStream>
#include <exception>

int main(int argc, char** argv)
{
    // 只建立 Qt 排字所需环境，不创建窗口，也不执行绘图测试。
    QGuiApplication application(argc, argv);
    QTextStream out(stdout);
    const auto arguments = application.arguments().mid(1);
    try {
        const auto cases = editor::testing::makeLayoutCases();
        if (arguments == QStringList{"--list"}) {
            for (const auto& c : cases) out << c.id << "  " << c.title << '\n';
            return 0;
        }
        QString selected;
        if (arguments.size() == 2 && arguments[0] == "--case") selected = arguments[1];
        else if (!arguments.isEmpty() && arguments != QStringList{"--all"}) {
            out << "Usage: EditorLayoutTests [--list | --all | --case 03]\n";
            return 2;
        }
        int ran = 0, failed = 0;
        for (const auto& c : cases) {
            if (!selected.isEmpty() && selected != c.id) continue;
            const auto invalid = editor::testing::validateSnapshot(c.document);
            if (!invalid.isEmpty()) throw std::logic_error(invalid.join('\n').toStdString());
            const auto expected = editor::testing::makeStandardLayout(c.id);
            // 实际实现只收到输入快照，标准结果不作为排版调用参数。
            const auto actual = editor::testing::composeForTest(c.document);
            const auto errors = editor::testing::compareLayout(c.document, expected, actual);
            ++ran;
            if (errors.isEmpty()) out << "PASS " << c.id << '\n';
            else {
                ++failed;
                out << "FAIL " << c.id << '\n';
                for (const auto& error : errors) out << "  " << error << '\n';
            }
        }
        if (ran == 0) { out << "Unknown case id (05 is deferred)\n"; return 2; }
        out << ran << " cases, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& exception) {
        out << "NOT RUN: " << exception.what() << '\n';
        return 2;
    }
}
