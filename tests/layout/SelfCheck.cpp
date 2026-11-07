#include "TestFixtures.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

using namespace editor::testing;
using namespace editor::data;

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    QTextStream out(stdout);
    try {
        if (!QFontDatabase::families().contains("Microsoft YaHei")) throw std::runtime_error("Microsoft YaHei is required for frozen standards");
        int checks = 0;
        const auto require = [&](bool ok, const QString& reason) {
            ++checks;
            if (!ok) throw std::runtime_error(reason.toStdString());
        };
        for (const auto& fixture : makeLayoutCases()) {
            require(validateSnapshot(fixture.document).isEmpty(), "invalid input " + fixture.id);
            const auto standard = makeStandardLayout(fixture.id);
            auto errors = compareLayout(fixture.document, standard, standard);
            require(errors.isEmpty(), "invalid standard " + fixture.id + "\n" + errors.join('\n'));
            auto reordered = standard;
            std::reverse(reordered.containers.begin(), reordered.containers.end());
            std::reverse(reordered.images.begin(), reordered.images.end());
            std::reverse(reordered.textFragments.begin(), reordered.textFragments.end());
            std::reverse(reordered.textLayouts.begin(), reordered.textLayouts.end());
            for (auto& fragment : reordered.textFragments)
                fragment.layoutIndex = int(reordered.textLayouts.size()) - 1 - fragment.layoutIndex;
            require(compareLayout(fixture.document, standard, reordered).isEmpty(), "record order must not matter");
            out << "FIXTURE OK " << fixture.id << '\n';
        }
        const auto fixture = makeLayoutCase("03");
        const auto standard = makeStandardLayout("03");
        const auto reject = [&](const QString& label, const std::function<void(LayoutResult&)>& mutate) {
            auto changed = standard;
            mutate(changed);
            require(!compareLayout(fixture.document, standard, changed).isEmpty(), "comparator missed " + label);
        };
        reject("revision", [](auto& r) { ++r.documentRevision; });
        reject("paper offset", [](auto& r) { r.pageRect.translate(20, 20); });
        reject("NaN", [](auto& r) { r.layoutBounds.setWidth(std::numeric_limits<qreal>::quiet_NaN()); });
        reject("padding", [](auto& r) { r.containers[0].contentRect.translate(1, 0); });
        reject("margin", [](auto& r) { r.containers[0].outerRect.translate(1, 0); });
        reject("parent", [](auto& r) { r.containers.back().parentId = "wrong"; });
        reject("duplicate container", [](auto& r) { r.containers.push_back(r.containers.front()); });
        reject("image missing", [](auto& r) { r.images.pop_back(); });
        reject("image crop", [](auto& r) { r.images[0].sourceRect.setWidth(1); });
        reject("image resource", [](auto& r) { r.images[0].resourceId = "wrong"; });
        reject("text missing", [](auto& r) { r.textFragments.pop_back(); });
        reject("duplicate text", [](auto& r) { r.textFragments.push_back(r.textFragments.front()); });
        reject("source range", [](auto& r) { ++r.textFragments[0].sourceRange.end; });
        reject("invalid layout", [](auto& r) { r.textFragments[0].layoutIndex = -1; });
        reject("null layout", [](auto& r) { r.textLayouts[0].reset(); });
        reject("invalid line", [](auto& r) { r.textFragments[0].lineIndex = 9999; });
        auto permitted = standard;
        permitted.containers[0].rect.translate(0.25, 0);
        require(compareLayout(fixture.document, standard, permitted).isEmpty(), "coordinate tolerance not respected");
        auto invalid = fixture.document;
        invalid.root.children[0].id = invalid.root.children[1].id;
        require(!validateSnapshot(invalid).isEmpty(), "duplicate ID not detected");
        invalid = fixture.document;
        invalid.root.children[1].children[0].children.push_back(invalid.root.children[0]);
        require(!validateSnapshot(invalid).isEmpty(), "excess nesting not detected");
        invalid = fixture.document;
        invalid.resources.clear();
        require(!validateSnapshot(invalid).isEmpty(), "missing resource not detected");
        {
            const auto rich = makeLayoutCase("07");
            const auto good = makeStandardLayout("07");
            auto bad = good;
            const auto& original = *good.textLayouts.front();
            auto changed = std::make_shared<QTextLayout>(original.text(), original.font());
            changed->setTextOption(original.textOption());
            auto formats = original.formats();
            for (auto& range : formats) range.format.setForeground(QColor("#ff00ff"));
            changed->setFormats(formats);
            changed->beginLayout();
            for (int i = 0; i < original.lineCount(); ++i) {
                auto line = changed->createLine();
                line.setLineWidth(original.lineAt(i).width());
                line.setPosition(original.lineAt(i).position());
            }
            changed->endLayout();
            bad.textLayouts.front() = changed;
            require(compareLayout(rich.document, good, bad).join('\n').contains(".color"), "text color change not detected");
        }
        // 这里只验证固定标准与比较器；没有调用 composeForTest，不能称为排版算法通过。
        out << checks << " fixture/comparator checks passed; layout implementation was NOT tested.\n";
        return 0;
    } catch (const std::exception& exception) {
        out << "SELF CHECK FAILED: " << exception.what() << '\n';
        return 1;
    }
}
