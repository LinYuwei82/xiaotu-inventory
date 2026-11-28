#pragma once

// 报表路由共用的导出与格式化工具，按职责分别声明；路由文件统一包含本头文件。

#include "report_export.h"
#include "report_format.h"

namespace xiaotu {
namespace report_common {

// 账龄口径与应收应付台账一致：只统计 v7 迁移后审核的单据。
inline constexpr const char* kArApScopeHeader = "X-Xiaotu-Data-Scope";
inline constexpr const char* kArApScope = "ar_ap_since_v7";

}  // namespace report_common
}  // namespace xiaotu
