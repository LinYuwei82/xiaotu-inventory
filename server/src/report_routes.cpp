#include <httplib.h>

#include "xiaotu/services.h"

namespace xiaotu {

void register_report_stock_routes(httplib::Server& server, Services& services);
void register_report_sales_routes(httplib::Server& server, Services& services);
void register_report_purchase_routes(httplib::Server& server, Services& services);
void register_report_finance_routes(httplib::Server& server, Services& services);

void register_report_routes(httplib::Server& server, Services& services) {
  register_report_stock_routes(server, services);
  register_report_sales_routes(server, services);
  register_report_purchase_routes(server, services);
  register_report_finance_routes(server, services);
}

}  // namespace xiaotu
