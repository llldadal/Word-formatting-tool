#pragma once

// 纵切自测用的本地替身：把交互模块开出的 EditRequest 小纸条按序落到文档快照上。
// 正式集成时这一层由文档模块替换；此处只服务于交互 + 导出两条链的独立联调。

#include "DocumentSnapshot.h"
#include "InteractionTypes.h"

#include <QString>
#include <vector>

namespace editor::slice {

struct ApplyResult {
    data::DocumentSnapshot document;
    bool ok = true;
    QString error; // 失败原因，写给人看的说明。
};

// 按 requests 的顺序执行；任何一条非法就返回 ok=false 且不修改输入文档。
// 成功且 requests 非空时，返回快照的 revision 加一，保持与排版结果的版本关联。
ApplyResult applyRequests(const data::DocumentSnapshot& doc,
                          const std::vector<interaction::EditRequest>& requests);

} // namespace editor::slice
