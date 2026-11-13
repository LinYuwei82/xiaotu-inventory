#include "xiaotu/print_template.h"

#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/crud.h"
#include "xiaotu/error.h"

namespace xiaotu {
namespace {

constexpr const char* kTemplateSelect =
    "SELECT id, code, name, target_type, target_key, page_size, orientation,"
    " paper_width_mm, paper_height_mm, margin_mm, font_size_pt, header_text, footer_text,"
    " show_page_no, is_default, status, version, is_deleted,"
    " created_at, created_by, updated_at, updated_by FROM print_template";

PrintColumnDef column_def(std::string key, std::string label, std::string type,
                          std::string align, int width_mm) {
  return PrintColumnDef{.key = std::move(key),
                        .label = std::move(label),
                        .type = std::move(type),
                        .align = std::move(align),
                        .width_mm = width_mm};
}

std::vector<PrintTargetDef> make_targets() {
  // 九种单据的明细列一致，模板只决定显示哪些、顺序与宽度。
  const std::vector<PrintColumnDef> doc_columns = {
      column_def("line_no", "序号", "INTEGER", "LEFT", 10),
      column_def("item_code", "商品编码", "TEXT", "LEFT", 26),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 44),
      column_def("spec", "规格", "TEXT", "LEFT", 26),
      column_def("unit", "单位", "TEXT", "CENTER", 12),
      column_def("qty", "数量", "INTEGER", "RIGHT", 14),
      column_def("price_cents", "单价（元）", "MONEY", "RIGHT", 18),
      column_def("amount_cents", "金额（元）", "MONEY", "RIGHT", 22),
  };
  const std::vector<PrintColumnDef> balance_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 26),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 44),
      column_def("unit", "单位", "TEXT", "CENTER", 12),
      column_def("warehouse_code", "仓库编码", "TEXT", "LEFT", 22),
      column_def("warehouse_name", "仓库名称", "TEXT", "LEFT", 28),
      column_def("qty", "数量", "INTEGER", "RIGHT", 14),
      column_def("amount_cents", "金额（元）", "MONEY", "RIGHT", 22),
  };
  const std::vector<PrintColumnDef> ledger_columns = {
      column_def("biz_time", "时间", "TEXT", "LEFT", 22),
      column_def("biz_type", "类型", "TEXT", "CENTER", 22),
      column_def("biz_category", "业务类型", "TEXT", "CENTER", 12),
      column_def("doc_no", "单据号", "TEXT", "LEFT", 26),
      column_def("item_code", "商品编码", "TEXT", "LEFT", 22),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 30),
      column_def("warehouse_name", "仓库", "TEXT", "LEFT", 20),
      column_def("qty", "数量", "INTEGER", "RIGHT", 12),
      column_def("amount_cents", "金额（元）", "MONEY", "RIGHT", 18),
  };
  const std::vector<PrintColumnDef> flow_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 20),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 30),
      column_def("unit", "单位", "TEXT", "CENTER", 10),
      column_def("warehouse_code", "仓库编码", "TEXT", "LEFT", 20),
      column_def("warehouse_name", "仓库名称", "TEXT", "LEFT", 20),
      column_def("opening_qty", "期初数量", "INTEGER", "RIGHT", 12),
      column_def("in_qty", "收入数量", "INTEGER", "RIGHT", 12),
      column_def("out_qty", "发出数量", "INTEGER", "RIGHT", 12),
      column_def("closing_qty", "期末数量", "INTEGER", "RIGHT", 12),
      column_def("in_amount_cents", "收入金额（元）", "MONEY", "RIGHT", 18),
      column_def("out_amount_cents", "发出金额（元）", "MONEY", "RIGHT", 18),
      column_def("adjust_amount_cents", "其他调整（元）", "MONEY", "RIGHT", 16),
  };
  const std::vector<PrintColumnDef> summary_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 24),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 28),
      column_def("unit", "单位", "TEXT", "CENTER", 10),
      column_def("opening_qty", "期初数量", "INTEGER", "RIGHT", 12),
      column_def("opening_amount_cents", "期初金额（元）", "MONEY", "RIGHT", 15),
      column_def("in_qty", "入库数量", "INTEGER", "RIGHT", 12),
      column_def("in_amount_cents", "入库金额（元）", "MONEY", "RIGHT", 15),
      column_def("out_qty", "出库数量", "INTEGER", "RIGHT", 12),
      column_def("out_amount_cents", "出库金额（元）", "MONEY", "RIGHT", 15),
      column_def("adjust_amount_cents", "其他调整（元）", "MONEY", "RIGHT", 14),
      column_def("closing_qty", "期末数量", "INTEGER", "RIGHT", 12),
      column_def("closing_amount_cents", "期末金额（元）", "MONEY", "RIGHT", 15),
  };
  const std::vector<PrintColumnDef> profit_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 24),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 38),
      column_def("unit", "单位", "TEXT", "CENTER", 10),
      column_def("qty", "销售数量", "INTEGER", "RIGHT", 12),
      column_def("revenue_cents", "销售收入（元）", "MONEY", "RIGHT", 18),
      column_def("cost_cents", "销售成本（元）", "MONEY", "RIGHT", 18),
      column_def("profit_cents", "毛利（元）", "MONEY", "RIGHT", 18),
      column_def("margin", "毛利率", "PERCENT", "RIGHT", 12),
  };
  const std::vector<PrintColumnDef> aging_columns = {
      column_def("partner_name", "往来单位", "TEXT", "LEFT", 36),
      column_def("direction", "方向", "TEXT", "CENTER", 12),
      column_def("total_cents", "合计（元）", "MONEY", "RIGHT", 18),
      column_def("not_due_cents", "未到期（元）", "MONEY", "RIGHT", 18),
      column_def("d1_30_cents", "1-30 天（元）", "MONEY", "RIGHT", 18),
      column_def("d31_60_cents", "31-60 天（元）", "MONEY", "RIGHT", 18),
      column_def("d61_90_cents", "61-90 天（元）", "MONEY", "RIGHT", 18),
      column_def("d90_plus_cents", "90 天以上（元）", "MONEY", "RIGHT", 18),
  };
  const std::vector<PrintColumnDef> statement_columns = {
      column_def("date", "日期", "TEXT", "LEFT", 22),
      column_def("kind", "类型", "TEXT", "CENTER", 14),
      column_def("doc_no", "单号", "TEXT", "LEFT", 30),
      column_def("summary", "摘要", "TEXT", "LEFT", 42),
      column_def("increase_cents", "增加（元）", "MONEY", "RIGHT", 18),
      column_def("decrease_cents", "减少（元）", "MONEY", "RIGHT", 18),
      column_def("balance_cents", "余额（元）", "MONEY", "RIGHT", 18),
  };
  const std::vector<PrintColumnDef> fund_ledger_columns = {
      column_def("date", "日期", "TEXT", "LEFT", 22),
      column_def("kind", "类型", "TEXT", "CENTER", 12),
      column_def("doc_no", "单号", "TEXT", "LEFT", 30),
      column_def("partner_name", "往来单位", "TEXT", "LEFT", 30),
      column_def("method", "结算方式", "TEXT", "CENTER", 14),
      column_def("operator", "经办人", "TEXT", "LEFT", 16),
      column_def("summary", "摘要", "TEXT", "LEFT", 36),
      column_def("income_cents", "收入（元）", "MONEY", "RIGHT", 18),
      column_def("expense_cents", "支出（元）", "MONEY", "RIGHT", 18),
      column_def("balance_cents", "余额（元）", "MONEY", "RIGHT", 18),
  };

  const std::vector<PrintColumnDef> purchase_detail_columns = {
      column_def("date", "日期", "TEXT", "LEFT", 22),
      column_def("doc_no", "单号", "TEXT", "LEFT", 28),
      column_def("kind", "类型", "TEXT", "CENTER", 14),
      column_def("supplier", "供应商", "TEXT", "LEFT", 30),
      column_def("item_code", "商品编码", "TEXT", "LEFT", 24),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 34),
      column_def("spec", "规格", "TEXT", "LEFT", 20),
      column_def("unit", "单位", "TEXT", "CENTER", 10),
      column_def("warehouse_name", "仓库", "TEXT", "LEFT", 22),
      column_def("qty", "数量", "INTEGER", "RIGHT", 14),
      column_def("price_cents", "单价（元）", "MONEY", "RIGHT", 16),
      column_def("amount_cents", "金额（元）", "MONEY", "RIGHT", 18),
  };
  // 采购汇总：入库与退货分列，均价只取入库行（无入库时打印「—」）。
  const std::vector<PrintColumnDef> purchase_supplier_columns = {
      column_def("supplier", "供应商", "TEXT", "LEFT", 40),
      column_def("in_qty", "入库数量", "INTEGER", "RIGHT", 16),
      column_def("in_amount_cents", "入库金额（元）", "MONEY", "RIGHT", 20),
      column_def("return_amount_cents", "退货额（元）", "MONEY", "RIGHT", 20),
      column_def("qty", "净数量", "INTEGER", "RIGHT", 16),
      column_def("amount_cents", "净金额（元）", "MONEY", "RIGHT", 20),
      column_def("avg_price_cents", "入库均价（元）", "MONEY", "RIGHT", 20),
  };
  const std::vector<PrintColumnDef> purchase_item_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 20),
      column_def("item_name", "商品名称", "TEXT", "LEFT", 34),
      column_def("spec", "规格", "TEXT", "LEFT", 18),
      column_def("unit", "单位", "TEXT", "CENTER", 10),
      column_def("in_qty", "入库数量", "INTEGER", "RIGHT", 14),
      column_def("in_amount_cents", "入库金额（元）", "MONEY", "RIGHT", 18),
      column_def("return_amount_cents", "退货额（元）", "MONEY", "RIGHT", 18),
      column_def("qty", "净数量", "INTEGER", "RIGHT", 14),
      column_def("amount_cents", "净金额（元）", "MONEY", "RIGHT", 18),
      column_def("avg_price_cents", "入库均价（元）", "MONEY", "RIGHT", 18),
  };

  const std::vector<PrintColumnDef> other_flow_columns = {
      column_def("category", "业务类型", "TEXT", "LEFT", 36),
      column_def("in_qty", "入库数量", "INTEGER", "RIGHT", 34),
      column_def("in_amount_cents", "入库金额（元）", "MONEY", "RIGHT", 36),
      column_def("out_qty", "出库数量", "INTEGER", "RIGHT", 34),
      column_def("out_amount_cents", "出库金额（元）", "MONEY", "RIGHT", 36),
  };

  const std::vector<PrintColumnDef> cost_adjust_columns = {
      column_def("item_code", "商品编码", "TEXT", "LEFT", 24),
      column_def("item_name", "商品", "TEXT", "LEFT", 36),
      column_def("spec", "规格", "TEXT", "LEFT", 22),
      column_def("unit", "单位", "TEXT", "CENTER", 12),
      column_def("warehouse", "仓库", "TEXT", "LEFT", 22),
      column_def("adjust_cents", "调整金额（元）", "MONEY", "RIGHT", 20),
      column_def("adjust_reason", "调整原因", "TEXT", "LEFT", 44),
  };

  const std::vector<PrintColumnDef> sales_customer_columns = {
      column_def("name", "客户", "TEXT", "LEFT", 40),
      column_def("orders", "单数", "INTEGER", "RIGHT", 12),
      column_def("qty", "销售数量", "INTEGER", "RIGHT", 16),
      column_def("sales_cents", "销售额（元）", "MONEY", "RIGHT", 20),
      column_def("return_cents", "退货额（元）", "MONEY", "RIGHT", 20),
      column_def("net_cents", "净销售额（元）", "MONEY", "RIGHT", 20),
      column_def("cost_cents", "成本（元）", "MONEY", "RIGHT", 18),
      column_def("profit_cents", "毛利（元）", "MONEY", "RIGHT", 18),
      column_def("margin", "毛利率", "TEXT", "RIGHT", 14),
      column_def("ratio", "销售额占比", "TEXT", "RIGHT", 16),
  };
  const std::vector<PrintColumnDef> sales_salesperson_columns = {
      column_def("name", "业务员", "TEXT", "LEFT", 32),
      column_def("orders", "单数", "INTEGER", "RIGHT", 12),
      column_def("customers", "客户数", "INTEGER", "RIGHT", 14),
      column_def("qty", "销售数量", "INTEGER", "RIGHT", 16),
      column_def("sales_cents", "销售额（元）", "MONEY", "RIGHT", 20),
      column_def("return_cents", "退货额（元）", "MONEY", "RIGHT", 20),
      column_def("net_cents", "净销售额（元）", "MONEY", "RIGHT", 20),
      column_def("cost_cents", "成本（元）", "MONEY", "RIGHT", 18),
      column_def("profit_cents", "毛利（元）", "MONEY", "RIGHT", 18),
      column_def("margin", "毛利率", "TEXT", "RIGHT", 14),
      column_def("ratio", "销售额占比", "TEXT", "RIGHT", 16),
  };

  return {
      {"DOC", "SAL_OUT", "销售出库单", doc_columns},
      {"DOC", "PUR_IN", "采购入库单", doc_columns},
      {"DOC", "PUR_RET", "采购退货单", doc_columns},
      {"DOC", "SAL_RET", "销售退货单", doc_columns},
      {"DOC", "STK_TRF", "库存调拨单", doc_columns},
      {"DOC", "STK_CNT", "库存盘点单", doc_columns},
      {"DOC", "OTH_IN", "其他入库单", doc_columns},
      {"DOC", "OTH_OUT", "其他出库单", doc_columns},
      {"DOC", "SAL_ORDER", "销售订单", doc_columns},
      {"DOC", "PUR_ORDER", "采购订单", doc_columns},
      {"DOC", "CST_ADJ", "成本调整单", cost_adjust_columns},
      {"REPORT", "stock-balance", "库存余额表", balance_columns},
      {"REPORT", "stock-ledger", "库存流水明细", ledger_columns},
      {"REPORT", "goods-flow", "收发明细", flow_columns},
      {"REPORT", "inventory-summary", "进销存汇总", summary_columns},
      {"REPORT", "sales-profit", "销售毛利", profit_columns},
      {"REPORT", "sales-customer", "销售报表（按客户）", sales_customer_columns},
      {"REPORT", "sales-salesperson", "销售报表（按业务员）", sales_salesperson_columns},
      {"REPORT", "purchase-detail", "采购明细", purchase_detail_columns},
      {"REPORT", "purchase-summary-supplier", "采购汇总（按供应商）", purchase_supplier_columns},
      {"REPORT", "purchase-summary-item", "采购汇总（按商品）", purchase_item_columns},
      {"REPORT", "other-flow-summary", "其他出入库汇总", other_flow_columns},
      {"REPORT", "ar-ap-aging", "应收应付余额与账龄", aging_columns},
      {"REPORT", "statement", "对账单", statement_columns},
      {"REPORT", "fund-ledger", "资金流水", fund_ledger_columns},
  };
}

int codepoint_count(std::string_view text) {
  int count = 0;
  for (const unsigned char ch : text) {
    if ((ch & 0xC0) != 0x80) ++count;
  }
  return count;
}

PrintTemplate template_from_row(const Stmt& stmt) {
  PrintTemplate row;
  row.id = stmt.i64(0);
  row.code = stmt.text(1);
  row.name = stmt.text(2);
  row.target_type = stmt.text(3);
  row.target_key = stmt.text(4);
  row.page_size = stmt.text(5);
  row.orientation = stmt.text(6);
  row.paper_width_mm = static_cast<int>(stmt.i64(7));
  row.paper_height_mm = static_cast<int>(stmt.i64(8));
  row.margin_mm = static_cast<int>(stmt.i64(9));
  row.font_size_pt = static_cast<int>(stmt.i64(10));
  row.header_text = stmt.text(11);
  row.footer_text = stmt.text(12);
  row.show_page_no = stmt.i64(13) != 0;
  row.is_default = stmt.i64(14) != 0;
  row.active = stmt.i64(15) != 0;
  row.version = stmt.i64(16);
  row.created_at = stmt.text(18);
  row.created_by = stmt.text(19);
  row.updated_at = stmt.text(20);
  row.updated_by = stmt.text(21);
  return row;
}

void load_columns(sqlite3* conn, PrintTemplate& tmpl) {
  Stmt stmt(conn,
            "SELECT column_key, position, visible, width_mm, align, font_size_pt, bold"
            " FROM print_template_column WHERE template_id = ? ORDER BY position;");
  stmt.bind(1, tmpl.id);
  while (stmt.step()) {
    PrintTemplateColumn column;
    column.column_key = stmt.text(0);
    column.position = static_cast<int>(stmt.i64(1));
    column.visible = stmt.i64(2) != 0;
    column.width_mm = static_cast<int>(stmt.i64(3));
    column.align = stmt.text(4);
    column.font_size_pt = static_cast<int>(stmt.i64(5));
    column.bold = stmt.i64(6) != 0;
    tmpl.columns.push_back(std::move(column));
  }
}

// 校验目标与列白名单，并把列位置重排为数组顺序。返回目标定义。
const PrintTargetDef& validate_template(PrintTemplate& tmpl) {
  const PrintTargetDef* target = find_print_target(tmpl.target_type, tmpl.target_key);
  if (target == nullptr) {
    throw AppError("INVALID_INPUT", "未知的打印目标：" + tmpl.target_type + "/" + tmpl.target_key);
  }
  if (tmpl.code.empty()) {
    throw AppError("INVALID_INPUT", "模板编码不能为空");
  }
  if (tmpl.name.empty()) {
    throw AppError("INVALID_INPUT", "模板名称不能为空");
  }
  if (tmpl.page_size != "A4" && tmpl.page_size != "A5" && tmpl.page_size != "CUSTOM") {
    throw AppError("INVALID_INPUT", "纸张只能是 A4、A5 或自定义");
  }
  if (tmpl.orientation != "PORTRAIT" && tmpl.orientation != "LANDSCAPE") {
    throw AppError("INVALID_INPUT", "纸张方向只能是纵向或横向");
  }
  if (tmpl.page_size == "CUSTOM" && (tmpl.paper_width_mm <= 0 || tmpl.paper_height_mm <= 0)) {
    throw AppError("INVALID_INPUT", "自定义纸张必须填写宽高");
  }
  if (tmpl.paper_width_mm < 0 || tmpl.paper_width_mm > 1000 || tmpl.paper_height_mm < 0 ||
      tmpl.paper_height_mm > 1000) {
    throw AppError("INVALID_INPUT", "纸张宽高需在 0-1000mm 之间");
  }
  if (tmpl.margin_mm < 0 || tmpl.margin_mm > 50) {
    throw AppError("INVALID_INPUT", "页边距需在 0-50mm 之间");
  }
  if (tmpl.font_size_pt < 6 || tmpl.font_size_pt > 24) {
    throw AppError("INVALID_INPUT", "字号需在 6-24pt 之间");
  }
  if (codepoint_count(tmpl.header_text) > 500 || codepoint_count(tmpl.footer_text) > 500) {
    throw AppError("INVALID_INPUT", "页眉页脚不能超过 500 个字符");
  }
  if (tmpl.columns.empty()) {
    throw AppError("INVALID_INPUT", "模板至少需要配置一列");
  }

  std::set<std::string> seen;
  for (std::size_t i = 0; i < tmpl.columns.size(); ++i) {
    PrintTemplateColumn& column = tmpl.columns[i];
    const PrintColumnDef* def = find_print_column(*target, column.column_key);
    if (def == nullptr) {
      throw AppError("INVALID_INPUT", "打印目标不包含列：" + column.column_key);
    }
    if (!seen.insert(column.column_key).second) {
      throw AppError("INVALID_INPUT", "列重复：" + column.column_key);
    }
    if (column.width_mm < 0 || column.width_mm > 500) {
      throw AppError("INVALID_INPUT", "列宽需在 0-500mm 之间");
    }
    if (column.align.empty()) {
      column.align = def->align;
    }
    if (column.align != "LEFT" && column.align != "CENTER" && column.align != "RIGHT") {
      throw AppError("INVALID_INPUT", "列对齐只能是左对齐、居中或右对齐");
    }
    if (column.font_size_pt != 0 && (column.font_size_pt < 6 || column.font_size_pt > 24)) {
      throw AppError("INVALID_INPUT", "列字号需为 0 或 6-24pt");
    }
    column.position = static_cast<int>(i) + 1;
  }
  return *target;
}

// 编码唯一性按全表判断：软删除的记录仍占用唯一索引，提前给出可读错误。
void ensure_code_free(sqlite3* conn, const std::string& code, std::int64_t exclude_id) {
  Stmt stmt(conn, "SELECT is_deleted FROM print_template WHERE code = ? AND id <> ? LIMIT 1;");
  stmt.bind(1, code).bind(2, exclude_id);
  if (stmt.step()) {
    const bool deleted = stmt.i64(0) != 0;
    throw AppError("DUPLICATE_CODE",
                   deleted ? "编码已被已删除的模板占用：" + code : "模板编码已存在：" + code);
  }
}

void clear_default(sqlite3* conn, const std::string& target_type, const std::string& target_key,
                   std::int64_t exclude_id) {
  Stmt stmt(conn,
            "UPDATE print_template SET is_default = 0"
            " WHERE target_type = ? AND target_key = ? AND is_default = 1 AND id <> ?;");
  stmt.bind(1, target_type).bind(2, target_key).bind(3, exclude_id);
  stmt.run();
}

void insert_columns(sqlite3* conn, std::int64_t template_id,
                    const std::vector<PrintTemplateColumn>& columns) {
  for (const PrintTemplateColumn& column : columns) {
    Stmt stmt(conn,
              "INSERT INTO print_template_column"
              " (template_id, column_key, position, visible, width_mm, align, font_size_pt, bold)"
              " VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
    stmt.bind(1, template_id)
        .bind(2, column.column_key)
        .bind(3, column.position)
        .bind(4, static_cast<std::int64_t>(column.visible ? 1 : 0))
        .bind(5, column.width_mm)
        .bind(6, column.align)
        .bind(7, column.font_size_pt)
        .bind(8, static_cast<std::int64_t>(column.bold ? 1 : 0))
        .run();
  }
}

}  // namespace

const std::vector<PrintTargetDef>& print_targets() {
  static const std::vector<PrintTargetDef> targets = make_targets();
  return targets;
}

const PrintTargetDef* find_print_target(std::string_view target_type,
                                        std::string_view target_key) {
  for (const PrintTargetDef& target : print_targets()) {
    if (target.type == target_type && target.key == target_key) {
      return &target;
    }
  }
  return nullptr;
}

const PrintColumnDef* find_print_column(const PrintTargetDef& target,
                                        std::string_view column_key) {
  for (const PrintColumnDef& column : target.columns) {
    if (column.key == column_key) {
      return &column;
    }
  }
  return nullptr;
}

PrintTemplate builtin_print_template(std::string_view target_type, std::string_view target_key) {
  PrintTemplate tmpl;
  tmpl.code = "builtin:" + std::string(target_type) + "/" + std::string(target_key);
  tmpl.target_type = std::string(target_type);
  tmpl.target_key = std::string(target_key);
  tmpl.version = 0;
  const PrintTargetDef* target = find_print_target(target_type, target_key);
  if (target == nullptr) {
    tmpl.name = "通用表格";
    return tmpl;
  }
  tmpl.name = target->name + "（内置默认）";
  int position = 0;
  for (const PrintColumnDef& def : target->columns) {
    PrintTemplateColumn column;
    column.column_key = def.key;
    column.position = ++position;
    column.visible = true;
    column.width_mm = def.width_mm;
    column.align = def.align;
    tmpl.columns.push_back(std::move(column));
  }
  return tmpl;
}

std::vector<PrintRenderColumn> resolved_print_columns(const PrintTemplate& tmpl) {
  std::vector<PrintRenderColumn> result;
  const PrintTargetDef* target = find_print_target(tmpl.target_type, tmpl.target_key);
  if (target == nullptr) {
    return result;
  }
  std::vector<PrintTemplateColumn> columns = tmpl.columns;
  std::sort(columns.begin(), columns.end(),
            [](const PrintTemplateColumn& left, const PrintTemplateColumn& right) {
              return left.position < right.position;
            });
  for (const PrintTemplateColumn& column : columns) {
    if (!column.visible) continue;
    const PrintColumnDef* def = find_print_column(*target, column.column_key);
    if (def == nullptr) continue;
    PrintRenderColumn render;
    render.def = *def;
    render.width_mm = column.width_mm;
    render.align = column.align.empty() ? def->align : column.align;
    render.font_size_pt = column.font_size_pt;
    render.bold = column.bold;
    result.push_back(std::move(render));
  }
  return result;
}

std::vector<PrintTemplate> PrintTemplateService::list(std::string_view target_type,
                                                      std::string_view target_key) const {
  std::vector<PrintTemplate> rows;
  db_.read([&](sqlite3* conn) {
    std::string sql = std::string(kTemplateSelect) + " WHERE is_deleted = 0";
    if (!target_type.empty()) sql += " AND target_type = ?";
    if (!target_key.empty()) sql += " AND target_key = ?";
    sql += " ORDER BY target_type, target_key, is_default DESC, id;";
    {
      Stmt stmt(conn, sql);
      std::int64_t index = 1;
      if (!target_type.empty()) stmt.bind(index++, target_type);
      if (!target_key.empty()) stmt.bind(index++, target_key);
      while (stmt.step()) {
        rows.push_back(template_from_row(stmt));
      }
    }
    for (PrintTemplate& row : rows) {
      load_columns(conn, row);
    }
  });
  return rows;
}

PrintTemplate PrintTemplateService::get(std::int64_t id) const {
  PrintTemplate tmpl;
  bool found = false;
  db_.read([&](sqlite3* conn) {
    {
      Stmt stmt(conn, std::string(kTemplateSelect) + " WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, id);
      if (stmt.step()) {
        tmpl = template_from_row(stmt);
        found = true;
      }
    }
    if (found) load_columns(conn, tmpl);
  });
  if (!found) {
    throw AppError("NOT_FOUND", "打印模板不存在或已删除");
  }
  return tmpl;
}

PrintTemplate PrintTemplateService::create(const PrintTemplate& input) {
  PrintTemplate tmpl = input;
  validate_template(tmpl);
  const AuditStamp stamp = audit_stamp();
  db_.write([&](sqlite3* conn) {
    ensure_code_free(conn, tmpl.code, 0);
    if (tmpl.is_default) {
      clear_default(conn, tmpl.target_type, tmpl.target_key, 0);
    }
    Stmt stmt(conn,
              "INSERT INTO print_template"
              " (code, name, target_type, target_key, page_size, orientation,"
              "  paper_width_mm, paper_height_mm, margin_mm, font_size_pt,"
              "  header_text, footer_text, show_page_no, is_default, status, version,"
              "  is_deleted, created_at, created_by, updated_at, updated_by)"
              " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 0, ?, ?, ?, ?);");
    stmt.bind(1, tmpl.code)
        .bind(2, tmpl.name)
        .bind(3, tmpl.target_type)
        .bind(4, tmpl.target_key)
        .bind(5, tmpl.page_size)
        .bind(6, tmpl.orientation)
        .bind(7, tmpl.paper_width_mm)
        .bind(8, tmpl.paper_height_mm)
        .bind(9, tmpl.margin_mm)
        .bind(10, tmpl.font_size_pt)
        .bind(11, tmpl.header_text)
        .bind(12, tmpl.footer_text)
        .bind(13, static_cast<std::int64_t>(tmpl.show_page_no ? 1 : 0))
        .bind(14, static_cast<std::int64_t>(tmpl.is_default ? 1 : 0))
        .bind(15, static_cast<std::int64_t>(tmpl.active ? 1 : 0))
        .bind(16, stamp.at)
        .bind(17, stamp.by)
        .bind(18, stamp.at)
        .bind(19, stamp.by)
        .run();
    tmpl.id = sqlite3_last_insert_rowid(conn);
    tmpl.version = 1;
    insert_columns(conn, tmpl.id, tmpl.columns);
    audit_write(conn, "create", "print_template", tmpl.id, tmpl.code);
  });
  tmpl.created_at = stamp.at;
  tmpl.created_by = stamp.by;
  tmpl.updated_at = stamp.at;
  tmpl.updated_by = stamp.by;
  return tmpl;
}

PrintTemplate PrintTemplateService::update(std::int64_t id, const PrintTemplate& input) {
  get(id);  // 不存在或已删除直接 404，避免把版本冲突误报成存在性错误。
  PrintTemplate tmpl = input;
  validate_template(tmpl);
  const AuditStamp stamp = audit_stamp();
  db_.write([&](sqlite3* conn) {
    ensure_code_free(conn, tmpl.code, id);
    if (tmpl.is_default) {
      clear_default(conn, tmpl.target_type, tmpl.target_key, id);
    }
    {
      Stmt stmt(conn,
                "UPDATE print_template SET code = ?, name = ?, target_type = ?, target_key = ?,"
                " page_size = ?, orientation = ?, paper_width_mm = ?, paper_height_mm = ?,"
                " margin_mm = ?, font_size_pt = ?, header_text = ?, footer_text = ?,"
                " show_page_no = ?, is_default = ?, status = ?, version = version + 1,"
                " updated_at = ?, updated_by = ?"
                " WHERE id = ? AND version = ? AND is_deleted = 0;");
      stmt.bind(1, tmpl.code)
          .bind(2, tmpl.name)
          .bind(3, tmpl.target_type)
          .bind(4, tmpl.target_key)
          .bind(5, tmpl.page_size)
          .bind(6, tmpl.orientation)
          .bind(7, tmpl.paper_width_mm)
          .bind(8, tmpl.paper_height_mm)
          .bind(9, tmpl.margin_mm)
          .bind(10, tmpl.font_size_pt)
          .bind(11, tmpl.header_text)
          .bind(12, tmpl.footer_text)
          .bind(13, static_cast<std::int64_t>(tmpl.show_page_no ? 1 : 0))
          .bind(14, static_cast<std::int64_t>(tmpl.is_default ? 1 : 0))
          .bind(15, static_cast<std::int64_t>(tmpl.active ? 1 : 0))
          .bind(16, stamp.at)
          .bind(17, stamp.by)
          .bind(18, id)
          .bind(19, tmpl.version)
          .run();
      if (sqlite3_changes(conn) == 0) {
        Stmt check(conn, "SELECT 1 FROM print_template WHERE id = ? AND is_deleted = 0;");
        check.bind(1, id);
        if (!check.step()) {
          throw AppError("NOT_FOUND", "打印模板不存在或已删除");
        }
        throw AppError("VERSION_CONFLICT", "模板已被其他人修改，请刷新后重试");
      }
    }
    {
      Stmt remove_columns(conn, "DELETE FROM print_template_column WHERE template_id = ?;");
      remove_columns.bind(1, id).run();
    }
    insert_columns(conn, id, tmpl.columns);
    audit_write(conn, "update", "print_template", id, tmpl.code);
  });
  return get(id);
}

void PrintTemplateService::remove(std::int64_t id) {
  db_.write([&](sqlite3* conn) {
    std::string code;
    {
      Stmt stmt(conn, "SELECT code FROM print_template WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, id);
      if (stmt.step()) {
        code = stmt.text(0);
      }
    }
    {
      Stmt stmt(conn, "UPDATE print_template SET is_default = 0 WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, id).run();
    }
    soft_delete(conn, "print_template", id);
    audit_write(conn, "delete", "print_template", id, code);
  });
}

PrintTemplate PrintTemplateService::set_default(std::int64_t id) {
  const PrintTemplate current = get(id);
  const AuditStamp stamp = audit_stamp();
  db_.write([&](sqlite3* conn) {
    clear_default(conn, current.target_type, current.target_key, id);
    Stmt stmt(conn,
              "UPDATE print_template SET is_default = 1, version = version + 1,"
              " updated_at = ?, updated_by = ? WHERE id = ? AND is_deleted = 0;");
    stmt.bind(1, stamp.at).bind(2, stamp.by).bind(3, id).run();
    audit_write(conn, "update", "print_template", id, current.code, "设为默认模板");
  });
  return get(id);
}

PrintTemplate PrintTemplateService::resolve(std::string_view target_type,
                                            std::string_view target_key,
                                            std::int64_t template_id) const {
  PrintTemplate tmpl;
  bool found = false;
  db_.read([&](sqlite3* conn) {
    if (template_id > 0) {
      {
        Stmt stmt(conn, std::string(kTemplateSelect) +
                            " WHERE id = ? AND is_deleted = 0 AND status = 1"
                            " AND target_type = ? AND target_key = ?;");
        stmt.bind(1, template_id).bind(2, target_type).bind(3, target_key);
        if (stmt.step()) {
          tmpl = template_from_row(stmt);
          found = true;
        }
      }
      if (found) {
        load_columns(conn, tmpl);
      }
    }
    if (!found) {
      {
        Stmt stmt(conn, std::string(kTemplateSelect) +
                            " WHERE target_type = ? AND target_key = ?"
                            " AND is_deleted = 0 AND status = 1 AND is_default = 1 LIMIT 1;");
        stmt.bind(1, target_type).bind(2, target_key);
        if (stmt.step()) {
          tmpl = template_from_row(stmt);
          found = true;
        }
      }
      if (found) {
        load_columns(conn, tmpl);
      }
    }
  });
  if (found) {
    return tmpl;
  }
  return builtin_print_template(target_type, target_key);
}

}  // namespace xiaotu
