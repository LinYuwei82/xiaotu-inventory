#include <httplib.h>

#include "xiaotu/services.h"

namespace xiaotu {

void register_report_stock_balance_routes(httplib::Server& server, Services& services);
void register_report_inventory_summary_routes(httplib::Server& server, Services& services);
void register_report_goods_flow_routes(httplib::Server& server, Services& services);
void register_report_other_flow_routes(httplib::Server& server, Services& services);
void register_report_stock_ledger_routes(httplib::Server& server, Services& services);
void register_report_cost_recalc_routes(httplib::Server& server, Services& services);
void register_report_sales_profit_routes(httplib::Server& server, Services& services);
void register_report_sales_summary_routes(httplib::Server& server, Services& services);
void register_report_sales_detail_routes(httplib::Server& server, Services& services);
void register_report_dashboard_routes(httplib::Server& server, Services& services);
void register_report_purchase_detail_routes(httplib::Server& server, Services& services);
void register_report_purchase_summary_routes(httplib::Server& server, Services& services);
void register_report_statement_routes(httplib::Server& server, Services& services);
void register_report_aging_routes(httplib::Server& server, Services& services);
void register_report_fund_ledger_routes(httplib::Server& server, Services& services);
void register_report_fund_balances_routes(httplib::Server& server, Services& services);

void register_report_routes(httplib::Server& server, Services& services) {
  register_report_stock_balance_routes(server, services);
  register_report_inventory_summary_routes(server, services);
  register_report_goods_flow_routes(server, services);
  register_report_other_flow_routes(server, services);
  register_report_stock_ledger_routes(server, services);
  register_report_cost_recalc_routes(server, services);
  register_report_sales_profit_routes(server, services);
  register_report_sales_summary_routes(server, services);
  register_report_sales_detail_routes(server, services);
  register_report_dashboard_routes(server, services);
  register_report_purchase_detail_routes(server, services);
  register_report_purchase_summary_routes(server, services);
  register_report_statement_routes(server, services);
  register_report_aging_routes(server, services);
  register_report_fund_ledger_routes(server, services);
  register_report_fund_balances_routes(server, services);
}

}  // namespace xiaotu
