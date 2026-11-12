#pragma once

#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/gen/warehouse_service.h"
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
        unit(database),
        docs(database),
        stock(database),
        users(database) {}

  Db& db;
  gen::ItemService item;
  gen::WarehouseService warehouse;
  gen::SupplierService supplier;
  gen::UnitService unit;
  DocumentEngine docs;
  StockService stock;
  UserService users;
};

}  // namespace xiaotu
