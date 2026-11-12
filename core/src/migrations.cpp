#include "xiaotu/migrations.h"

#include <sqlite3.h>

#include <cstdio>
#include <map>
#include <string>

#include "xiaotu/error.h"

namespace xiaotu {
namespace {

constexpr const char* kV1 = R"SQL(
CREATE TABLE item (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  spec       TEXT    NOT NULL DEFAULT '',
  unit       TEXT    NOT NULL DEFAULT '件',
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_item_code ON item(code);

CREATE TABLE warehouse (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  is_default INTEGER NOT NULL DEFAULT 0,
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (is_default IN (0, 1)),
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_warehouse_code ON warehouse(code);

CREATE TABLE doc_header (
  id                 INTEGER PRIMARY KEY,
  doc_no             TEXT    NOT NULL,
  doc_type           TEXT    NOT NULL,
  doc_date           TEXT    NOT NULL,
  warehouse_id       INTEGER NOT NULL REFERENCES warehouse(id),
  customer           TEXT    NOT NULL DEFAULT '',
  status             TEXT    NOT NULL,
  total_qty          INTEGER NOT NULL DEFAULT 0,
  total_amount_cents INTEGER NOT NULL DEFAULT 0,
  created_at         TEXT    NOT NULL,
  created_by         TEXT    NOT NULL DEFAULT 'system',
  approved_at        TEXT,
  approved_by        TEXT,
  CHECK (status IN ('DRAFT', 'APPROVED', 'VOIDED'))
);
CREATE UNIQUE INDEX uk_doc_header_no ON doc_header(doc_no);
CREATE INDEX idx_doc_header_type_date ON doc_header(doc_type, doc_date);
CREATE INDEX idx_doc_header_warehouse ON doc_header(warehouse_id, doc_date);

CREATE TABLE doc_line (
  id           INTEGER PRIMARY KEY,
  doc_id       INTEGER NOT NULL REFERENCES doc_header(id),
  line_no      INTEGER NOT NULL,
  item_id      INTEGER NOT NULL REFERENCES item(id),
  qty          INTEGER NOT NULL,
  price_cents  INTEGER NOT NULL,
  amount_cents INTEGER NOT NULL,
  CHECK (qty > 0),
  CHECK (price_cents >= 0),
  CHECK (amount_cents >= 0)
);
CREATE UNIQUE INDEX uk_doc_line_no ON doc_line(doc_id, line_no);
CREATE INDEX idx_doc_line_item ON doc_line(item_id);

-- 库存流水：只增不改，是库存与成本的事实来源。
CREATE TABLE stock_ledger (
  id           INTEGER PRIMARY KEY,
  doc_id       INTEGER NOT NULL,
  doc_line_id  INTEGER NOT NULL,
  biz_type     TEXT    NOT NULL,
  biz_time     TEXT    NOT NULL,
  warehouse_id INTEGER NOT NULL REFERENCES warehouse(id),
  item_id      INTEGER NOT NULL REFERENCES item(id),
  qty          INTEGER NOT NULL,
  amount_cents INTEGER NOT NULL,
  created_at   TEXT    NOT NULL,
  CHECK (qty <> 0)
);
CREATE INDEX idx_stock_ledger_item_wh_time ON stock_ledger(item_id, warehouse_id, biz_time, id);
CREATE INDEX idx_stock_ledger_doc ON stock_ledger(doc_id);
CREATE UNIQUE INDEX uk_stock_ledger_doc_line ON stock_ledger(doc_line_id, biz_type);

-- 库存余额：由流水派生，可随时按流水重建。
CREATE TABLE stock_balance (
  warehouse_id INTEGER NOT NULL REFERENCES warehouse(id),
  item_id      INTEGER NOT NULL REFERENCES item(id),
  qty          INTEGER NOT NULL DEFAULT 0,
  amount_cents INTEGER NOT NULL DEFAULT 0,
  updated_at   TEXT    NOT NULL,
  PRIMARY KEY (warehouse_id, item_id)
);
CREATE INDEX idx_stock_balance_item ON stock_balance(item_id);

CREATE TABLE doc_seq (
  doc_type   TEXT    NOT NULL,
  period_key TEXT    NOT NULL,
  next_val   INTEGER NOT NULL,
  PRIMARY KEY (doc_type, period_key)
);

INSERT INTO warehouse (id, code, name, is_default, status, is_deleted, created_at, created_by, updated_at, updated_by)
VALUES (1, 'MAIN', '主仓库', 1, 1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system');
)SQL";

// v2：用户与角色。v1 的表结构保持原样，这里只新增表与内置数据。
constexpr const char* kV2 = R"SQL(
CREATE TABLE app_role (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  is_builtin INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  updated_at TEXT    NOT NULL,
  CHECK (is_builtin IN (0, 1))
);
CREATE UNIQUE INDEX uk_app_role_code ON app_role(code);

-- 角色与权限点：只在启动时整体载入内存缓存，运行期不写。
CREATE TABLE app_role_permission (
  role_id    INTEGER NOT NULL REFERENCES app_role(id),
  permission TEXT    NOT NULL,
  PRIMARY KEY (role_id, permission)
);

CREATE TABLE app_user (
  id            INTEGER PRIMARY KEY,
  username      TEXT    NOT NULL,
  display_name  TEXT    NOT NULL DEFAULT '',
  role_id       INTEGER NOT NULL REFERENCES app_role(id),
  password_hash TEXT    NOT NULL DEFAULT '',
  status        INTEGER NOT NULL DEFAULT 1,
  is_deleted    INTEGER NOT NULL DEFAULT 0,
  created_at    TEXT    NOT NULL,
  created_by    TEXT    NOT NULL DEFAULT 'system',
  updated_at    TEXT    NOT NULL,
  updated_by    TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_app_user_username ON app_user(username);

INSERT INTO app_role (id, code, name, is_builtin, created_at, updated_at) VALUES
  (1, 'admin',    '管理员', 1, datetime('now', 'localtime'), datetime('now', 'localtime')),
  (2, 'operator', '操作员', 1, datetime('now', 'localtime'), datetime('now', 'localtime')),
  (3, 'viewer',   '只读',   1, datetime('now', 'localtime'), datetime('now', 'localtime'));

-- admin 保留通配权限，新增权限点无需再补行；其余角色按最小集合授予。
INSERT INTO app_role_permission (role_id, permission) VALUES
  (1, '*'),
  (2, 'item.read'), (2, 'item.write'),
  (2, 'warehouse.read'), (2, 'warehouse.write'),
  (2, 'document.read'), (2, 'document.write'),
  (2, 'stock.read'),
  (3, 'item.read'), (3, 'warehouse.read'), (3, 'document.read'), (3, 'stock.read');

-- 内置管理员：一次性 token 换取会话时使用；口令登录在 M5 接入。
INSERT INTO app_user (id, username, display_name, role_id, password_hash, status, is_deleted,
                      created_at, created_by, updated_at, updated_by)
VALUES (1, 'admin', '管理员', 1, '', 1, 0,
        datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system');
)SQL";

// v3：供应商档案，并让单据可以引用供应商（名称快照保留在 doc_header.customer）。
constexpr const char* kV3 = R"SQL(
CREATE TABLE supplier (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  contact    TEXT    NOT NULL DEFAULT '',
  phone      TEXT    NOT NULL DEFAULT '',
  address    TEXT    NOT NULL DEFAULT '',
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_supplier_code ON supplier(code);

-- 0 表示未引用档案（历史单据与销售类单据）；非 0 时指向 supplier。
ALTER TABLE doc_header ADD COLUMN partner_id INTEGER NOT NULL DEFAULT 0;

INSERT INTO app_role_permission (role_id, permission) VALUES
  (2, 'supplier.read'), (2, 'supplier.write'),
  (3, 'supplier.read');
)SQL";

// v4：计量单位档案。商品保留 unit 文本快照，新增 unit_id 指向档案，老数据 unit_id=0 继续按文本读取。
constexpr const char* kV4 = R"SQL(
CREATE TABLE unit (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_unit_code ON unit(code);

ALTER TABLE item ADD COLUMN unit_id INTEGER NOT NULL DEFAULT 0;

INSERT INTO unit (id, code, name, status, is_deleted, created_at, created_by, updated_at, updated_by) VALUES
  (1, 'JIAN',  '件',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (2, 'PING',  '瓶',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (3, 'GE',    '个',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (4, 'XIANG', '箱',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (5, 'BAO',   '包',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (6, 'MI',    '米',   1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'),
  (7, 'KG',    '千克', 1, 0, datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system');

INSERT INTO app_role_permission (role_id, permission) VALUES
  (2, 'unit.read'), (2, 'unit.write'),
  (3, 'unit.read');
)SQL";

std::string checksum_of(std::string_view sql) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : sql) {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(buffer);
}

}  // namespace

const std::vector<Migration>& all_migrations() {
  static const std::vector<Migration> migrations = {
      {1, "v1_initial_schema", kV1},
      {2, "v2_user_and_role", kV2},
      {3, "v3_supplier", kV3},
      {4, "v4_unit", kV4},
  };
  return migrations;
}

void migrate(Db& db) {
  db.write_raw([&](sqlite3* conn) {
    exec_sql(conn,
             "CREATE TABLE IF NOT EXISTS schema_migration ("
             "  version    INTEGER PRIMARY KEY,"
             "  name       TEXT NOT NULL,"
             "  checksum   TEXT NOT NULL,"
             "  applied_at TEXT NOT NULL"
             ");");
  });

  std::map<int, std::string> applied;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT version, checksum FROM schema_migration ORDER BY version;");
    while (stmt.step()) {
      applied[static_cast<int>(stmt.i64(0))] = stmt.text(1);
    }
  });

  for (const Migration& migration : all_migrations()) {
    const std::string checksum = checksum_of(migration.sql);
    const auto existing = applied.find(migration.version);
    if (existing != applied.end()) {
      // 已发布的迁移脚本不允许被修改，否则不同版本的程序会写出不同的库结构。
      if (existing->second != checksum) {
        throw AppError("MIGRATION_CHECKSUM",
                       "迁移脚本 v" + std::to_string(migration.version) + " 的内容与数据库记录不一致");
      }
      continue;
    }

    db.write_raw([&](sqlite3* conn) {
      exec_sql(conn, "BEGIN IMMEDIATE;");
      try {
        exec_sql(conn, migration.sql);
        Stmt record(conn,
                    "INSERT INTO schema_migration (version, name, checksum, applied_at)"
                    " VALUES (?, ?, ?, ?);");
        record.bind(1, static_cast<std::int64_t>(migration.version))
            .bind(2, migration.name)
            .bind(3, checksum)
            .bind(4, now_text())
            .run();
        exec_sql(conn, "PRAGMA user_version = " + std::to_string(migration.version) + ";");
        exec_sql(conn, "COMMIT;");
      } catch (...) {
        try {
          exec_sql(conn, "ROLLBACK;");
        } catch (...) {
        }
        throw;
      }
    });
  }
}

}  // namespace xiaotu
