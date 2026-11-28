#pragma once

#include <cstdint>
#include <string>

#include "xiaotu/report_service.h"
#include "xiaotu/setting_service.h"

namespace xiaotu {
namespace report_common {

// 金额统一按「元、两位小数」输出；从整数分换算，避免浮点误差。
std::string yuan(std::int64_t cents);

std::string period_label(const ReportFilter& filter);

std::string dimension_label(std::int64_t id, const char* name);

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents);

std::string biz_category_label(const std::string& category);

// 成本口径说明：报表标注当前口径与切换日，避免把切换前后的毛利直接对比。
std::string cost_basis_label(const SystemSettings& settings);

std::string method_label(const std::string& method);

}  // namespace report_common
}  // namespace xiaotu
