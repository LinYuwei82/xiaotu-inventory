#include "print_render.h"

#include <cstdint>
#include <map>
#include <string>

#include "xiaotu/document.h"
#include "xiaotu/error.h"
#include "xiaotu/qty.h"
#include "xiaotu/services.h"

namespace xiaotu {

void render_document(Services& services, const PrintTargetDef& target, const httplib::Request& req,
                     PrintRender& render) {
  const std::int64_t doc_id = param_i64(req, "doc_id", 0);
  if (doc_id <= 0) {
    throw AppError("INVALID_INPUT", "请指定单据");
  }
  const DocDetail detail = services.docs.detail(doc_id);
  if (detail.doc_type != target.key) {
    throw AppError("INVALID_INPUT",
                   "单据类型与打印目标不一致：" + detail.doc_type + " / " + target.key);
  }

  std::map<std::int64_t, gen::ItemRow> items;
  for (const auto& row : services.item.list()) {
    items[row.id] = row;
  }
  std::string warehouse_name;
  std::map<std::int64_t, std::string> warehouse_names;
  for (const auto& row : services.warehouse.list()) {
    warehouse_names[row.id] = row.name;
    if (row.id == detail.warehouse_id) warehouse_name = row.name;
  }

  int line_no = 0;
  for (const auto& line : detail.lines) {
    const auto found = items.find(line.item_id);
    const std::string code = found == items.end() ? std::string{} : found->second.code;
    const std::string name = found == items.end() ? std::string{} : found->second.name;
    const std::string spec = found == items.end() ? std::string{} : found->second.spec;
    const std::string unit = found == items.end() ? std::string{} : found->second.unit;
    if (detail.doc_type == "CST_ADJ") {
      const auto wh = warehouse_names.find(line.warehouse_id);
      render.rows.push_back({{"line_no", ++line_no},
                             {"item_code", code},
                             {"item_name", name},
                             {"spec", spec},
                             {"unit", unit},
                             {"warehouse", wh == warehouse_names.end() ? std::string{}
                                                                       : wh->second},
                             {"adjust_cents", line.adjust_cents},
                             {"adjust_reason", line.adjust_reason}});
      continue;
    }
    render.rows.push_back({{"line_no", ++line_no},
                           {"item_code", code},
                           {"item_name", name},
                           {"spec", spec},
                           {"unit", unit},
                           {"qty", qty_text(line.qty)},
                           {"price_cents", line.price_cents},
                           {"amount_cents", mul_qty_price(line.qty, line.price_cents)},
                           {"tax_rate_text", rate_text(line.tax_rate_bp)},
                           {"price_includes_tax", line.price_includes_tax ? "是" : "否"},
                           {"net_cents", line.net_cents},
                           {"tax_cents", line.tax_cents},
                           {"with_tax_cents", line.net_cents + line.tax_cents}});
  }

  const std::string header_warehouse =
      detail.doc_type == "CST_ADJ" ? std::string("按明细行") : warehouse_name;
  render.subtitle = "单号 " + detail.doc_no + "；日期 " + detail.doc_date + "；仓库 " +
                    (header_warehouse.empty() ? "—" : header_warehouse) + "；状态 " +
                    doc_status_label(detail.status);
  render.header_fields = nlohmann::json::array({
      {{"label", "单号"}, {"value", detail.doc_no}},
      {{"label", "日期"}, {"value", detail.doc_date}},
      {{"label", "仓库"}, {"value", header_warehouse}},
      {{"label", "往来单位"}, {"value", detail.partner}},
  });
  if (!detail.biz_category.empty()) {
    render.header_fields.push_back(
        {{"label", "业务类型"}, {"value", stock_biz_category_label(detail.biz_category)}});
  }
  if (!detail.salesperson_name.empty()) {
    render.header_fields.push_back(
        {{"label", "业务员"}, {"value", detail.salesperson_name}});
  }
  render.header_fields.push_back({{"label", "状态"}, {"value", doc_status_label(detail.status)}});
  render.totals = {{"total_qty", qty_text(detail.total_qty)},
                   {"total_amount_cents", detail.total_amount_cents},
                   {"total_amount_text", print_money_text(detail.total_amount_cents)},
                   {"total_net_cents", detail.total_net_cents},
                   {"total_tax_cents", detail.total_tax_cents},
                   {"total_with_tax_cents", detail.total_with_tax_cents},
                   {"total_net_text", print_money_text(detail.total_net_cents)},
                   {"total_tax_text", print_money_text(detail.total_tax_cents)},
                   {"total_with_tax_text", print_money_text(detail.total_with_tax_cents)}};
  render.placeholders["doc_no"] = detail.doc_no;
  render.placeholders["doc_date"] = detail.doc_date;
  render.placeholders["partner"] = detail.partner;
  render.placeholders["warehouse"] = warehouse_name;
}

}  // namespace xiaotu
