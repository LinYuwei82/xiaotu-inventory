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

// v12：资金账户与收付款单。纯增量：新增账户表、给 settlement 加列，
// 不修改任何历史金额、单号与核销分摊；老记录 account_id=0 表示「未指定账户」。
constexpr const char* kV12 = R"SQL(
CREATE TABLE fund_account (
  id            INTEGER PRIMARY KEY,
  code          TEXT    NOT NULL,
  name          TEXT    NOT NULL,
  kind          TEXT    NOT NULL,
  account_no    TEXT    NOT NULL DEFAULT '',
  bank_name     TEXT    NOT NULL DEFAULT '',
  opening_cents INTEGER NOT NULL DEFAULT 0,
  sort_order    INTEGER NOT NULL DEFAULT 0,
  status        INTEGER NOT NULL DEFAULT 1,
  is_deleted    INTEGER NOT NULL DEFAULT 0,
  created_at    TEXT    NOT NULL,
  created_by    TEXT    NOT NULL DEFAULT 'system',
  updated_at    TEXT    NOT NULL,
  updated_by    TEXT    NOT NULL DEFAULT 'system',
  CHECK (kind IN ('CASH', 'BANK', 'WECHAT', 'ALIPAY', 'OTHER')),
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_fund_account_code ON fund_account(code);
CREATE UNIQUE INDEX uk_fund_account_name ON fund_account(name);
CREATE INDEX idx_fund_account_sort ON fund_account(sort_order, id);

-- 已被收付款单引用的账户只能停用，不能删除，避免历史资金流水失去归属。
CREATE TRIGGER trg_fund_account_delete_guard
BEFORE UPDATE OF is_deleted ON fund_account
WHEN NEW.is_deleted = 1 AND OLD.is_deleted = 0
 AND EXISTS (SELECT 1 FROM settlement WHERE account_id = OLD.id)
BEGIN
  SELECT RAISE(ABORT, '账户已被收付款单引用，只能停用');
END;

ALTER TABLE settlement ADD COLUMN account_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE settlement ADD COLUMN method     TEXT    NOT NULL DEFAULT '';
ALTER TABLE settlement ADD COLUMN operator   TEXT    NOT NULL DEFAULT '';
CREATE INDEX idx_settlement_account_date ON settlement(account_id, settled_on, id);
)SQL";

// v13：成本调整单。仓库与调整原因下沉到明细行；
// stock_ledger 允许「数量 0、金额非 0」的调整流水，因此两张表都重建。
constexpr const char* kV13 = R"SQL(
CREATE TABLE doc_line_v13 (
  id                   INTEGER PRIMARY KEY,
  doc_id               INTEGER NOT NULL REFERENCES doc_header(id),
  line_no              INTEGER NOT NULL,
  item_id              INTEGER NOT NULL REFERENCES item(id),
  qty                  INTEGER NOT NULL,
  price_cents          INTEGER NOT NULL,
  amount_cents         INTEGER NOT NULL,
  converted_qty        INTEGER NOT NULL DEFAULT 0,
  source_order_line_id INTEGER NOT NULL DEFAULT 0,
  warehouse_id         INTEGER NOT NULL DEFAULT 0,
  adjust_cents         INTEGER NOT NULL DEFAULT 0,
  adjust_reason        TEXT    NOT NULL DEFAULT '',
  CHECK (qty >= 0),
  CHECK (price_cents >= 0),
  CHECK (amount_cents >= 0),
  CHECK (warehouse_id >= 0)
);
INSERT INTO doc_line_v13 (id, doc_id, line_no, item_id, qty, price_cents, amount_cents,
                          converted_qty, source_order_line_id, warehouse_id,
                          adjust_cents, adjust_reason)
SELECT id, doc_id, line_no, item_id, qty, price_cents, amount_cents,
       converted_qty, source_order_line_id, 0, 0, ''
FROM doc_line;
DROP TABLE doc_line;
ALTER TABLE doc_line_v13 RENAME TO doc_line;
CREATE UNIQUE INDEX uk_doc_line_no ON doc_line(doc_id, line_no);
CREATE INDEX idx_doc_line_item ON doc_line(item_id);
CREATE INDEX idx_doc_line_source_order ON doc_line(source_order_line_id);

CREATE TABLE stock_ledger_v13 (
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
  CHECK (amount_cents <> 0)
);
INSERT INTO stock_ledger_v13 (id, doc_id, doc_line_id, biz_type, biz_time, warehouse_id,
                              item_id, qty, amount_cents, created_at)
SELECT id, doc_id, doc_line_id, biz_type, biz_time, warehouse_id,
       item_id, qty, amount_cents, created_at
FROM stock_ledger;
DROP TABLE stock_ledger;
ALTER TABLE stock_ledger_v13 RENAME TO stock_ledger;
CREATE INDEX idx_stock_ledger_item_wh_time ON stock_ledger(item_id, warehouse_id, biz_time, id);
CREATE INDEX idx_stock_ledger_doc ON stock_ledger(doc_id);
CREATE UNIQUE INDEX uk_stock_ledger_doc_line ON stock_ledger(doc_line_id, biz_type);
)SQL";

// v14：结存数量/金额不得为负。只加触发器兜底，不重建表、不改历史数据；
// 历史负结存留给盘点单修正，迁移不做静默归零。
constexpr const char* kV14 = R"SQL(
CREATE TRIGGER trg_stock_balance_non_negative_insert
BEFORE INSERT ON stock_balance
WHEN NEW.qty < 0 OR NEW.amount_cents < 0
BEGIN
  SELECT RAISE(ABORT, '结存数量和金额不能为负');
END;

CREATE TRIGGER trg_stock_balance_non_negative_update
BEFORE UPDATE OF qty, amount_cents ON stock_balance
WHEN NEW.qty < 0 OR NEW.amount_cents < 0
BEGIN
  SELECT RAISE(ABORT, '结存数量和金额不能为负');
END;
)SQL";

// v15：盘点流水拆分盘盈/盘亏方向。历史行按 qty 符号回填标签（方向是事实，不是推断），
// 只改 biz_type，不动数量与金额；反审核回冲行按其 qty 反推原方向。
constexpr const char* kV15 = R"SQL(
UPDATE stock_ledger
SET biz_type = CASE WHEN qty > 0 THEN 'STK_CNT_IN' ELSE 'STK_CNT_OUT' END
WHERE biz_type = 'STK_CNT' AND qty <> 0;

UPDATE stock_ledger
SET biz_type = CASE WHEN qty < 0 THEN 'STK_CNT_IN_REVERSE' ELSE 'STK_CNT_OUT_REVERSE' END
WHERE biz_type = 'REVERSE' AND qty <> 0
  AND doc_id IN (SELECT id FROM doc_header WHERE doc_type = 'STK_CNT');
)SQL";

// v16：其他出入库的业务类型。历史单据不猜业务含义，统一归入「其他」，
// 保证报表按类型归集时既不漏统计也不出现空值；非其他出入库单据保持空串。
constexpr const char* kV16 = R"SQL(
ALTER TABLE doc_header ADD COLUMN biz_category TEXT NOT NULL DEFAULT '';
UPDATE doc_header SET biz_category = 'OTHER'
WHERE doc_type IN ('OTH_IN', 'OTH_OUT') AND biz_category = '';
)SQL";

// v17：商品分类、条码与税率。分类用 parent_id + path 冗余，按前缀匹配整棵子树，
// 不递归；层数由数据库放开、界面按产品约定卡三层。条码本版一商品一码，
// 同时建好 item_barcode 预留一商品多码的扩展位（暂不写入）。
constexpr const char* kV17 = R"SQL(
CREATE TABLE item_category (
  id         INTEGER PRIMARY KEY,
  parent_id  INTEGER NOT NULL DEFAULT 0,
  name       TEXT    NOT NULL,
  path       TEXT    NOT NULL,
  sort_order INTEGER NOT NULL DEFAULT 0,
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_item_category_sibling ON item_category(parent_id, name)
  WHERE is_deleted = 0;
CREATE INDEX idx_item_category_path ON item_category(path);

ALTER TABLE item ADD COLUMN category_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE item ADD COLUMN tax_rate_bp INTEGER NOT NULL DEFAULT 0;
ALTER TABLE item ADD COLUMN barcode TEXT NOT NULL DEFAULT '';
CREATE UNIQUE INDEX uk_item_barcode ON item(barcode) WHERE barcode <> '';

CREATE TABLE item_barcode (
  id         INTEGER PRIMARY KEY,
  item_id    INTEGER NOT NULL REFERENCES item(id),
  barcode    TEXT    NOT NULL,
  is_primary INTEGER NOT NULL DEFAULT 1,
  created_at TEXT    NOT NULL,
  CHECK (is_primary IN (0, 1))
);
CREATE UNIQUE INDEX uk_item_barcode_code ON item_barcode(barcode);
CREATE UNIQUE INDEX uk_item_barcode_item ON item_barcode(item_id);
)SQL";

// v18：职员档案与单据业务员。职员是业务档案，不强制绑定登录账号（user_id 预留），
// 与社区版「1 个账号、10 名职员」的额度口径一致。业务员落在单据表头，
// 销售订单 / 销售出库 / 销售退货适用，保存名称快照避免改名影响历史单据。
constexpr const char* kV18 = R"SQL(
CREATE TABLE employee (
  id         INTEGER PRIMARY KEY,
  code       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  phone      TEXT    NOT NULL DEFAULT '',
  department TEXT    NOT NULL DEFAULT '',
  position   TEXT    NOT NULL DEFAULT '',
  user_id    INTEGER NOT NULL DEFAULT 0,
  status     INTEGER NOT NULL DEFAULT 1,
  is_deleted INTEGER NOT NULL DEFAULT 0,
  created_at TEXT    NOT NULL,
  created_by TEXT    NOT NULL DEFAULT 'system',
  updated_at TEXT    NOT NULL,
  updated_by TEXT    NOT NULL DEFAULT 'system',
  CHECK (status IN (0, 1)),
  CHECK (is_deleted IN (0, 1))
);
CREATE UNIQUE INDEX uk_employee_code ON employee(code);

ALTER TABLE doc_header ADD COLUMN salesperson_id   INTEGER NOT NULL DEFAULT 0;
ALTER TABLE doc_header ADD COLUMN salesperson_name TEXT    NOT NULL DEFAULT '';
CREATE INDEX idx_doc_header_salesperson ON doc_header(salesperson_id, doc_date);

INSERT INTO app_role_permission (role_id, permission) VALUES
  (2, 'employee.read'), (2, 'employee.write'),
  (3, 'employee.read');
)SQL";

// v19：客户/供应商税号与客户信用额度。额度存整数分，0 表示不限制（最常见的新客户
// 场景），负数由服务层拒绝；是否硬拦由 config.toml 的 [sales] credit_limit_mode 决定。
constexpr const char* kV19 = R"SQL(
ALTER TABLE customer ADD COLUMN tax_no TEXT NOT NULL DEFAULT '';
ALTER TABLE customer ADD COLUMN credit_limit_cents INTEGER NOT NULL DEFAULT 0;
ALTER TABLE supplier ADD COLUMN tax_no TEXT NOT NULL DEFAULT '';
CREATE INDEX idx_customer_tax_no ON customer(tax_no) WHERE tax_no <> '';
CREATE INDEX idx_supplier_tax_no ON supplier(tax_no) WHERE tax_no <> '';
)SQL";

// v20：应收应付期初。期初直接写 ar_ap_entry 并打标记，doc_id 固定为 0，
// 因此把原来「一张单据一条台账」的唯一索引放宽为只约束真实单据。
constexpr const char* kV20 = R"SQL(
ALTER TABLE ar_ap_entry ADD COLUMN opening INTEGER NOT NULL DEFAULT 0;
DROP INDEX uk_ar_ap_doc;
CREATE UNIQUE INDEX uk_ar_ap_doc ON ar_ap_entry(doc_id) WHERE doc_id > 0;
CREATE INDEX idx_ar_ap_opening ON ar_ap_entry(opening) WHERE opening = 1;
INSERT INTO app_role_permission (role_id, permission) VALUES (2, 'opening.write');
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
      {12, "v12_fund_account", kV12},
      {13, "v13_cost_adjust", kV13},
      {14, "v14_stock_balance_non_negative", kV14},
      {15, "v15_stocktake_ledger_direction", kV15},
      {16, "v16_document_biz_category", kV16},
      {17, "v17_item_category_barcode_tax", kV17},
      {18, "v18_employee_salesperson", kV18},
      {19, "v19_partner_tax_no_credit_limit", kV19},
      {20, "v20_ar_ap_opening", kV20},
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

  // 只有全新数据库才预置「现金」账户；升级库保持账户为空，由用户按实际情况建档，
  // 历史 settlement 一律 account_id=0，不把旧数据强行挂到现金上。
  if (applied.empty()) {
    db.write_raw([&](sqlite3* conn) {
      exec_sql(conn,
               "INSERT OR IGNORE INTO fund_account"
               " (id, code, name, kind, account_no, bank_name, opening_cents,"
               "  sort_order, status, is_deleted, created_at, created_by,"
               "  updated_at, updated_by)"
               " VALUES (1, 'CASH', '现金', 'CASH', '', '', 0, 10, 1, 0,"
               "         datetime('now', 'localtime'), 'system',"
               "         datetime('now', 'localtime'), 'system');");
    });
  }
}

int target_schema_version() {
  const auto& migrations = all_migrations();
  return migrations.empty() ? 0 : migrations.back().version;
}

}  // namespace xiaotu
