#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/edition.h"

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/log.h"
#include "xiaotu/models.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace {

// 账龄口径与应收应付台账一致：只统计 v7 迁移后审核的单据。
constexpr const char* kArApScopeHeader = "X-Xiaotu-Data-Scope";
constexpr const char* kArApScope = "ar_ap_since_v7";

// 查询参数里的整数：非法值要报 400，而不是让 stoll/stoi 抛成 500。
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

// ---------- CSV 导出 ----------

bool csv_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "csv";
}

bool xlsx_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "xlsx";
}

std::string csv_escape(const std::string& value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') out += "\"\"";
    else out += ch;
  }
  out += '"';
  return out;
}

void csv_row(std::string& out, std::initializer_list<std::string> fields) {
  bool first = true;
  for (const auto& field : fields) {
    if (!first) out += ',';
    first = false;
    out += csv_escape(field);
  }
  out += "\r\n";
}

// 金额统一按「元、两位小数」输出；从整数分换算，避免浮点误差。
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

void csv_preamble(std::string& out, const std::string& title, const std::string& scope) {
  csv_row(out, {"报表", title});
  csv_row(out, {"筛选条件", scope});
  csv_row(out, {"导出时间", now_text()});
  csv_row(out, {"金额说明",
                "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  out += "\r\n";
}

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content("\xEF\xBB\xBF" + body, "text/csv; charset=utf-8");
}

void xlsx_preamble(XlsxSheet& sheet, const std::string& title, const std::string& scope) {
  sheet.add_text_row({"报表", title});
  sheet.add_text_row({"筛选条件", scope});
  sheet.add_text_row({"导出时间", now_text()});
  sheet.add_text_row({"金额说明",
                      "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  sheet.add_text_row({});
}

void xlsx_preamble(XlsxStreamWriter& sheet, const std::string& title, const std::string& scope) {
  sheet.add_text_row({"报表", title});
  sheet.add_text_row({"筛选条件", scope});
  sheet.add_text_row({"导出时间", now_text()});
  sheet.add_text_row({"金额说明",
                      "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  sheet.add_text_row({});
}

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content(body,
                  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

constexpr const char* kXlsxContentType =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";

// 大报表 CSV 的写缓冲：攒够一块就交给 HTTP sink，内存占用与导出行数无关。
class CsvStream {
 public:
  CsvStream() { buffer_ = "\xEF\xBB\xBF"; }

  bool raw(httplib::DataSink& sink, const std::string& text) {
    buffer_ += text;
    return buffer_.size() < kFlushBytes || flush(sink);
  }

  bool row(httplib::DataSink& sink, std::initializer_list<std::string> fields) {
    csv_row(buffer_, fields);
    return buffer_.size() < kFlushBytes || flush(sink);
  }

  bool flush(httplib::DataSink& sink) {
    if (buffer_.empty()) return true;
    const bool ok = sink.write(buffer_.data(), buffer_.size());
    buffer_.clear();
    return ok;
  }

 private:
  static constexpr std::size_t kFlushBytes = 256 * 1024;
  std::string buffer_;
};

// 流式 CSV：提供者只被调用一次，写入过程按块推给客户端；返回 false 表示中途写失败。
void send_csv_stream(httplib::Response& res, const std::string& filename,
                     std::function<bool(httplib::DataSink&, CsvStream&)> body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_chunked_content_provider(
      "text/csv; charset=utf-8",
      [body = std::move(body)](std::size_t, httplib::DataSink& sink) {
        CsvStream out;
        bool ok = false;
        // httplib 只在路由阶段包了 try/catch，provider 里抛出的异常没人接，
        // 会沿着线程池 worker 逃出去 terminate 整个进程，这里兜住并中断响应。
        try {
          ok = body(sink, out) && out.flush(sink);
        } catch (const std::exception& error) {
          log_warn(std::string("导出流中断：") + error.what());
          ok = false;
        } catch (...) {
          log_warn("导出流中断：未知异常");
          ok = false;
        }
        sink.done();
        return ok;
      });
}

// 把已生成的 xlsx 临时文件按块写给客户端，响应结束后删掉临时文件。
void send_xlsx_file(httplib::Response& res, const std::string& filename,
                    const std::string& path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open()) {
    std::remove(path.c_str());
    json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
    return;
  }
  file->seekg(0, std::ios::end);
  const auto length = static_cast<std::size_t>(file->tellg());
  file->seekg(0, std::ios::beg);
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content_provider(
      length, kXlsxContentType,
      [file](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
        file->clear();
        file->seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(64 * 1024);
        std::size_t remaining = length;
        while (remaining > 0) {
          const std::size_t chunk = std::min(remaining, buffer.size());
          file->read(buffer.data(), static_cast<std::streamsize>(chunk));
          const auto got = static_cast<std::size_t>(file->gcount());
          if (got == 0) return false;
          if (!sink.write(buffer.data(), got)) return false;
          remaining -= got;
        }
        return true;
      },
      [path](bool) { std::remove(path.c_str()); });
}

std::string date_stamp() {
  std::string stamp = now_text().substr(0, 10);
  stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
  return stamp;
}

nlohmann::json balance_to_json(const StockBalanceRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
  };
}

// 库存流水的类型标签：盘盈/盘亏与各种 *_REVERSE 统一转成中文，导出与页面口径一致。
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

bool is_reverse_biz(const std::string& biz_type) {
  return biz_type == "REVERSE" || (biz_type.size() > 8 &&
                                   biz_type.compare(biz_type.size() - 8, 8, "_REVERSE") == 0);
}

// 反审核回冲按操作日入账，但必须带上原单据日期，否则跨期回冲无法追溯。
std::string ledger_biz_label(const LedgerDetailRow& row) {
  const std::string label = stock_biz_label(row.biz_type);
  if (is_reverse_biz(row.biz_type) && !row.doc_date.empty()) {
    return label + "（原单 " + row.doc_date + "）";
  }
  return label;
}

nlohmann::json ledger_to_json(const LedgerDetailRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"biz_time", row.biz_time},
      {"biz_type", row.biz_type},
      {"biz_type_label", ledger_biz_label(row)},
      {"is_reverse", is_reverse_biz(row.biz_type)},
      {"biz_category", row.biz_category},
      {"doc_no", row.doc_no},
      {"doc_date", row.doc_date},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json flow_to_json(const GoodsFlowRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"opening_qty", qty_text(row.opening_qty)},
      {"in_qty", qty_text(row.in_qty)},
      {"out_qty", qty_text(row.out_qty)},
      {"closing_qty", qty_text(row.closing_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
      {"adjust_amount_cents", row.adjust_amount_cents},
  };
}

nlohmann::json summary_to_json(const InventorySummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"opening_qty", qty_text(row.opening_qty)},
      {"in_qty", qty_text(row.in_qty)},
      {"out_qty", qty_text(row.out_qty)},
      {"closing_qty", qty_text(row.closing_qty)},
      {"opening_amount_cents", row.opening_amount_cents},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
      {"adjust_amount_cents", row.adjust_amount_cents},
      {"closing_amount_cents", row.closing_amount_cents},
  };
}

nlohmann::json aging_to_json(const AgingRow& row) {
  return nlohmann::json{
      {"partner_key", row.partner_key},
      {"partner_name", row.partner_name},
      {"direction", row.direction},
      {"total_cents", row.total_cents},
      {"not_due_cents", row.not_due_cents},
      {"d1_30_cents", row.d1_30_cents},
      {"d31_60_cents", row.d31_60_cents},
      {"d61_90_cents", row.d61_90_cents},
      {"d90_plus_cents", row.d90_plus_cents},
  };
}

std::string method_label(const std::string& method) {
  if (method == "CASH") return "现金";
  if (method == "BANK") return "银行";
  if (method == "WECHAT") return "微信";
  if (method == "ALIPAY") return "支付宝";
  if (method == "OTHER") return "其他";
  return method.empty() ? "未记录" : method;
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

nlohmann::json profit_to_json(const SalesProfitRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"qty", qty_text(row.qty)},
      {"revenue_cents", row.revenue_cents},
      {"cost_cents", row.cost_cents},
      {"profit_cents", row.profit_cents},
  };
}

// 成本口径说明：报表标注当前口径与切换日，避免把切换前后的毛利直接对比。
std::string cost_basis_label(const SystemSettings& settings) {
  std::string label = settings.cost_basis == "NET" ? "不含税净额" : "含税价";
  if (!settings.cost_basis_changed_at.empty()) {
    label += "（切换于 " + settings.cost_basis_changed_at + "）";
  }
  return label;
}

std::string biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

// 采购报表口径说明：入库与退货同表，退货按负数冲减，只统计已审核单据。
std::string purchase_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" + dimension_label(filter.supplier_id, "供应商") + "；" +
         dimension_label(filter.item_id, "商品") + "；" +
         dimension_label(filter.warehouse_id, "仓库") +
         "；包含采购入库与采购退货，退货数量与金额按负数冲减，仅统计已审核单据";
}

nlohmann::json other_flow_to_json(const OtherFlowSummaryRow& row) {
  return nlohmann::json{
      {"category", row.category},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"out_qty", qty_text(row.out_qty)},
      {"out_amount_cents", row.out_amount_cents},
  };
}

std::string other_flow_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" +
         dimension_label(filter.warehouse_id, "仓库") + "；业务类型 " +
         (filter.biz_category.empty() ? std::string("全部")
                                      : biz_category_label(filter.biz_category)) +
         "；仅统计已审核的其他出入库，出库数量与金额取绝对值";
}

nlohmann::json purchase_detail_to_json(const PurchaseDetailRow& row) {
  return nlohmann::json{
      {"date", row.date},
      {"doc_no", row.doc_no},
      {"kind", row.kind},
      {"supplier", row.supplier},
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"price_cents", row.price_cents},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json purchase_supplier_to_json(const PurchaseSupplierSummaryRow& row) {
  return nlohmann::json{
      {"partner_id", row.partner_id},
      {"supplier", row.supplier},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"return_amount_cents", row.return_amount_cents},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"has_inbound", row.has_inbound},
      {"avg_price_cents", row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                          : nlohmann::json(nullptr)},
  };
}

nlohmann::json sales_summary_to_json(const SalesSummaryRow& row, std::int64_t total_net,
                                     bool by_salesperson) {
  nlohmann::json json{{"group_id", row.group_id},
                      {"name", row.name},
                      {"orders", row.orders},
                      {"qty", qty_text(row.qty)},
                      {"sales_cents", row.sales_cents},
                      {"return_cents", row.return_cents},
                      {"net_cents", row.net_cents},
                      {"cost_cents", row.cost_cents},
                      {"profit_cents", row.profit_cents},
                      {"margin", margin_text(row.net_cents, row.profit_cents)},
                      {"ratio", margin_text(total_net, row.net_cents)}};
  if (by_salesperson) json["customers"] = row.customers;
  return json;
}

nlohmann::json sales_detail_to_json(const SalesDetailRow& row) {
  return nlohmann::json{{"date", row.date},
                        {"doc_no", row.doc_no},
                        {"customer", row.customer},
                        {"salesperson", row.salesperson},
                        {"item_code", row.item_code},
                        {"item_name", row.item_name},
                        {"spec", row.spec},
                        {"unit", row.unit},
                        {"qty", qty_text(row.qty)},
                        {"price_cents", row.price_cents},
                        {"amount_cents", row.amount_cents},
                        {"cost_cents", row.cost_cents},
                        {"profit_cents", row.amount_cents - row.cost_cents}};
}

nlohmann::json purchase_item_to_json(const PurchaseItemSummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"return_amount_cents", row.return_amount_cents},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"has_inbound", row.has_inbound},
      {"avg_price_cents", row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                          : nlohmann::json(nullptr)},
  };
}

nlohmann::json purchase_totals_to_json(const PurchaseSummaryTotals& totals) {
  return nlohmann::json{
      {"in_qty", qty_text(totals.in_qty)},
      {"in_amount_cents", totals.in_amount_cents},
      {"return_amount_cents", totals.return_amount_cents},
      {"qty", qty_text(totals.qty)},
      {"amount_cents", totals.amount_cents},
      {"has_inbound", totals.has_inbound},
      {"avg_price_cents", totals.has_inbound ? nlohmann::json(totals.avg_price_cents)
                                             : nlohmann::json(nullptr)},
  };
}

std::string doc_type_label(const std::string& doc_type) {
  if (doc_type == "SAL_OUT") return "销售出库";
  if (doc_type == "SAL_RET") return "销售退货";
  if (doc_type == "PUR_IN") return "采购入库";
  if (doc_type == "PUR_RET") return "采购退货";
  return doc_type;
}

// 库存流水的类型标签见文件上方的 stock_biz_label / ledger_biz_label。

std::string qty_direction_label(int direction) {
  if (direction > 0) return "入库";
  if (direction < 0) return "出库";
  return "全部";
}

std::string statement_kind_label(const StatementResult& result, const StatementLine& line) {
  if (line.kind == "DOC") return "单据";
  const std::string base = result.direction == -1 ? "付款" : "收款";
  return line.is_reverse ? base + "撤销" : base;
}

std::string statement_summary(const StatementResult& result, const StatementLine& line) {
  if (line.kind == "DOC") return doc_type_label(line.doc_type);
  if (!line.is_reverse && !line.note.empty()) return line.note;
  std::string summary;
  if (line.is_reverse) {
    summary = std::string("撤销") + (result.direction == -1 ? "付款" : "收款");
    if (!line.reverses_no.empty()) summary += "（原单 " + line.reverses_no + "）";
  } else {
    summary = statement_kind_label(result, line);
  }
  if (!line.note.empty()) summary += "；" + line.note;
  return summary;
}

nlohmann::json statement_to_json(const StatementResult& result) {
  nlohmann::json lines = nlohmann::json::array();
  for (const auto& line : result.lines) {
    lines.push_back(nlohmann::json{
        {"date", line.date},
        {"kind", line.kind},
        {"kind_label", statement_kind_label(result, line)},
        {"doc_no", line.doc_no},
        {"summary", statement_summary(result, line)},
        {"is_reverse", line.is_reverse},
        {"reverses_no", line.reverses_no},
        {"increase_cents", line.increase_cents},
        {"decrease_cents", line.decrease_cents},
        {"balance_cents", line.balance_cents},
    });
  }
  return nlohmann::json{
      {"partner_key", result.partner_key},
      {"partner_name", result.partner_name},
      {"direction", result.direction},
      {"from", result.from},
      {"to", result.to},
      {"opening_cents", result.opening_cents},
      {"increase_cents", result.increase_cents},
      {"decrease_cents", result.decrease_cents},
      {"closing_cents", result.closing_cents},
      {"settle_net_cents", result.settle_net_cents},
      {"reverse_cents", result.reverse_cents},
      {"reverse_count", result.reverse_count},
      {"lines", lines},
  };
}

}  // namespace

void register_report_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/stock-balance",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? std::stoll(req.get_param_value("warehouse_id"))
                             : 0;
                     const std::int64_t item_id =
                         req.has_param("item_id") ? std::stoll(req.get_param_value("item_id")) : 0;
                     const bool hide_zero = req.has_param("hide_zero") &&
                                            req.get_param_value("hide_zero") != "0";
                     const auto rows = services.stock.balances(warehouse_id, item_id, hide_zero);
                     // 数量跨单位不可加总，合计行只汇总金额并标注行数。
                     std::int64_t balance_amount = 0;
                     for (const auto& row : rows) balance_amount += row.amount_cents;
                     const std::string balance_total_label =
                         "合计 " + std::to_string(rows.size()) + " 行";
                     const std::string scope =
                         dimension_label(warehouse_id, "仓库") + "；" +
                         dimension_label(item_id, "商品") +
                         (hide_zero ? "；隐藏零库存" : "；包含零库存") +
                         "；数量跨单位不可加总，合计只汇总金额";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "库存余额表", scope);
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.qty), yuan(row.amount_cents)});
                       }
                       csv_row(csv, {balance_total_label, "", "", "", "", "",
                                     yuan(balance_amount)});
                       send_csv(res, "stock-balance-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("库存余额表");
                       xlsx_preamble(sheet, "库存余额表", scope);
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text(balance_total_label),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::money(balance_amount)});
                       send_xlsx(res, "stock-balance-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/inventory-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.inventory_summary(filter);
                     // 数量跨单位不可加总，合计行只汇总五个金额列。
                     std::int64_t sum_opening = 0;
                     std::int64_t sum_in = 0;
                     std::int64_t sum_out = 0;
                     std::int64_t sum_adjust = 0;
                     std::int64_t sum_closing = 0;
                     for (const auto& row : rows) {
                       sum_opening += row.opening_amount_cents;
                       sum_in += row.in_amount_cents;
                       sum_out += row.out_amount_cents;
                       sum_adjust += row.adjust_amount_cents;
                       sum_closing += row.closing_amount_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "进销存汇总",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") + "；" +
                                        dimension_label(filter.warehouse_id, "仓库") +
                                        "；按商品跨仓库汇总；数量跨单位不可加总，合计只汇总金额；"
                                        "数量为 0、只动金额的流水单列「其他调整」");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "期初数量",
                                     "期初金额（元）", "入库数量", "入库金额（元）",
                                     "出库数量", "出库金额（元）", "其他调整（元）",
                                     "期末数量", "期末金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.opening_qty),
                                       yuan(row.opening_amount_cents),
                                       qty_text(row.in_qty), yuan(row.in_amount_cents),
                                       qty_text(row.out_qty), yuan(row.out_amount_cents),
                                       yuan(row.adjust_amount_cents),
                                       qty_text(row.closing_qty),
                                       yuan(row.closing_amount_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", yuan(sum_opening), "",
                                     yuan(sum_in), "", yuan(sum_out), yuan(sum_adjust), "",
                                     yuan(sum_closing)});
                       send_csv(res, "inventory-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("进销存汇总");
                       xlsx_preamble(sheet, "进销存汇总",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；" +
                                         dimension_label(filter.warehouse_id, "仓库") +
                                         "；按商品跨仓库汇总；数量跨单位不可加总，合计只汇总金额；"
                                         "数量为 0、只动金额的流水单列「其他调整」");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "期初数量",
                                             "期初金额（元）", "入库数量", "入库金额（元）",
                                             "出库数量", "出库金额（元）", "其他调整（元）",
                                             "期末数量", "期末金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::money(row.opening_amount_cents),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents),
                                        XlsxCell::money(row.adjust_amount_cents),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.closing_amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(sum_opening), XlsxCell::text(""),
                                      XlsxCell::money(sum_in), XlsxCell::text(""),
                                      XlsxCell::money(sum_out),
                                      XlsxCell::money(sum_adjust), XlsxCell::text(""),
                                      XlsxCell::money(sum_closing)});
                       send_xlsx(res, "inventory-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(summary_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/stock-ledger",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const std::string flow =
                         req.has_param("flow") ? req.get_param_value("flow") : "";
                     if (flow == "in") filter.qty_direction = 1;
                     if (flow == "out") filter.qty_direction = -1;
                     if (req.has_param("biz_category")) {
                       filter.biz_category = req.get_param_value("biz_category");
                     }
                     const auto totals = services.reports.stock_ledger_totals(filter);
                     const auto settings = services.settings.load();
                     const std::string base_scope =
                         period_label(filter) + "；" +
                         dimension_label(filter.item_id, "商品") + "；" +
                         dimension_label(filter.warehouse_id, "仓库") + "；方向 " +
                         qty_direction_label(filter.qty_direction) + "；业务类型 " +
                         (filter.biz_category.empty() ? std::string("全部")
                                                      : biz_category_label(filter.biz_category)) +
                         "；成本口径 " + cost_basis_label(settings) + "；共 " +
                         std::to_string(totals.rows) + " 行";
                     if (csv_requested(req) || xlsx_requested(req)) {
                       // 导出不分页、不截断：边查边写，内存占用与总行数无关。
                       ReportFilter export_filter = filter;
                       export_filter.limit = 0;
                       export_filter.offset = 0;
                       const std::string scope = base_scope + "；全部导出";
                       if (csv_requested(req)) {
                         send_csv_stream(
                             res, "stock-ledger-" + date_stamp() + ".csv",
                             [&services, export_filter, scope, totals](httplib::DataSink& sink,
                                                                       CsvStream& out) {
                               std::string preamble;
                               csv_preamble(preamble, "库存流水明细", scope);
                               if (!out.raw(sink, preamble)) return false;
                               if (!out.row(sink, {"时间", "类型", "业务类型", "单据号",
                                                   "商品编码", "商品名称", "仓库", "数量",
                                                   "金额（元）"})) {
                                 return false;
                               }
                               const bool streamed = services.reports.stream_stock_ledger(
                                   export_filter, [&](const LedgerDetailRow& row) {
                                     return out.row(
                                         sink, {row.biz_time, ledger_biz_label(row),
                                                biz_category_label(row.biz_category),
                                                row.doc_no, row.item_code, row.item_name,
                                                row.warehouse_name, qty_text(row.qty),
                                                yuan(row.amount_cents)});
                                   });
                               if (!streamed) return false;
                               return out.row(sink, {"合计", "", "", "", "", "", "",
                                                     qty_text(totals.qty),
                                                     yuan(totals.amount_cents)});
                             });
                         return;
                       }
                       XlsxStreamWriter sheet("库存流水明细");
                       xlsx_preamble(sheet, "库存流水明细", scope);
                       sheet.add_header_row({"时间", "类型", "业务类型", "单据号", "商品编码",
                                             "商品名称", "仓库", "数量", "金额（元）"});
                       const bool streamed = services.reports.stream_stock_ledger(
                           export_filter, [&](const LedgerDetailRow& row) {
                             sheet.add_row({XlsxCell::text(row.biz_time),
                                            XlsxCell::text(ledger_biz_label(row)),
                                            XlsxCell::text(biz_category_label(row.biz_category)),
                                            XlsxCell::text(row.doc_no),
                                            XlsxCell::text(row.item_code),
                                            XlsxCell::text(row.item_name),
                                            XlsxCell::text(row.warehouse_name),
                                            XlsxCell::text(qty_text(row.qty)),
                                            XlsxCell::money(row.amount_cents)});
                             return sheet.ok();
                           });
                       if (streamed) {
                         sheet.add_row({XlsxCell::text("合计"),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::money(totals.amount_cents)});
                       }
                       const std::string path = sheet.finish();
                       if (path.empty()) {
                         json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
                         return;
                       }
                       send_xlsx_file(res, "stock-ledger-" + date_stamp() + ".xlsx", path);
                       return;
                     }
                     const auto rows = services.reports.stock_ledger(filter);
                     const std::int64_t last_row =
                         static_cast<std::int64_t>(filter.offset + rows.size());
                     const bool truncated = totals.rows > last_row;
                     const std::string scope =
                         base_scope +
                         (rows.empty() ? std::string()
                                       : "，当前显示第 " +
                                             std::to_string(filter.offset + 1) + "–" +
                                             std::to_string(last_row) + " 行") +
                         (truncated ? "；可翻页或导出查看全部" : std::string());
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(ledger_to_json(row));
                     }
                     json_ok(res, nlohmann::json{{"total", totals.rows},
                                                 {"limit", filter.limit},
                                                 {"offset", filter.offset},
                                                 {"truncated", truncated},
                                                 {"cost_basis", settings.cost_basis},
                                                 {"cost_basis_changed_at",
                                                  settings.cost_basis_changed_at},
                                                 {"rows", list},
                                                 {"totals",
                                                  {{"qty", qty_text(totals.qty)},
                                                   {"amount_cents", totals.amount_cents}}}});
                   }));

  server.Get("/api/reports/goods-flow",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.goods_flow(filter);
                     // 数量跨单位不可加总，合计行只汇总收入 / 发出 / 其他调整金额。
                     std::int64_t flow_in_amount = 0;
                     std::int64_t flow_out_amount = 0;
                     std::int64_t flow_adjust_amount = 0;
                     for (const auto& row : rows) {
                       flow_in_amount += row.in_amount_cents;
                       flow_out_amount += row.out_amount_cents;
                       flow_adjust_amount += row.adjust_amount_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "收发明细",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.warehouse_id, "仓库") +
                                        "；数量跨单位不可加总，合计只汇总金额；"
                                        "数量为 0、只动金额的流水单列「其他调整」");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "期初数量", "收入数量", "发出数量", "期末数量",
                                     "收入金额（元）", "发出金额（元）", "其他调整（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.opening_qty),
                                       qty_text(row.in_qty), qty_text(row.out_qty),
                                       qty_text(row.closing_qty),
                                       yuan(row.in_amount_cents), yuan(row.out_amount_cents),
                                       yuan(row.adjust_amount_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", "", "", "", "", "",
                                     yuan(flow_in_amount), yuan(flow_out_amount),
                                     yuan(flow_adjust_amount)});
                       send_csv(res, "goods-flow-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("收发明细");
                       xlsx_preamble(sheet, "收发明细",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.warehouse_id, "仓库") +
                                         "；数量跨单位不可加总，合计只汇总金额；"
                                         "数量为 0、只动金额的流水单列「其他调整」");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "期初数量", "收入数量", "发出数量",
                                             "期末数量", "收入金额（元）", "发出金额（元）",
                                             "其他调整（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::money(row.out_amount_cents),
                                        XlsxCell::money(row.adjust_amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(flow_in_amount),
                                      XlsxCell::money(flow_out_amount),
                                      XlsxCell::money(flow_adjust_amount)});
                       send_xlsx(res, "goods-flow-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/sales-profit",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     EditionPolicy::instance().require("report.profit");
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.sales_profit(filter);
                     const auto settings = services.settings.load();
                     // 数量跨单位不可加总；合计行汇总收入、成本、毛利并重算毛利率。
                     std::int64_t profit_revenue = 0;
                     std::int64_t profit_cost = 0;
                     std::int64_t profit_total = 0;
                     for (const auto& row : rows) {
                       profit_revenue += row.revenue_cents;
                       profit_cost += row.cost_cents;
                       profit_total += row.profit_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "销售毛利",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") +
                                        "；客户 " +
                                        (filter.customer_id == 0
                                             ? std::string("全部")
                                             : "ID " + std::to_string(filter.customer_id)) +
                                        "；收入取单据金额，成本取审核写入的库存流水，"
                                        "已反审核单据不计入；成本口径 " +
                                        cost_basis_label(settings));
                       csv_row(csv, {"商品编码", "商品名称", "单位", "销售数量",
                                     "销售收入（元）", "销售成本（元）", "毛利（元）", "毛利率"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.qty), yuan(row.revenue_cents),
                                       yuan(row.cost_cents), yuan(row.profit_cents),
                                       margin_text(row.revenue_cents, row.profit_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", yuan(profit_revenue),
                                     yuan(profit_cost), yuan(profit_total),
                                     margin_text(profit_revenue, profit_total)});
                       send_csv(res, "sales-profit-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("销售毛利");
                       xlsx_preamble(sheet, "销售毛利",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；客户 " +
                                         (filter.customer_id == 0
                                              ? std::string("全部")
                                              : "ID " + std::to_string(filter.customer_id)) +
                                         "；收入取单据金额，成本取审核写入的库存流水，"
                                         "已反审核单据不计入；成本口径 " +
                                         cost_basis_label(settings));
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "销售数量",
                                             "销售收入（元）", "销售成本（元）", "毛利（元）",
                                             "毛利率"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.revenue_cents),
                                        XlsxCell::money(row.cost_cents),
                                        XlsxCell::money(row.profit_cents),
                                        XlsxCell::text(
                                            margin_text(row.revenue_cents, row.profit_cents))});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(profit_revenue),
                                      XlsxCell::money(profit_cost),
                                      XlsxCell::money(profit_total),
                                      XlsxCell::text(margin_text(profit_revenue, profit_total))});
                       send_xlsx(res, "sales-profit-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       auto item = profit_to_json(row);
                       // 每行标注当前成本口径与切换日，避免拿切换前后的毛利直接比较。
                       item["cost_basis"] = settings.cost_basis;
                       item["cost_basis_changed_at"] = settings.cost_basis_changed_at;
                       list.push_back(item);
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/sales-summary",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const bool by_salesperson =
                         req.has_param("dimension") &&
                         req.get_param_value("dimension") == "salesperson";
                     const auto rows = services.reports.sales_summary(
                         filter, by_salesperson ? "salesperson" : "customer");
                     std::int64_t total_orders = 0;
                     std::int64_t total_qty = 0;
                     std::int64_t total_sales = 0;
                     std::int64_t total_return = 0;
                     std::int64_t total_net = 0;
                     std::int64_t total_cost = 0;
                     std::int64_t total_profit = 0;
                     for (const auto& row : rows) {
                       total_orders += row.orders;
                       total_qty += row.qty;
                       total_sales += row.sales_cents;
                       total_return += row.return_cents;
                       total_net += row.net_cents;
                       total_cost += row.cost_cents;
                       total_profit += row.profit_cents;
                     }
                     const std::string title =
                         by_salesperson ? "销售报表（按业务员）" : "销售报表（按客户）";
                     const std::string subtitle =
                         period_label(filter) + "；只计已审核的销售出库与销售退货；" +
                         "退货按负数冲减；毛利按出库时的移动加权成本";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, title, subtitle);
                       if (by_salesperson) {
                         csv_row(csv, {"业务员", "单数", "客户数", "销售数量", "销售额（元）",
                                       "退货额（元）", "净销售额（元）", "成本（元）",
                                       "毛利（元）", "毛利率", "销售额占比"});
                       } else {
                         csv_row(csv, {"客户", "单数", "销售数量", "销售额（元）",
                                       "退货额（元）", "净销售额（元）", "成本（元）",
                                       "毛利（元）", "毛利率", "销售额占比"});
                       }
                       for (const auto& row : rows) {
                         const std::string ratio = margin_text(total_net, row.net_cents);
                         if (by_salesperson) {
                           csv_row(csv, {row.name, std::to_string(row.orders),
                                         std::to_string(row.customers), qty_text(row.qty),
                                         yuan(row.sales_cents), yuan(row.return_cents),
                                         yuan(row.net_cents), yuan(row.cost_cents),
                                         yuan(row.profit_cents),
                                         margin_text(row.net_cents, row.profit_cents), ratio});
                         } else {
                           csv_row(csv, {row.name, std::to_string(row.orders),
                                         qty_text(row.qty), yuan(row.sales_cents),
                                         yuan(row.return_cents), yuan(row.net_cents),
                                         yuan(row.cost_cents), yuan(row.profit_cents),
                                         margin_text(row.net_cents, row.profit_cents), ratio});
                         }
                       }
                       if (by_salesperson) {
                         csv_row(csv, {"合计", std::to_string(total_orders), "",
                                       qty_text(total_qty), yuan(total_sales),
                                       yuan(total_return), yuan(total_net), yuan(total_cost),
                                       yuan(total_profit),
                                       margin_text(total_net, total_profit), "100.00%"});
                       } else {
                         csv_row(csv, {"合计", std::to_string(total_orders),
                                       qty_text(total_qty), yuan(total_sales),
                                       yuan(total_return), yuan(total_net), yuan(total_cost),
                                       yuan(total_profit),
                                       margin_text(total_net, total_profit), "100.00%"});
                       }
                       send_csv(res, std::string(by_salesperson ? "sales-salesperson-"
                                                                : "sales-customer-") +
                                         date_stamp() + ".csv",
                                csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet(title);
                       xlsx_preamble(sheet, title, subtitle);
                       if (by_salesperson) {
                         sheet.add_header_row({"业务员", "单数", "客户数", "销售数量",
                                               "销售额（元）", "退货额（元）", "净销售额（元）",
                                               "成本（元）", "毛利（元）", "毛利率",
                                               "销售额占比"});
                       } else {
                         sheet.add_header_row({"客户", "单数", "销售数量", "销售额（元）",
                                               "退货额（元）", "净销售额（元）", "成本（元）",
                                               "毛利（元）", "毛利率", "销售额占比"});
                       }
                       for (const auto& row : rows) {
                         const std::string ratio = margin_text(total_net, row.net_cents);
                         std::vector<XlsxCell> cells;
                         cells.push_back(XlsxCell::text(row.name));
                         cells.push_back(XlsxCell::integer(row.orders));
                         if (by_salesperson) {
                           cells.push_back(XlsxCell::integer(row.customers));
                         }
                         cells.push_back(XlsxCell::text(qty_text(row.qty)));
                         cells.push_back(XlsxCell::money(row.sales_cents));
                         cells.push_back(XlsxCell::money(row.return_cents));
                         cells.push_back(XlsxCell::money(row.net_cents));
                         cells.push_back(XlsxCell::money(row.cost_cents));
                         cells.push_back(XlsxCell::money(row.profit_cents));
                         cells.push_back(XlsxCell::text(
                             margin_text(row.net_cents, row.profit_cents)));
                         cells.push_back(XlsxCell::text(ratio));
                         sheet.add_row(cells);
                       }
                       std::vector<XlsxCell> total_cells;
                       total_cells.push_back(XlsxCell::text("合计"));
                       total_cells.push_back(XlsxCell::integer(total_orders));
                       if (by_salesperson) total_cells.push_back(XlsxCell::text(""));
                       total_cells.push_back(XlsxCell::text(qty_text(total_qty)));
                       total_cells.push_back(XlsxCell::money(total_sales));
                       total_cells.push_back(XlsxCell::money(total_return));
                       total_cells.push_back(XlsxCell::money(total_net));
                       total_cells.push_back(XlsxCell::money(total_cost));
                       total_cells.push_back(XlsxCell::money(total_profit));
                       total_cells.push_back(
                           XlsxCell::text(margin_text(total_net, total_profit)));
                       total_cells.push_back(XlsxCell::text("100.00%"));
                       sheet.add_row(total_cells);
                       send_xlsx(res, std::string(by_salesperson ? "sales-salesperson-"
                                                                 : "sales-customer-") +
                                         date_stamp() + ".xlsx",
                                sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(sales_summary_to_json(row, total_net, by_salesperson));
                     }
                     nlohmann::json totals{{"orders", total_orders},
                                           {"qty", qty_text(total_qty)},
                                           {"sales_cents", total_sales},
                                           {"return_cents", total_return},
                                           {"net_cents", total_net},
                                           {"cost_cents", total_cost},
                                           {"profit_cents", total_profit},
                                           {"margin", margin_text(total_net, total_profit)}};
                     json_ok(res, nlohmann::json{{"rows", list}, {"totals", totals}});
                   }));

  server.Get("/api/reports/sales-detail",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const auto totals = services.reports.sales_detail_totals(filter);
                     const std::string title = "销售明细";
                     const std::string scope =
                         period_label(filter) + "；只计已审核的销售出库与销售退货；共 " +
                         std::to_string(totals.rows) + " 行";
                     if (csv_requested(req) || xlsx_requested(req)) {
                       // 导出不分页、不截断：边查边写，内存占用与总行数无关。
                       ReportFilter export_filter = filter;
                       export_filter.limit = 0;
                       export_filter.offset = 0;
                       if (csv_requested(req)) {
                         send_csv_stream(
                             res, "sales-detail-" + date_stamp() + ".csv",
                             [&services, export_filter, title, scope,
                              totals](httplib::DataSink& sink, CsvStream& out) {
                               std::string preamble;
                               csv_preamble(preamble, title, scope + "；全部导出");
                               if (!out.raw(sink, preamble)) return false;
                               if (!out.row(sink, {"日期", "单号", "客户", "业务员", "商品编码",
                                                   "商品名称", "规格", "单位", "数量",
                                                   "单价（元）", "金额（元）", "成本（元）",
                                                   "毛利（元）"})) {
                                 return false;
                               }
                               const bool streamed = services.reports.stream_sales_detail(
                                   export_filter, [&](const SalesDetailRow& row) {
                                     return out.row(
                                         sink, {row.date, row.doc_no, row.customer,
                                                row.salesperson, row.item_code, row.item_name,
                                                row.spec, row.unit, qty_text(row.qty),
                                                yuan(row.price_cents), yuan(row.amount_cents),
                                                yuan(row.cost_cents),
                                                yuan(row.amount_cents - row.cost_cents)});
                                   });
                               if (!streamed) return false;
                               return out.row(sink, {"合计", "", "", "", "", "", "", "",
                                                     qty_text(totals.qty), "",
                                                     yuan(totals.amount_cents),
                                                     yuan(totals.cost_cents),
                                                     yuan(totals.profit_cents)});
                             });
                         return;
                       }
                       XlsxStreamWriter sheet(title);
                       xlsx_preamble(sheet, title, scope + "；全部导出");
                       sheet.add_header_row({"日期", "单号", "客户", "业务员", "商品编码",
                                             "商品名称", "规格", "单位", "数量", "单价（元）",
                                             "金额（元）", "成本（元）", "毛利（元）"});
                       const bool streamed = services.reports.stream_sales_detail(
                           export_filter, [&](const SalesDetailRow& row) {
                             sheet.add_row({XlsxCell::text(row.date),
                                            XlsxCell::text(row.doc_no),
                                            XlsxCell::text(row.customer),
                                            XlsxCell::text(row.salesperson),
                                            XlsxCell::text(row.item_code),
                                            XlsxCell::text(row.item_name),
                                            XlsxCell::text(row.spec), XlsxCell::text(row.unit),
                                            XlsxCell::text(qty_text(row.qty)),
                                            XlsxCell::money(row.price_cents),
                                            XlsxCell::money(row.amount_cents),
                                            XlsxCell::money(row.cost_cents),
                                            XlsxCell::money(row.amount_cents - row.cost_cents)});
                             return sheet.ok();
                           });
                       if (streamed) {
                         sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::text(""),
                                        XlsxCell::money(totals.amount_cents),
                                        XlsxCell::money(totals.cost_cents),
                                        XlsxCell::money(totals.profit_cents)});
                       }
                       const std::string path = sheet.finish();
                       if (path.empty()) {
                         json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
                         return;
                       }
                       send_xlsx_file(res, "sales-detail-" + date_stamp() + ".xlsx", path);
                       return;
                     }
                     const auto rows = services.reports.sales_detail(filter, "customer");
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) list.push_back(sales_detail_to_json(row));
                     json_ok(res, nlohmann::json{
                                     {"rows", list},
                                     {"total", totals.rows},
                                     {"limit", filter.limit},
                                     {"offset", filter.offset},
                                     {"totals",
                                      {{"qty", qty_text(totals.qty)},
                                       {"amount_cents", totals.amount_cents},
                                       {"cost_cents", totals.cost_cents},
                                       {"profit_cents", totals.profit_cents}}}});
                   }));

  server.Get("/api/reports/dashboard",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     EditionPolicy::instance().require("report.dashboard");
                     const std::string today = req.has_param("today")
                                                   ? req.get_param_value("today")
                                                   : today_text();
                     const auto result = services.reports.dashboard(today);
                     nlohmann::json debtors = nlohmann::json::array();
                     for (const auto& debtor : result.top_debtors) {
                       debtors.push_back(nlohmann::json{{"partner_id", debtor.partner_id},
                                                        {"name", debtor.name},
                                                        {"outstanding_cents",
                                                         debtor.outstanding_cents}});
                     }
                     json_ok(res, nlohmann::json{
                                     {"as_of", result.today},
                                     {"month_start", result.month_start},
                                     {"today",
                                      {{"sales_cents", result.today_sales_cents},
                                       {"return_cents", result.today_return_cents},
                                       {"net_cents", result.today_sales_cents -
                                                         result.today_return_cents},
                                       {"cost_cents", result.today_cost_cents},
                                       {"profit_cents", result.today_profit_cents}}},
                                     {"month",
                                      {{"sales_cents", result.month_sales_cents},
                                       {"return_cents", result.month_return_cents},
                                       {"net_cents", result.month_sales_cents -
                                                         result.month_return_cents},
                                       {"cost_cents", result.month_cost_cents},
                                       {"profit_cents", result.month_profit_cents}}},
                                     {"stock_value_cents", result.stock_value_cents},
                                     {"top_debtors", debtors}});
                   }));

  server.Get("/api/reports/purchase-detail",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const auto totals = services.reports.purchase_detail_totals(filter);
                     const std::string base_scope =
                         purchase_scope(filter) + "；共 " + std::to_string(totals.rows) + " 行";
                     if (csv_requested(req) || xlsx_requested(req)) {
                       // 导出不分页、不截断：边查边写，内存占用与总行数无关。
                       ReportFilter export_filter = filter;
                       export_filter.limit = 0;
                       export_filter.offset = 0;
                       const std::string scope = base_scope + "；全部导出";
                       if (csv_requested(req)) {
                         send_csv_stream(
                             res, "purchase-detail-" + date_stamp() + ".csv",
                             [&services, export_filter, scope, totals](httplib::DataSink& sink,
                                                                      CsvStream& out) {
                               std::string preamble;
                               csv_preamble(preamble, "采购明细", scope);
                               if (!out.raw(sink, preamble)) return false;
                               if (!out.row(sink, {"日期", "单号", "类型", "供应商",
                                                   "商品编码", "商品名称", "规格", "单位",
                                                   "仓库", "数量", "单价（元）",
                                                   "金额（元）"})) {
                                 return false;
                               }
                               const bool streamed = services.reports.stream_purchase_detail(
                                   export_filter, [&](const PurchaseDetailRow& row) {
                                     return out.row(
                                         sink, {row.date, row.doc_no, row.kind, row.supplier,
                                                row.item_code, row.item_name, row.spec,
                                                row.unit, row.warehouse_name,
                                                qty_text(row.qty), yuan(row.price_cents),
                                                yuan(row.amount_cents)});
                                   });
                               if (!streamed) return false;
                               return out.row(sink, {"合计", "", "", "", "", "", "", "", "",
                                                     qty_text(totals.qty), "",
                                                     yuan(totals.amount_cents)});
                             });
                         return;
                       }
                       XlsxStreamWriter sheet("采购明细");
                       xlsx_preamble(sheet, "采购明细", scope);
                       sheet.add_header_row({"日期", "单号", "类型", "供应商", "商品编码",
                                             "商品名称", "规格", "单位", "仓库", "数量",
                                             "单价（元）", "金额（元）"});
                       const bool streamed = services.reports.stream_purchase_detail(
                           export_filter, [&](const PurchaseDetailRow& row) {
                             sheet.add_row({XlsxCell::text(row.date),
                                            XlsxCell::text(row.doc_no),
                                            XlsxCell::text(row.kind),
                                            XlsxCell::text(row.supplier),
                                            XlsxCell::text(row.item_code),
                                            XlsxCell::text(row.item_name),
                                            XlsxCell::text(row.spec),
                                            XlsxCell::text(row.unit),
                                            XlsxCell::text(row.warehouse_name),
                                            XlsxCell::text(qty_text(row.qty)),
                                            XlsxCell::money(row.price_cents),
                                            XlsxCell::money(row.amount_cents)});
                             return sheet.ok();
                           });
                       if (streamed) {
                         sheet.add_row({XlsxCell::text("合计"),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::text(""),
                                        XlsxCell::money(totals.amount_cents)});
                       }
                       const std::string path = sheet.finish();
                       if (path.empty()) {
                         json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
                         return;
                       }
                       send_xlsx_file(res, "purchase-detail-" + date_stamp() + ".xlsx", path);
                       return;
                     }
                     const auto rows = services.reports.purchase_detail(filter);
                     const std::int64_t last_row =
                         static_cast<std::int64_t>(filter.offset + rows.size());
                     const bool truncated = totals.rows > last_row;
                     const std::string scope =
                         base_scope +
                         (rows.empty() ? std::string()
                                       : "，当前显示第 " +
                                             std::to_string(filter.offset + 1) + "–" +
                                             std::to_string(last_row) + " 行") +
                         (truncated ? "；可翻页或导出查看全部" : std::string());
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_detail_to_json(row));
                     }
                     json_ok(res, nlohmann::json{{"total", totals.rows},
                                                 {"limit", filter.limit},
                                                 {"offset", filter.offset},
                                                 {"truncated", truncated},
                                                 {"rows", list},
                                                 {"totals",
                                                  {{"qty", qty_text(totals.qty)},
                                                   {"amount_cents", totals.amount_cents}}}});
                   }));

  // 采购汇总：by=supplier（默认）按供应商名称升序，by=item 按商品编码升序。
  server.Get("/api/reports/purchase-summary",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const std::string by =
                         req.has_param("by") ? req.get_param_value("by") : std::string("supplier");
                     const bool by_item = by == "item";
                     const auto totals = services.reports.purchase_summary_totals(filter);
                     const std::string avg_text =
                         totals.has_inbound ? yuan(totals.avg_price_cents) : "—";
                     const std::string scope = purchase_scope(filter) + "；维度 " +
                                               (by_item ? "按商品" : "按供应商") +
                                               "；均价 = 入库金额 ÷ 入库数量（只计入库，退货不参与）";

                     if (by_item) {
                       const auto rows = services.reports.purchase_summary_by_item(filter);
                       if (csv_requested(req)) {
                         std::string csv;
                         csv_preamble(csv, "采购汇总（按商品）", scope);
                         csv_row(csv, {"商品编码", "商品名称", "规格", "单位", "入库数量",
                                       "入库金额（元）", "退货额（元）", "净数量", "净金额（元）",
                                       "入库均价（元）"});
                         for (const auto& row : rows) {
                           csv_row(csv, {row.item_code, row.item_name, row.spec, row.unit,
                                         qty_text(row.in_qty), yuan(row.in_amount_cents),
                                         yuan(row.return_amount_cents), qty_text(row.qty),
                                         yuan(row.amount_cents),
                                         row.has_inbound ? yuan(row.avg_price_cents) : "—"});
                         }
                         csv_row(csv, {"合计", "", "", "", qty_text(totals.in_qty),
                                       yuan(totals.in_amount_cents),
                                       yuan(totals.return_amount_cents), qty_text(totals.qty),
                                       yuan(totals.amount_cents), avg_text});
                         send_csv(res, "purchase-summary-item-" + date_stamp() + ".csv", csv);
                         return;
                       }
                       if (xlsx_requested(req)) {
                         XlsxSheet sheet("采购汇总（按商品）");
                         xlsx_preamble(sheet, "采购汇总（按商品）", scope);
                         sheet.add_header_row({"商品编码", "商品名称", "规格", "单位", "入库数量",
                                               "入库金额（元）", "退货额（元）", "净数量",
                                               "净金额（元）", "入库均价（元）"});
                         for (const auto& row : rows) {
                           sheet.add_row({XlsxCell::text(row.item_code),
                                          XlsxCell::text(row.item_name),
                                          XlsxCell::text(row.spec),
                                          XlsxCell::text(row.unit),
                                          XlsxCell::text(qty_text(row.in_qty)),
                                          XlsxCell::money(row.in_amount_cents),
                                          XlsxCell::money(row.return_amount_cents),
                                          XlsxCell::text(qty_text(row.qty)),
                                          XlsxCell::money(row.amount_cents),
                                          row.has_inbound ? XlsxCell::money(row.avg_price_cents)
                                                          : XlsxCell::text("—")});
                         }
                         sheet.add_row({XlsxCell::text("合计"),
                                        XlsxCell::text(""),
                                        XlsxCell::text(""),
                                        XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.in_qty)),
                                        XlsxCell::money(totals.in_amount_cents),
                                        XlsxCell::money(totals.return_amount_cents),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::money(totals.amount_cents),
                                        totals.has_inbound
                                            ? XlsxCell::money(totals.avg_price_cents)
                                            : XlsxCell::text("—")});
                         send_xlsx(res, "purchase-summary-item-" + date_stamp() + ".xlsx",
                                   sheet.build());
                         return;
                       }
                       nlohmann::json list = nlohmann::json::array();
                       for (const auto& row : rows) {
                         list.push_back(purchase_item_to_json(row));
                       }
                       json_ok(res, nlohmann::json{{"rows", list},
                                                   {"totals", purchase_totals_to_json(totals)}});
                       return;
                     }

                     const auto rows = services.reports.purchase_summary_by_supplier(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "采购汇总（按供应商）", scope);
                       csv_row(csv, {"供应商", "入库数量", "入库金额（元）", "退货额（元）",
                                     "净数量", "净金额（元）", "入库均价（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.supplier, qty_text(row.in_qty),
                                       yuan(row.in_amount_cents),
                                       yuan(row.return_amount_cents), qty_text(row.qty),
                                       yuan(row.amount_cents),
                                       row.has_inbound ? yuan(row.avg_price_cents) : "—"});
                       }
                       csv_row(csv, {"合计", qty_text(totals.in_qty),
                                     yuan(totals.in_amount_cents),
                                     yuan(totals.return_amount_cents), qty_text(totals.qty),
                                     yuan(totals.amount_cents), avg_text});
                       send_csv(res, "purchase-summary-supplier-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("采购汇总（按供应商）");
                       xlsx_preamble(sheet, "采购汇总（按供应商）", scope);
                       sheet.add_header_row({"供应商", "入库数量", "入库金额（元）", "退货额（元）",
                                             "净数量", "净金额（元）", "入库均价（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.supplier),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::money(row.return_amount_cents),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents),
                                        row.has_inbound ? XlsxCell::money(row.avg_price_cents)
                                                        : XlsxCell::text("—")});
                       }
                       sheet.add_row({XlsxCell::text("合计"),
                                      XlsxCell::text(qty_text(totals.in_qty)),
                                      XlsxCell::money(totals.in_amount_cents),
                                      XlsxCell::money(totals.return_amount_cents),
                                      XlsxCell::text(qty_text(totals.qty)),
                                      XlsxCell::money(totals.amount_cents),
                                      totals.has_inbound
                                          ? XlsxCell::money(totals.avg_price_cents)
                                          : XlsxCell::text("—")});
                       send_xlsx(res, "purchase-summary-supplier-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_supplier_to_json(row));
                     }
                     json_ok(res, nlohmann::json{{"rows", list},
                                                 {"totals", purchase_totals_to_json(totals)}});
                   }));

  // 其他出入库按业务类型汇总：入库与出库分列，固定顺序赠品/期初/领用/报损/其他。
  server.Get("/api/reports/other-flow-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     if (req.has_param("biz_category")) {
                       filter.biz_category = req.get_param_value("biz_category");
                     }
                     const auto rows = services.reports.other_flow_summary(filter);
                     const std::string scope = other_flow_scope(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "其他出入库汇总", scope);
                       csv_row(csv, {"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                     "出库金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {biz_category_label(row.category), qty_text(row.in_qty),
                                       yuan(row.in_amount_cents), qty_text(row.out_qty),
                                       yuan(row.out_amount_cents)});
                       }
                       send_csv(res, "other-flow-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("其他出入库汇总");
                       xlsx_preamble(sheet, "其他出入库汇总", scope);
                       sheet.add_header_row({"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                             "出库金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(biz_category_label(row.category)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents)});
                       }
                       send_xlsx(res, "other-flow-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(other_flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/statement",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     if (partner_key.empty()) {
                       throw AppError("INVALID_INPUT", "请指定往来单位");
                     }
                     // 方向：显式参数优先；未传时按伙伴角色（P:）或台账方向推断，C:/T: 应收、S: 应付。
                     int direction = 1;
                     const std::string direction_param =
                         req.has_param("direction") ? req.get_param_value("direction") : "";
                     if (!direction_param.empty()) {
                       direction = direction_param == "-1" || direction_param == "AP" ? -1 : 1;
                     } else {
                       services.db.read([&](sqlite3* conn) {
                         direction = partner_key_direction(conn, partner_key);
                       });
                     }
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to = req.has_param("to") ? req.get_param_value("to") : "";
                     const StatementResult result =
                         services.reports.partner_statement(partner_key, direction, from, to);
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       const std::string period = from.empty() && to.empty()
                                                      ? "全部期间"
                                                      : (from.empty() ? std::string("起始不限") : from) +
                                                            " 至 " +
                                                            (to.empty() ? std::string("截止不限") : to);
                       const std::string partner =
                           (result.partner_name.empty() ? std::string("未命名")
                                                        : result.partner_name) +
                           "（" + result.partner_key + "）";
                       std::string csv;
                       csv_preamble(csv, "对账单",
                                    std::string("往来单位 ") + partner + "；方向 " +
                                        (direction == -1 ? "应付" : "应收") + "；期间 " + period +
                                        "；仅统计 v7 迁移后审核的单据，已反审核单据不计入；"
                                        "撤销收付款按撤销当天入账，标注原单并单列净额");
                       csv_row(csv, {"日期", "类型", "单号", "摘要", "增加（元）", "减少（元）",
                                     "余额（元）"});
                       csv_row(csv, {"期初余额", "", "", "", "", "",
                                     yuan(result.opening_cents)});
                       for (const auto& line : result.lines) {
                         csv_row(csv, {line.date, statement_kind_label(result, line),
                                       line.doc_no, statement_summary(result, line),
                                       yuan(line.increase_cents), yuan(line.decrease_cents),
                                       yuan(line.balance_cents)});
                       }
                       csv_row(csv, {"本期合计", "", "", "", yuan(result.increase_cents),
                                     yuan(result.decrease_cents), yuan(result.closing_cents)});
                       csv_row(csv, {"其中：撤销收付款", "",
                                     std::to_string(result.reverse_count) + " 笔", "",
                                     yuan(result.reverse_cents), "", ""});
                       csv_row(csv, {"期间核销净额", "", "",
                                     (direction == -1 ? "付款" : "收款") + std::string(" − 撤销回冲"),
                                     "", yuan(result.settle_net_cents), ""});
                       send_csv(res,
                                std::string("statement-") + (direction == -1 ? "ap" : "ar") + "-" +
                                    date_stamp() + ".csv",
                                csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       const std::string period = from.empty() && to.empty()
                                                      ? "全部期间"
                                                      : (from.empty() ? std::string("起始不限") : from) +
                                                            " 至 " +
                                                            (to.empty() ? std::string("截止不限") : to);
                       const std::string partner =
                           (result.partner_name.empty() ? std::string("未命名")
                                                        : result.partner_name) +
                           "（" + result.partner_key + "）";
                       XlsxSheet sheet("对账单");
                       xlsx_preamble(sheet, "对账单",
                                     std::string("往来单位 ") + partner + "；方向 " +
                                         (direction == -1 ? "应付" : "应收") + "；期间 " + period +
                                         "；仅统计 v7 迁移后审核的单据，已反审核单据不计入；"
                                         "撤销收付款按撤销当天入账，标注原单并单列净额");
                       sheet.add_header_row({"日期", "类型", "单号", "摘要", "增加（元）",
                                             "减少（元）", "余额（元）"});
                       sheet.add_row({XlsxCell::text("期初余额"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(result.opening_cents)});
                       for (const auto& line : result.lines) {
                         sheet.add_row({XlsxCell::text(line.date),
                                        XlsxCell::text(statement_kind_label(result, line)),
                                        XlsxCell::text(line.doc_no),
                                        XlsxCell::text(statement_summary(result, line)),
                                        XlsxCell::money(line.increase_cents),
                                        XlsxCell::money(line.decrease_cents),
                                        XlsxCell::money(line.balance_cents)});
                       }
                       sheet.add_row({XlsxCell::text("本期合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(result.increase_cents),
                                      XlsxCell::money(result.decrease_cents),
                                      XlsxCell::money(result.closing_cents)});
                       sheet.add_row({XlsxCell::text("其中：撤销收付款"), XlsxCell::text(""),
                                      XlsxCell::text(std::to_string(result.reverse_count) + " 笔"),
                                      XlsxCell::text(""),
                                      XlsxCell::money(result.reverse_cents),
                                      XlsxCell::text(""), XlsxCell::text("")});
                       sheet.add_row({XlsxCell::text("期间核销净额"), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::text((direction == -1 ? "付款" : "收款") +
                                                     std::string(" − 撤销回冲")),
                                      XlsxCell::text(""),
                                      XlsxCell::money(result.settle_net_cents),
                                      XlsxCell::text("")});
                       send_xlsx(res,
                                 std::string("statement-") + (direction == -1 ? "ap" : "ar") + "-" +
                                     date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json body = statement_to_json(result);
                     body["data_scope"] = kArApScope;
                     json_ok(res, body);
                   }));

  server.Get("/api/reports/ar-ap-aging",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string as_of = req.has_param("as_of")
                                                   ? req.get_param_value("as_of")
                                                   : today_text();
                     const auto rows = services.reports.ar_ap_aging(direction, as_of);
                     // 合计行按当前截止日的全量分组求和。
                     std::int64_t aging_total = 0;
                     std::int64_t aging_not_due = 0;
                     std::int64_t aging_d1_30 = 0;
                     std::int64_t aging_d31_60 = 0;
                     std::int64_t aging_d61_90 = 0;
                     std::int64_t aging_d90_plus = 0;
                     for (const auto& row : rows) {
                       aging_total += row.total_cents;
                       aging_not_due += row.not_due_cents;
                       aging_d1_30 += row.d1_30_cents;
                       aging_d31_60 += row.d31_60_cents;
                       aging_d61_90 += row.d61_90_cents;
                       aging_d90_plus += row.d90_plus_cents;
                     }
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "应收应付余额与账龄",
                                    std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                        "；截止日 " + as_of +
                                        "；仅统计 v7 迁移后审核的单据"
                                        "；按截止日时点还原：业务日期晚于截止日不计入，"
                                        "核销按核销日期还原");
                       csv_row(csv, {"往来单位", "方向", "合计（元）", "未到期（元）", "1-30 天（元）",
                                     "31-60 天（元）", "61-90 天（元）", "90 天以上（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.partner_name.empty() ? row.partner_key
                                                               : row.partner_name,
                                       row.direction == -1 ? "应付" : "应收",
                                       yuan(row.total_cents), yuan(row.not_due_cents),
                                       yuan(row.d1_30_cents), yuan(row.d31_60_cents),
                                       yuan(row.d61_90_cents), yuan(row.d90_plus_cents)});
                       }
                       csv_row(csv, {"合计", "", yuan(aging_total), yuan(aging_not_due),
                                     yuan(aging_d1_30), yuan(aging_d31_60),
                                     yuan(aging_d61_90), yuan(aging_d90_plus)});
                       send_csv(res, "ar-ap-aging-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("应收应付余额与账龄");
                       xlsx_preamble(sheet, "应收应付余额与账龄",
                                     std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                         "；截止日 " + as_of +
                                         "；仅统计 v7 迁移后审核的单据"
                                         "；按截止日时点还原：业务日期晚于截止日不计入，"
                                         "核销按核销日期还原");
                       sheet.add_header_row({"往来单位", "方向", "合计（元）", "未到期（元）",
                                             "1-30 天（元）", "31-60 天（元）", "61-90 天（元）",
                                             "90 天以上（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.partner_name.empty() ? row.partner_key
                                                                      : row.partner_name),
                              XlsxCell::text(row.direction == -1 ? "应付" : "应收"),
                              XlsxCell::money(row.total_cents),
                              XlsxCell::money(row.not_due_cents),
                              XlsxCell::money(row.d1_30_cents),
                              XlsxCell::money(row.d31_60_cents),
                              XlsxCell::money(row.d61_90_cents),
                              XlsxCell::money(row.d90_plus_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::money(aging_total),
                                      XlsxCell::money(aging_not_due),
                                      XlsxCell::money(aging_d1_30),
                                      XlsxCell::money(aging_d31_60),
                                      XlsxCell::money(aging_d61_90),
                                      XlsxCell::money(aging_d90_plus)});
                       send_xlsx(res, "ar-ap-aging-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(aging_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  // 资金流水（现金日记账）：包含期初行与逐行余额，按账户 + 日期筛选。
  server.Get("/api/reports/fund-ledger",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     // account_id：数字为指定账户，0 为历史「未指定账户」，all 为全部账户。
                     std::int64_t account_id = 0;
                     if (req.has_param("account_id")) {
                       const std::string value = req.get_param_value("account_id");
                       account_id = value == "all" ? -1 : std::stoll(value);
                     }
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to =
                         req.has_param("to") ? req.get_param_value("to") : "";
                     const std::string format =
                         req.has_param("format") ? req.get_param_value("format") : "json";
                     const auto rows = services.settlements.ledger(account_id, from, to);
                     const auto direction_text = [](const FundLedgerRow& row) -> std::string {
                       if (row.opening) return "期初";
                       // 撤销行金额为负，列内只显示绝对值，靠类型区分「撤销收款/付款」。
                       const bool reversed = row.note.rfind("撤销", 0) == 0;
                       if (row.direction == 1) return reversed ? "撤销收款" : "收款";
                       return reversed ? "撤销付款" : "付款";
                     };

                     if (format == "csv") {
                       std::string csv = "\xEF\xBB\xBF";
                       csv_row(csv, {"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                     "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.settled_on, row.settle_no, direction_text(row),
                                       row.partner_name, method_label(row.method),
                                       row.operator_name, row.note,
                                       row.opening ? std::string{}
                                                   : yuan(row.income_cents),
                                       row.opening ? std::string{}
                                                   : yuan(row.expense_cents),
                                       yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-ledger-" + date_stamp() + ".csv", csv);
                       return;
                     }

                     if (format == "xlsx") {
                       XlsxSheet sheet("资金流水");
                       sheet.add_header_row({"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                             "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.settled_on),
                              XlsxCell::text(row.settle_no),
                              XlsxCell::text(direction_text(row)),
                              XlsxCell::text(row.partner_name),
                              XlsxCell::text(method_label(row.method)),
                              XlsxCell::text(row.operator_name),
                              XlsxCell::text(row.note),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.income_cents),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.expense_cents),
                              XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-ledger-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }

                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back({
                           {"opening", row.opening},
                           {"settled_on", row.settled_on},
                           {"settle_no", row.settle_no},
                           {"direction", row.opening ? 0 : row.direction},
                           {"direction_text", direction_text(row)},
                           {"partner_name", row.partner_name},
                           {"method", row.method},
                           {"method_text", method_label(row.method)},
                           {"operator", row.operator_name},
                           {"note", row.note},
                           {"income_cents", row.income_cents},
                           {"expense_cents", row.expense_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

  // 账户余额一览：期初 + 累计收入 − 累计支出。
  server.Get("/api/reports/fund-balances",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string as_of =
                         req.has_param("as_of") ? req.get_param_value("as_of") : "";
                     const auto rows = services.settlements.account_balances(as_of);
                     const std::string scope =
                         "截至 " + (as_of.empty() ? std::string("今天") : as_of) +
                         "；期初 + 累计收入 − 累计支出";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "账户余额一览", scope);
                       csv_row(csv, {"账户编码", "账户名称", "类型", "期初（元）",
                                     "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.code, row.name, method_label(row.kind),
                                       yuan(row.opening_cents), yuan(row.inflow_cents),
                                       yuan(row.outflow_cents), yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-balances-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("账户余额一览");
                       xlsx_preamble(sheet, "账户余额一览", scope);
                       sheet.add_header_row({"账户编码", "账户名称", "类型", "期初（元）",
                                             "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.code), XlsxCell::text(row.name),
                                        XlsxCell::text(method_label(row.kind)),
                                        XlsxCell::money(row.opening_cents),
                                        XlsxCell::money(row.inflow_cents),
                                        XlsxCell::money(row.outflow_cents),
                                        XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-balances-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back({
                           {"id", row.id},
                           {"code", row.code},
                           {"name", row.name},
                           {"kind", row.kind},
                           {"kind_text", method_label(row.kind)},
                           {"opening_cents", row.opening_cents},
                           {"inflow_cents", row.inflow_cents},
                           {"outflow_cents", row.outflow_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

  // 成本重算只读核对：按流水重放移动加权平均，与账面结存比对，不写回。
  server.Get("/api/stock/cost-recalc",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t item_id =
                         req.has_param("item_id") ? std::stoll(req.get_param_value("item_id")) : 0;
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? std::stoll(req.get_param_value("warehouse_id"))
                             : 0;
                     const CostRecalcResult result =
                         services.cost_recalc.recalculate(item_id, warehouse_id);
                     nlohmann::json differences = nlohmann::json::array();
                     for (const auto& row : result.differences) {
                       differences.push_back(
                           {{"item_id", row.item_id},
                            {"item_code", row.item_code},
                            {"item_name", row.item_name},
                            {"warehouse_id", row.warehouse_id},
                            {"warehouse_name", row.warehouse_name},
                            {"book_qty", qty_text(row.book_qty)},
                            {"book_amount_cents", row.book_amount_cents},
                            {"replay_qty", qty_text(row.replay_qty)},
                            {"replay_amount_cents", row.replay_amount_cents},
                            {"qty_diff", qty_text(row.qty_diff)},
                            {"amount_diff_cents", row.amount_diff_cents}});
                     }
                     json_ok(res, nlohmann::json{
                                      {"ledger_rows", result.ledger_rows},
                                      {"item_count", result.item_count},
                                      {"difference_count", result.difference_count},
                                      {"qty_difference_count", result.qty_difference_count},
                                      {"differences", differences},
                                  });
                   }));
}

}  // namespace xiaotu
