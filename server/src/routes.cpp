#include "xiaotu/routes.h"

#include <httplib.h>

#include "xiaotu/gen/customer_routes.h"
#include "xiaotu/gen/fund_account_routes.h"
#include "xiaotu/gen/item_routes.h"
#include "xiaotu/gen/supplier_routes.h"
#include "xiaotu/gen/unit_routes.h"
#include "xiaotu/gen/warehouse_routes.h"
#include "xiaotu/services.h"

namespace xiaotu {

void register_document_routes(httplib::Server& server, Services& services);
void register_stock_routes(httplib::Server& server, Services& services);
void register_user_routes(httplib::Server& server, Services& services);
void register_audit_routes(httplib::Server& server, Services& services);
void register_finance_routes(httplib::Server& server, Services& services);
void register_report_routes(httplib::Server& server, Services& services);
void register_print_routes(httplib::Server& server, Services& services);
void register_masterdata_routes(httplib::Server& server, Services& services);

void register_all_routes(httplib::Server& server, Services& services) {
  gen::register_item_routes(server, services);
  gen::register_warehouse_routes(server, services);
  gen::register_supplier_routes(server, services);
  gen::register_customer_routes(server, services);
  gen::register_unit_routes(server, services);
  gen::register_fund_account_routes(server, services);
  register_masterdata_routes(server, services);
  register_document_routes(server, services);
  register_stock_routes(server, services);
  register_user_routes(server, services);
  register_audit_routes(server, services);
  register_finance_routes(server, services);
  register_report_routes(server, services);
  register_print_routes(server, services);
}

}  // namespace xiaotu
