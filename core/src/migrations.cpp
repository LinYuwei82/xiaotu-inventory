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

// v5：审计日志。只追加，不提供修改与删除；不建外键，对象被删除后仍可追溯。
constexpr const char* kV5 = R"SQL(
CREATE TABLE audit_log (
  id        INTEGER PRIMARY KEY,
  at        TEXT    NOT NULL,
  actor     TEXT    NOT NULL,
  action    TEXT    NOT NULL,
  entity    TEXT    NOT NULL,
  entity_id INTEGER NOT NULL DEFAULT 0,
  entity_no TEXT    NOT NULL DEFAULT '',
  detail    TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX idx_audit_entity ON audit_log(entity, entity_id);
CREATE INDEX idx_audit_at ON audit_log(at);
)SQL";

// v6：调拨单需要调入仓库；0 表示非调拨单据。
constexpr const char* kV6 = R"SQL(
ALTER TABLE doc_header ADD COLUMN target_warehouse_id INTEGER NOT NULL DEFAULT 0;
)SQL";

// v7：客户档案、单据客户引用、应收应付台账与核销（只追加）。
constexpr const char* kV7 = R"SQL(
CREATE TABLE customer (
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
CREATE UNIQUE INDEX uk_customer_code ON customer(code);
CREATE INDEX idx_customer_name ON customer(name);

ALTER TABLE doc_header ADD COLUMN customer_id INTEGER NOT NULL DEFAULT 0;

-- 存量回填：按去重名称建档（同名合并），只补 customer_id，不改金额与单号；重复执行不会重复建档。
INSERT INTO customer (code, name, contact, phone, address, status, is_deleted,
                      created_at, created_by, updated_at, updated_by)
SELECT 'KH' || printf('%04d', COALESCE((SELECT MAX(id) FROM customer), 0)
                             + ROW_NUMBER() OVER (ORDER BY h.name)),
       h.name, '', '', '', 1, 0,
       datetime('now', 'localtime'), 'system', datetime('now', 'localtime'), 'system'
FROM (SELECT DISTINCT customer AS name FROM doc_header
      WHERE customer <> '' AND customer_id = 0) h
WHERE NOT EXISTS (SELECT 1 FROM customer c WHERE c.name = h.name);

UPDATE doc_header
SET customer_id = (SELECT id FROM customer c WHERE c.name = doc_header.customer)
WHERE customer_id = 0 AND customer <> '';

-- 往来台账：一张单据一条，退货为负数（红字），反审核置 VOIDED。
CREATE TABLE ar_ap_entry (
  id            INTEGER PRIMARY KEY,
  doc_id        INTEGER NOT NULL,
  doc_no        TEXT    NOT NULL,
  doc_type      TEXT    NOT NULL,
  direction     INTEGER NOT NULL,
  partner_id    INTEGER NOT NULL DEFAULT 0,
  partner_name  TEXT    NOT NULL,
  partner_key   TEXT    NOT NULL,
  biz_date      TEXT    NOT NULL,
  due_date      TEXT    NOT NULL DEFAULT '',
  amount_cents  INTEGER NOT NULL,
  settled_cents INTEGER NOT NULL DEFAULT 0,
  status        TEXT    NOT NULL DEFAULT 'OPEN',
  created_at    TEXT    NOT NULL,
  created_by    TEXT    NOT NULL DEFAULT 'system',
  CHECK (direction IN (1, -1)),
  CHECK (status IN ('OPEN', 'PARTIAL', 'SETTLED', 'VOIDED'))
);
CREATE UNIQUE INDEX uk_ar_ap_doc ON ar_ap_entry(doc_id);
CREATE INDEX idx_ar_ap_partner ON ar_ap_entry(partner_key, direction, status);
CREATE INDEX idx_ar_ap_biz_date ON ar_ap_entry(biz_date);

-- 核销记录：只追加；撤销生成负数记录并指向原记录。
CREATE TABLE settlement (
  id           INTEGER PRIMARY KEY,
  settle_no    TEXT    NOT NULL,
  direction    INTEGER NOT NULL,
  partner_key  TEXT    NOT NULL,
  partner_name TEXT    NOT NULL,
  amount_cents INTEGER NOT NULL,
  settled_on   TEXT    NOT NULL,
  reverses_id  INTEGER NOT NULL DEFAULT 0,
  note         TEXT    NOT NULL DEFAULT '',
  created_at   TEXT    NOT NULL,
  created_by   TEXT    NOT NULL DEFAULT 'system',
  CHECK (direction IN (1, -1))
);
CREATE UNIQUE INDEX uk_settlement_no ON settlement(settle_no);
CREATE INDEX idx_settlement_partner ON settlement(partner_key, direction, settled_on);

-- 核销分摊：事实来源，entry 上的 settled_cents 只是同事务内的汇总缓存。
CREATE TABLE settlement_allocation (
  id            INTEGER PRIMARY KEY,
  settlement_id INTEGER NOT NULL,
  entry_id      INTEGER NOT NULL,
  amount_cents  INTEGER NOT NULL,
  created_at    TEXT    NOT NULL
);
CREATE UNIQUE INDEX uk_settlement_alloc ON settlement_allocation(settlement_id, entry_id);
CREATE INDEX idx_settlement_alloc_entry ON settlement_allocation(entry_id);

INSERT INTO app_role_permission (role_id, permission) VALUES
  (2, 'customer.read'), (2, 'customer.write'),
  (2, 'finance.read'), (2, 'finance.write'),
  (3, 'customer.read'), (3, 'finance.read');
)SQL";

// v8：自定义打印模板。模板只保存展示配置（纸张、页边距、字号、页眉页脚、列显示），
// 不保存 SQL、脚本或查询条件；列配置用子表，删除模板时一并软删除。
constexpr const char* kV8 = R"SQL(
CREATE TABLE print_template (
  id              INTEGER PRIMARY KEY,
  code            TEXT    NOT NULL,
  name            TEXT    NOT NULL,
  target_type     TEXT    NOT NULL,
  target_key      TEXT    NOT NULL,
  page_size       TEXT    NOT NULL DEFAULT 'A4',
  orientation     TEXT    NOT NULL DEFAULT 'PORTRAIT',
  paper_width_mm  INTEGER NOT NULL DEFAULT 0,
  paper_height_mm INTEGER NOT NULL DEFAULT 0,
  margin_mm       INTEGER NOT NULL DEFAULT 12,
  font_size_pt    INTEGER NOT NULL DEFAULT 10,
  header_text     TEXT    NOT NULL DEFAULT '',
  footer_text     TEXT    NOT NULL DEFAULT '',
  show_page_no    INTEGER NOT NULL DEFAULT 1,
  is_default      INTEGER NOT NULL DEFAULT 0,
  status          INTEGER NOT NULL DEFAULT 1,
  version         INTEGER NOT NULL DEFAULT 1,
  is_deleted      INTEGER NOT NULL DEFAULT 0,
  created_at      TEXT    NOT NULL,
  created_by      TEXT    NOT NULL DEFAULT 'system',
  updated_at      TEXT    NOT NULL,
  updated_by      TEXT    NOT NULL DEFAULT 'system',
  CHECK (target_type IN ('DOC', 'REPORT')),
  CHECK (page_size IN ('A4', 'A5', 'CUSTOM')),
  CHECK (orientation IN ('PORTRAIT', 'LANDSCAPE')),
  CHECK (paper_width_mm BETWEEN 0 AND 1000),
  CHECK (paper_height_mm BETWEEN 0 AND 1000),
  CHECK (page_size <> 'CUSTOM' OR (paper_width_mm > 0 AND paper_height_mm > 0)),
  CHECK (margin_mm BETWEEN 0 AND 50),
  CHECK (font_size_pt BETWEEN 6 AND 24),
  CHECK (length(header_text) <= 500),
  CHECK (length(footer_text) <= 500),
  CHECK (show_page_no IN (0, 1)),
  CHECK (is_default IN (0, 1)),
  CHECK (status IN (0, 1)),
  CHECK (version >= 1),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_print_template_code ON print_template(code);
CREATE INDEX idx_print_template_target ON print_template(target_type, target_key);
-- 每个目标最多一个默认模板；软删除后让位，允许重新指定默认。
CREATE UNIQUE INDEX uk_print_template_default ON print_template(target_type, target_key)
  WHERE is_default = 1 AND is_deleted = 0;

CREATE TABLE print_template_column (
  id           INTEGER PRIMARY KEY,
  template_id  INTEGER NOT NULL REFERENCES print_template(id),
  column_key   TEXT    NOT NULL,
  position     INTEGER NOT NULL,
  visible      INTEGER NOT NULL DEFAULT 1,
  width_mm     INTEGER NOT NULL DEFAULT 0,
  align        TEXT    NOT NULL DEFAULT 'LEFT',
  font_size_pt INTEGER NOT NULL DEFAULT 0,
  bold         INTEGER NOT NULL DEFAULT 0,
  CHECK (position > 0),
  CHECK (visible IN (0, 1)),
  CHECK (width_mm BETWEEN 0 AND 500),
  CHECK (align IN ('LEFT', 'CENTER', 'RIGHT')),
  CHECK (font_size_pt = 0 OR font_size_pt BETWEEN 6 AND 24),
  CHECK (bold IN (0, 1))
);
CREATE UNIQUE INDEX uk_print_template_column_key ON print_template_column(template_id, column_key);
CREATE UNIQUE INDEX uk_print_template_column_pos ON print_template_column(template_id, position);

INSERT INTO app_role_permission (role_id, permission) VALUES
  (2, 'print.template.read'), (2, 'print.template.write'),
  (3, 'print.template.read');
)SQL";

// v9：销售订单。订单复用 doc_header/doc_line，只增加订单状态与转出库映射，
// 不改旧表结构、不回填历史单据。
constexpr const char* kV9 = R"SQL(
ALTER TABLE doc_header ADD COLUMN order_status TEXT NOT NULL DEFAULT '';
ALTER TABLE doc_line ADD COLUMN converted_qty INTEGER NOT NULL DEFAULT 0;
ALTER TABLE doc_line ADD COLUMN source_order_line_id INTEGER NOT NULL DEFAULT 0;
CREATE INDEX idx_doc_line_source_order ON doc_line(source_order_line_id);
)SQL";

// v10：会话持久化。只新增表，不改既有表；表中只存令牌散列，不存明文令牌，
// 数据库文件泄露也不等于登录态泄露。csrf 需要回传给页面，按原样保存。
constexpr const char* kV10 = R"SQL(
CREATE TABLE app_session (
  token_hash   TEXT    PRIMARY KEY,
  user_id      INTEGER NOT NULL REFERENCES app_user(id),
  csrf         TEXT    NOT NULL,
  created_at   TEXT    NOT NULL,
  last_seen_at TEXT    NOT NULL,
  expires_at   TEXT    NOT NULL
);
CREATE INDEX idx_app_session_user ON app_session(user_id);
CREATE INDEX idx_app_session_expires ON app_session(expires_at);
)SQL";

// v11：数量改为 4 位定点整数（×10000），金额仍为分。
// 盘点实盘数可能为 0，原 doc_line 的 CHECK (qty > 0) 会挡住，因此重建该表放宽为 qty >= 0；
// 重建时保留 v9 的 converted_qty / source_order_line_id 与全部索引。
constexpr const char* kV11 = R"SQL(
ALTER TABLE unit ADD COLUMN qty_decimals INTEGER NOT NULL DEFAULT 0
  CHECK (qty_decimals BETWEEN 0 AND 4);

UPDATE unit SET qty_decimals = 2 WHERE code = 'MI';
UPDATE unit SET qty_decimals = 3 WHERE code = 'KG';

UPDATE doc_header    SET total_qty = total_qty * 10000;
UPDATE doc_line      SET qty = qty * 10000,
                         converted_qty = converted_qty * 10000;
UPDATE stock_balance SET qty = qty * 10000;
UPDATE stock_ledger  SET qty = qty * 10000;

CREATE TABLE doc_line_v11 (
  id                   INTEGER PRIMARY KEY,
  doc_id               INTEGER NOT NULL REFERENCES doc_header(id),
  line_no              INTEGER NOT NULL,
  item_id              INTEGER NOT NULL REFERENCES item(id),
  qty                  INTEGER NOT NULL,
  price_cents          INTEGER NOT NULL,
  amount_cents         INTEGER NOT NULL,
  converted_qty        INTEGER NOT NULL DEFAULT 0,
  source_order_line_id INTEGER NOT NULL DEFAULT 0,
  CHECK (qty >= 0),
  CHECK (price_cents >= 0),
  CHECK (amount_cents >= 0)
);
INSERT INTO doc_line_v11 (id, doc_id, line_no, item_id, qty, price_cents, amount_cents,
                          converted_qty, source_order_line_id)
SELECT id, doc_id, line_no, item_id, qty, price_cents, amount_cents,
       converted_qty, source_order_line_id
FROM doc_line;
DROP TABLE doc_line;
ALTER TABLE doc_line_v11 RENAME TO doc_line;
CREATE UNIQUE INDEX uk_doc_line_no ON doc_line(doc_id, line_no);
CREATE INDEX idx_doc_line_item ON doc_line(item_id);
CREATE INDEX idx_doc_line_source_order ON doc_line(source_order_line_id);
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
      {5, "v5_audit_log", kV5},
      {6, "v6_transfer_stocktake", kV6},
      {7, "v7_customer_finance", kV7},
      {8, "v8_print_template", kV8},
      {9, "v9_sales_order", kV9},
      {10, "v10_session", kV10},
      {11, "v11_decimal_qty", kV11},
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

int target_schema_version() {
  const auto& migrations = all_migrations();
  return migrations.empty() ? 0 : migrations.back().version;
}

}  // namespace xiaotu
