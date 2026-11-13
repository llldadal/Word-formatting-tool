#include "TextBoundary.h"

#include <QTextBoundaryFinder>

namespace editor::interaction {
namespace {

const data::ContainerNode* findNode(const data::ContainerNode& root,
                                    const data::ContainerId& id)
{
    if (root.id == id)
        return &root;
    for (const auto& child : root.children) {
        if (child.id == id)
            return &child;
        for (const auto& grand : child.children)
            if (grand.id == id)
                return &grand;
    }
    return nullptr;
}

} // 匿名命名空间结束

const data::TextContent* findTextContent(const data::DocumentSnapshot& doc,
                                         const data::ContainerId& id)
{
    const data::ContainerNode* node = findNode(doc.root, id);
    if (!node || node->type != data::ContainerType::Text)
        return nullptr;
    return &node->text;
}

int textLength(const data::DocumentSnapshot& doc, const data::ContainerId& id)
{
    const data::TextContent* content = findTextContent(doc, id);
    return content ? content->text.size() : 0;
}

int nextBoundary(const QString& text, int pos)
{
    if (text.isEmpty())
        return 0;
    const int start = qBound(0, pos, text.size());
    if (start >= text.size())
        return text.size();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(start);
    const int next = finder.toNextBoundary();
    return next < 0 ? text.size() : next;
}

int prevBoundary(const QString& text, int pos)
{
    if (text.isEmpty())
        return 0;
    const int start = qBound(0, pos, text.size());
    if (start <= 0)
        return 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(start);
    const int prev = finder.toPreviousBoundary();
    return prev < 0 ? 0 : prev;
}

int nextBoundary(const data::DocumentSnapshot& doc, const data::ContainerId& id, int pos)
{
    const data::TextContent* content = findTextContent(doc, id);
    return content ? nextBoundary(content->text, pos) : 0;
}

int prevBoundary(const data::DocumentSnapshot& doc, const data::ContainerId& id, int pos)
{
    const data::TextContent* content = findTextContent(doc, id);
    return content ? prevBoundary(content->text, pos) : 0;
}

} // 命名空间 editor::interaction 结束
