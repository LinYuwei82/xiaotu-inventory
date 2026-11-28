#include "report_display.h"

#include <string>

namespace xiaotu {
namespace report_common {

std::string yuan(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string period_label(const ReportFilter& filter) {
  if (filter.from.empty() && filter.to.empty()) return "全部期间";
  return (filter.from.empty() ? std::string("起始不限") : filter.from) + " 至 " +
         (filter.to.empty() ? std::string("截止不限") : filter.to);
}

std::string dimension_label(std::int64_t id, const char* name) {
  return id == 0 ? std::string(name) + " 全部"
                 : std::string(name) + " ID " + std::to_string(id);
}

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents) {
  if (revenue_cents == 0) return "—";
  // 毛利率保留一位小数，用整数千分比换算，展示层也不引入浮点。
  const std::int64_t direction = revenue_cents < 0 ? -1 : 1;
  const std::int64_t revenue = revenue_cents < 0 ? -revenue_cents : revenue_cents;
  const std::int64_t profit = profit_cents * direction;
  const std::int64_t negative = profit < 0 ? -1 : 1;
  const std::int64_t absolute = profit < 0 ? -profit : profit;
  const std::int64_t tenths = (absolute * 1000 + revenue / 2) / revenue;
  return (negative < 0 ? "-" : "") + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + "%";
}

std::string biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

std::string cost_basis_label(const SystemSettings& settings) {
  std::string label = settings.cost_basis == "NET" ? "不含税净额" : "含税价";
  if (!settings.cost_basis_changed_at.empty()) {
    label += "（切换于 " + settings.cost_basis_changed_at + "）";
  }
  return label;
}

std::string method_label(const std::string& method) {
  if (method == "CASH") return "现金";
  if (method == "BANK") return "银行";
  if (method == "WECHAT") return "微信";
  if (method == "ALIPAY") return "支付宝";
  if (method == "OTHER") return "其他";
  return method.empty() ? "未记录" : method;
}

}  // namespace report_common
}  // namespace xiaotu
