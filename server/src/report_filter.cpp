#include "report_filter.h"

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

}  // namespace report_common
}  // namespace xiaotu
