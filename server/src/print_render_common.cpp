#include "print_render.h"

#include <cstdio>
#include <string>
#include <string_view>

#include "xiaotu/error.h"

namespace xiaotu {

std::int64_t param_i64(const httplib::Request& req, const char* name, std::int64_t fallback) {
  if (!req.has_param(name)) return fallback;
  try {
    return std::stoll(req.get_param_value(name));
  } catch (const std::exception&) {
    throw AppError("INVALID_INPUT", std::string("参数不是合法数字：") + name);
  }
}

std::string param_text(const httplib::Request& req, const char* name) {
  return req.has_param(name) ? req.get_param_value(name) : std::string{};
}

std::string period_label(const std::string& from, const std::string& to) {
  if (from.empty() && to.empty()) return "全部期间";
  return (from.empty() ? std::string("起始不限") : from) + " 至 " +
         (to.empty() ? std::string("截止不限") : to);
}

std::string dimension_label(std::int64_t id, const char* name) {
  return id == 0 ? std::string(name) + " 全部"
                 : std::string(name) + " ID " + std::to_string(id);
}

std::string print_money_text(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

// 税率（整数万分比）→ 展示文本：1300 → 13%，650 → 6.5%。
std::string rate_text(std::int64_t bp) {
  std::string text = std::to_string(bp / 100);
  if (bp % 100 != 0) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), ".%02lld", static_cast<long long>(bp % 100));
    std::string fraction = buffer;
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    text += fraction;
  }
  return text + "%";
}

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents) {
  if (revenue_cents == 0) return "—";
  // 毛利率保留一位小数，用整数千分比换算，展示层也不引入浮点。
  const std::int64_t direction = revenue_cents < 0 ? -1 : 1;
  const std::int64_t revenue = revenue_cents < 0 ? -revenue_cents : revenue_cents;
  const std::int64_t profit = profit_cents * direction;
  const bool negative = profit < 0;
  const std::int64_t absolute = negative ? -profit : profit;
  const std::int64_t tenths = (absolute * 1000 + revenue / 2) / revenue;
  return (negative ? "-" : "") + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + "%";
}

std::string doc_type_label(const std::string& doc_type) {
  if (doc_type == "SAL_OUT") return "销售出库";
  if (doc_type == "SAL_RET") return "销售退货";
  if (doc_type == "PUR_IN") return "采购入库";
  if (doc_type == "PUR_RET") return "采购退货";
  if (doc_type == "STK_TRF") return "库存调拨";
  if (doc_type == "STK_CNT") return "库存盘点";
  return doc_type;
}

// 库存流水类型标签：盘盈/盘亏与各种 *_REVERSE 统一转中文。
std::string stock_biz_label(const std::string& biz_type) {
  if (biz_type == "REVERSE") return "反审核回冲";
  constexpr std::string_view kReverse = "_REVERSE";
  if (biz_type.size() > kReverse.size() &&
      biz_type.compare(biz_type.size() - kReverse.size(), kReverse.size(), kReverse) == 0) {
    return stock_biz_label(biz_type.substr(0, biz_type.size() - kReverse.size())) + "回冲";
  }
  if (biz_type == "PUR_IN") return "采购入库";
  if (biz_type == "PUR_RET") return "采购退货";
  if (biz_type == "SAL_OUT") return "销售出库";
  if (biz_type == "SAL_RET") return "销售退货";
  if (biz_type == "OTH_IN") return "其他入库";
  if (biz_type == "OTH_OUT") return "其他出库";
  if (biz_type == "STK_TRF") return "调拨出库";
  if (biz_type == "STK_TRF_IN") return "调拨入库";
  if (biz_type == "STK_CNT_IN") return "盘盈";
  if (biz_type == "STK_CNT_OUT") return "盘亏";
  if (biz_type == "STK_CNT") return "盘点调整";
  if (biz_type == "CST_ADJ") return "成本调整";
  return biz_type;
}

std::string qty_direction_label(int direction) {
  if (direction > 0) return "入库";
  if (direction < 0) return "出库";
  return "全部";
}

// 反审核回冲按操作日入账，打印时带上原单据日期便于跨期追溯。
std::string ledger_biz_label(const LedgerDetailRow& row) {
  std::string label = stock_biz_label(row.biz_type);
  if ((row.biz_type == "REVERSE" ||
       (row.biz_type.size() > 8 &&
        row.biz_type.compare(row.biz_type.size() - 8, 8, "_REVERSE") == 0)) &&
      !row.doc_date.empty()) {
    label += "（原单 " + row.doc_date + "）";
  }
  return label;
}

std::string stock_biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

std::string doc_status_label(const std::string& status) {
  if (status == "DRAFT") return "草稿";
  if (status == "APPROVED") return "已审核";
  if (status == "VOIDED") return "已作废";
  return status;
}

// 页眉页脚只做纯文本占位符替换；未知占位符保持原样，不执行任何表达式。
std::string apply_placeholders(std::string text,
                               const std::map<std::string, std::string>& values) {
  for (const auto& [key, value] : values) {
    const std::string token = "{" + key + "}";
    std::string::size_type pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
      text.replace(pos, token.size(), value);
      pos += value.size();
    }
  }
  return text;
}

const char* data_permission(const PrintTargetDef& target) {
  if (target.type == "DOC") return "document.read";
  if (target.key == "sales-profit" || target.key == "sales-customer" ||
      target.key == "sales-salesperson" || target.key == "purchase-detail" ||
      target.key == "purchase-summary-supplier" || target.key == "purchase-summary-item" ||
      target.key == "ar-ap-aging" || target.key == "statement" || target.key == "fund-ledger") {
    return "finance.read";
  }
  return "stock.read";
}

ReportFilter report_filter(const httplib::Request& req) {
  ReportFilter filter;
  filter.from = param_text(req, "from");
  filter.to = param_text(req, "to");
  filter.item_id = param_i64(req, "item_id", 0);
  filter.warehouse_id = param_i64(req, "warehouse_id", 0);
  filter.customer_id = param_i64(req, "customer_id", 0);
  filter.supplier_id = param_i64(req, "supplier_id", 0);
  filter.salesperson_id = param_i64(req, "salesperson_id", 0);
  const std::string flow = param_text(req, "flow");
  if (flow == "in") filter.qty_direction = 1;
  if (flow == "out") filter.qty_direction = -1;
  filter.biz_category = param_text(req, "biz_category");
  filter.limit = static_cast<int>(param_i64(req, "limit", 100000));
  return filter;
}

}  // namespace xiaotu
