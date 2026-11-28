#pragma once

#include <string>

#include "xiaotu/report_service.h"

namespace xiaotu {

// 采购明细与采购汇总共用的口径说明。
std::string purchase_scope(const ReportFilter& filter);

}  // namespace xiaotu
