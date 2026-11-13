#pragma once

#include <filesystem>

#include "xiaotu/customer_service.h"
#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/finance_service.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/employee_service.h"
#include "xiaotu/gen/fund_account_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/cost_recalc.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/print_template.h"
#include "xiaotu/report_service.h"
#include "xiaotu/settlement_service.h"
#include "xiaotu/setting_service.h"
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
        employee(database),
        fund_account(database),
        docs(database),
        stock(database),
        finance(database),
        settlements(database),
        reports(database),
        print_templates(database),
        cost_recalc(database),
        settings(database),
        users(database) {}

  Db& db;
  // 解析后的备份目录（[storage] backup_dir 展开结果）；空表示 <数据目录>/backups。
  std::filesystem::path backup_dir;
  gen::ItemService item;
  gen::WarehouseService warehouse;
  gen::SupplierService supplier;
  CustomerService customer;
  gen::UnitService unit;
  gen::EmployeeService employee;
  gen::FundAccountService fund_account;
  DocumentEngine docs;
  StockService stock;
  FinanceService finance;
  SettlementService settlements;
  ReportService reports;
  PrintTemplateService print_templates;
  CostRecalcService cost_recalc;
  SettingService settings;
  UserService users;

  // 实际备份目录：未显式设置时退回数据库所在目录下的 backups。
  std::filesystem::path effective_backup_dir() const {
    if (!backup_dir.empty()) {
      return backup_dir;
    }
    return db.file().parent_path() / "backups";
  }
};

}  // namespace xiaotu
