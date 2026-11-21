#pragma once

#include "LayoutResult.h"

#include <optional>

namespace editor::layout::v02 {

// All rectangles and QTextLine positions are local to the paper on this page.
// The page index is its zero-based position in PagedLayoutResult::pages.
struct LayoutPage {
    QRectF pageRect;
    QRectF contentRect;
    std::vector<data::ContainerLayout> containers;
    std::vector<data::TextFragment> textFragments;
    std::vector<data::ImageLayout> images;
};

enum class LayoutIssueKind {
    InvalidPage,
    ContainerDoesNotFit,
    ImageDoesNotFit,
    MissingImage,
    TextRowDoesNotFit,
    PageLimitExceeded
};

struct LayoutIssue {
    LayoutIssueKind kind;
    data::ContainerId containerId;
    QString reason;
    std::optional<data::TextRange> sourceRange;
};

struct PagedLayoutResult {
    quint64 documentRevision = 0;
    std::vector<LayoutPage> pages;
    // TextFragment::layoutIndex addresses this shared pool, across all pages.
    std::vector<std::shared_ptr<const QTextLayout>> textLayouts;
    std::vector<LayoutIssue> issues;
};

// Paginate a document snapshot without changing it. An object that cannot be
// placed is reported in issues. Other objects may still be laid out.
PagedLayoutResult composeLayout(const data::DocumentSnapshot& document);

} // namespace editor::layout::v02
