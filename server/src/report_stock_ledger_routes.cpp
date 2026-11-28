#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

namespace xiaotu {
namespace {

using namespace report_common;

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

std::string qty_direction_label(int direction) {
  if (direction > 0) return "入库";
  if (direction < 0) return "出库";
  return "全部";
}

}  // namespace

void register_report_stock_ledger_routes(httplib::Server& server, Services& services) {
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

  // 成本重算只读核对：按流水重放移动加权平均，与账面结存比对，不写回。
  server.Get("/api/stock/cost-recalc",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t item_id =
                         req.has_param("item_id") ? parse_id(req.get_param_value("item_id")) : 0;
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? parse_id(req.get_param_value("warehouse_id"))
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
