#pragma once

#include "DocumentSnapshot.h"
#include "LayoutResult.h"

namespace editor::layout {

data::LayoutResult composeLayout(
    const data::DocumentSnapshot& document
    );

}