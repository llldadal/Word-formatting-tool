#include "LayoutAdapter.h"
#include "LayoutModule.h"

namespace editor::testing {

data::LayoutResult composeForTest(const data::DocumentSnapshot& document)
{
    return layout::composeLayout(document);
}

} // namespace editor::testing