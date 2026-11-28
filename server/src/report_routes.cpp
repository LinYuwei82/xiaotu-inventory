#include <httplib.h>

#include "xiaotu/services.h"

namespace xiaotu {

void register_report_stock_routes(httplib::Server& server, Services& services);
void register_report_stock_flow_routes(httplib::Server& server, Services& services);
void register_report_stock_ledger_routes(httplib::Server& server, Services& services);
void register_report_sales_routes(httplib::Server& server, Services& services);
void register_report_sales_detail_routes(httplib::Server& server, Services& services);
void register_report_purchase_detail_routes(httplib::Server& server, Services& services);
void register_report_purchase_summary_routes(httplib::Server& server, Services& services);
void register_report_statement_routes(httplib::Server& server, Services& services);
void register_report_aging_routes(httplib::Server& server, Services& services);
void register_report_fund_routes(httplib::Server& server, Services& services);

void register_report_routes(httplib::Server& server, Services& services) {
  register_report_stock_routes(server, services);
  register_report_stock_flow_routes(server, services);
  register_report_stock_ledger_routes(server, services);
  register_report_sales_routes(server, services);
  register_report_sales_detail_routes(server, services);
  register_report_purchase_detail_routes(server, services);
  register_report_purchase_summary_routes(server, services);
  register_report_statement_routes(server, services);
  register_report_aging_routes(server, services);
  register_report_fund_routes(server, services);
}

}  // namespace xiaotu
