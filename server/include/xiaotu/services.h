#pragma once

#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/finance_service.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/cost_recalc.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/print_template.h"
#include "xiaotu/report_service.h"
#include "xiaotu/settlement_service.h"
#include "xiaotu/stock_service.h"
#include "xiaotu/user_service.h"

namespace xiaotu {

// 服务组合根：HTTP 层只依赖这里，不直接接触 DAO 与连接。
struct Services {
  explicit Services(Db& database)
      : db(database),
        item(database),
        warehouse(database),
        supplier(database),
        customer(database),
        unit(database),
        docs(database),
        stock(database),
        finance(database),
        settlements(database),
        reports(database),
        print_templates(database),
        cost_recalc(database),
        users(database) {}

  Db& db;
  gen::ItemService item;
  gen::WarehouseService warehouse;
  gen::SupplierService supplier;
  gen::CustomerService customer;
  gen::UnitService unit;
  DocumentEngine docs;
  StockService stock;
  FinanceService finance;
  SettlementService settlements;
  ReportService reports;
  PrintTemplateService print_templates;
  CostRecalcService cost_recalc;
  UserService users;
};

}  // namespace xiaotu
