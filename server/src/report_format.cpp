#include "report_format.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>

#include "xiaotu/error.h"

namespace xiaotu {
namespace report_common {

std::int64_t query_int(const httplib::Request& req, const char* name) {
  const std::string text = req.get_param_value(name);
  std::size_t consumed = 0;
  std::int64_t value = 0;
  const auto reject = [name, &text]() {
    throw AppError("INVALID_INPUT", std::string("参数 ") + name + " 不是有效整数：" + text);
  };
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    reject();
  }
  if (consumed != text.size()) {
    reject();
  }
  return value;
}

ReportFilter parse_filter(const httplib::Request& req) {
  ReportFilter filter;
  if (req.has_param("from")) filter.from = req.get_param_value("from");
  if (req.has_param("to")) filter.to = req.get_param_value("to");
  if (req.has_param("item_id")) filter.item_id = query_int(req, "item_id");
  if (req.has_param("warehouse_id")) {
    filter.warehouse_id = query_int(req, "warehouse_id");
  }
  if (req.has_param("customer_id")) {
    filter.customer_id = query_int(req, "customer_id");
  }
  if (req.has_param("supplier_id")) {
    filter.supplier_id = query_int(req, "supplier_id");
  }
  if (req.has_param("salesperson_id")) {
    filter.salesperson_id = query_int(req, "salesperson_id");
  }
  // limit 走 int：超过 int 范围的值以前会在 static_cast 后回绕成 0（不限行数），
  // 一次请求就能把全量流水读进内存，这里直接拒绝。
  if (req.has_param("limit")) {
    const std::int64_t limit = query_int(req, "limit");
    if (limit < 0 || limit > std::numeric_limits<int>::max()) {
      throw AppError("INVALID_INPUT", "参数 limit 超出范围");
    }
    filter.limit = static_cast<int>(limit);
  }
  if (req.has_param("offset")) {
    const std::int64_t offset = query_int(req, "offset");
    filter.offset = offset < 0 ? 0 : static_cast<int>(
                                         std::min<std::int64_t>(
                                             offset, std::numeric_limits<int>::max()));
  }
  return filter;
}

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
