#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>

#include "xiaotu/print_template.h"
#include "xiaotu/report_service.h"

namespace xiaotu {

class Services;

// 打印渲染的统一产物：标题/副标题、表格行、抬头字段、合计与页眉页脚占位符。
struct PrintRender {
  std::string title;
  std::string subtitle;
  nlohmann::json rows = nlohmann::json::array();
  nlohmann::json header_fields = nlohmann::json::array();
  nlohmann::json totals = nlohmann::json::object();
  std::map<std::string, std::string> placeholders;
};

// 报表打印的输入：服务、目标定义、请求参数、解析后的筛选条件与输出目标。
struct PrintReportContext {
  Services& services;
  const PrintTargetDef& target;
  const httplib::Request& req;
  const ReportFilter& filter;
  PrintRender& render;
};

// 请求参数解析：打印与报表导出共用同一套筛选语义。
std::int64_t param_i64(const httplib::Request& req, const char* name, std::int64_t fallback);
std::string param_text(const httplib::Request& req, const char* name);
ReportFilter report_filter(const httplib::Request& req);

// 展示格式化：金额、税率、毛利率、期间与维度标签等；报表与单据打印共用。
std::string period_label(const std::string& from, const std::string& to);
std::string dimension_label(std::int64_t id, const char* name);
// 金额展示文本（元）。与 core 的 masterdata_io::money_text 区分，这里只服务打印。
std::string print_money_text(std::int64_t cents);
std::string rate_text(std::int64_t bp);
std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents);
std::string doc_type_label(const std::string& doc_type);
std::string stock_biz_label(const std::string& biz_type);
std::string qty_direction_label(int direction);
std::string ledger_biz_label(const LedgerDetailRow& row);
std::string stock_biz_category_label(const std::string& category);
std::string doc_status_label(const std::string& status);
std::string apply_placeholders(std::string text, const std::map<std::string, std::string>& values);

// 打印目标本身的数据权限码（渲染不要求模板管理权限）。
const char* data_permission(const PrintTargetDef& target);

// 报表类打印目标：按 target.key 分派到各自的渲染函数；未知 key 按对账单处理。
void render_report(PrintReportContext& ctx);
void render_sales_summary(PrintReportContext& ctx);
void render_stock_balance(PrintReportContext& ctx);
void render_stock_ledger(PrintReportContext& ctx);
void render_goods_flow(PrintReportContext& ctx);
void render_inventory_summary(PrintReportContext& ctx);
void render_sales_profit(PrintReportContext& ctx);
void render_purchase_detail(PrintReportContext& ctx);
void render_purchase_summary_supplier(PrintReportContext& ctx);
void render_purchase_summary_item(PrintReportContext& ctx);
void render_other_flow_summary(PrintReportContext& ctx);
void render_fund_ledger(PrintReportContext& ctx);
void render_ar_ap_aging(PrintReportContext& ctx);
void render_partner_statement(PrintReportContext& ctx);

// 单据类打印目标。
void render_document(Services& services, const PrintTargetDef& target, const httplib::Request& req,
                     PrintRender& render);

}  // namespace xiaotu
