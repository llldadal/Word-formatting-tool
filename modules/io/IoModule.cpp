#include "DocumentSnapshot.h"
#include "LayoutResult.h"

// 导入导出模块的工作区。
// 使用同一版本的文档快照和容器几何结果，导出可编辑的 DOCX。
// 负责段落语义、字符格式、图片资源、位置与矩形绕排到 Word 原生结构的映射。
// DOCX 映射和兼容性边界见 docs/v0.1-DOCX映射草案.md，实际 Word 效果需要验证。
// 文件路径由 UI 提供，导出模块反馈成功或带原因的失败结果。
// 文件读写库、内部类与方法由模块负责人设计。
