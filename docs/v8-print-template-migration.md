# v8 打印模板迁移 SQL（v7 → v8）

> 历史归档：本文是 v7 → v8 的一次性迁移记录，当前 schema 已到 v39，命令与路径可能已过时，勿照做。

- 适用版本：数据库 schema **v7 → v8**
- 迁移名：`v8_print_template`
- 来源：[migrations.cpp](../core/src/migrations.cpp) 中的 `kV8` 常量，下面「完整语句」为原样摘录，可直接查看与手工执行。

## 这次迁移做什么

1. 新建 `print_template`：打印模板主表，保存纸张、方向、页边距、字号、页眉页脚、默认标记、`version` 乐观锁与软删除/审计字段。
2. 新建 `print_template_column`：模板的列配置子表，保存列顺序、可见性、列宽、对齐、字号与加粗。
3. 建立索引，其中 `uk_print_template_default` 是部分唯一索引，保证**同一目标最多一个默认模板**（软删除的模板不占用名额）。
4. 追加 `print.template.read` / `print.template.write` 权限行：操作员可读写模板，只读角色只能查看。

本迁移只做 `CREATE TABLE` / `CREATE INDEX` / 权限 `INSERT`，不修改 v7 及更早的任何表，也不回填数据。

## 执行前置条件

1. 当前库必须是 v7。执行前确认：

   ```sql
   PRAGMA user_version;                      -- 应为 7
   SELECT MAX(version) FROM schema_migration; -- 应为 7
   ```

2. `app_role` 中必须已有内置角色 `2 = operator`、`3 = viewer`（由 v2 迁移创建）。`admin`（角色 1）持有 `*` 通配权限，不需要额外授权行。
   如果曾手工删除过这两个角色，请先恢复，否则权限 `INSERT` 会因外键失败。
3. 权限行使用 `(role_id, permission)` 主键，重复插入会冲突；手工执行前可先查：

   ```sql
   SELECT role_id, permission FROM app_role_permission
   WHERE permission LIKE 'print.template.%';
   ```

4. 部分唯一索引需要 SQLite 3.8.0 及以上；本项目内嵌的 amalgamation 满足要求。

## 完整语句（原样）

```sql
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
```

## 逐段说明

### 1. `print_template`

| 字段 | 作用 |
| --- | --- |
| `code` / `name` | 模板编码与名称，编码唯一 |
| `target_type` / `target_key` | 绑定目标：`DOC`（六种单据）或 `REPORT`（七张报表），键由程序注册表校验 |
| `page_size` / `orientation` | `A4` / `A5` / `CUSTOM`，纵向或横向 |
| `paper_width_mm` / `paper_height_mm` | 仅 `CUSTOM` 使用，且必须大于 0 |
| `margin_mm` | 页边距，0–50mm |
| `font_size_pt` | 模板基础字号，6–24pt |
| `header_text` / `footer_text` | 页眉页脚纯文本，最多 500 字符；程序只做占位符字符串替换 |
| `show_page_no` | 是否显示页码 |
| `is_default` | 目标默认模板标记 |
| `status` | 启用 / 停用 |
| `version` | 乐观锁版本，更新时比对，成功后加一 |
| `is_deleted` | 软删除标记，删除后该目标回退到内置默认模板 |
| 审计字段 | `created_at/by`、`updated_at/by` |

表级 `CHECK` 把纸张、方向、页边距、字号、页眉页脚长度等约束放在数据库层，程序与手工执行都不会写出越界数据。

### 2. 主表索引

- `uk_print_template_code`：模板编码全局唯一。
- `idx_print_template_target`：按目标查询模板列表与默认模板。
- `uk_print_template_default`：**部分唯一索引**，只约束 `is_default = 1 AND is_deleted = 0` 的行，因此每个目标只能有一个生效中的默认模板；软删除或取消默认后名额自动释放。

### 3. `print_template_column`

列配置子表，`template_id` 指向模板。`position` 从 1 开始表示显示顺序；`visible` 控制是否打印；`width_mm = 0` 表示自动列宽；`align` 为 `LEFT` / `CENTER` / `RIGHT`；`font_size_pt = 0` 表示跟随模板字号，否则 6–24pt；`bold` 控制加粗。

模板只保存展示配置，不保存 SQL、脚本、HTML 或查询条件；可打印的列由程序内注册表（列白名单）决定。

### 4. 子表索引

- `uk_print_template_column_key`：同一模板内列不重复。
- `uk_print_template_column_pos`：同一模板内位置不重复，保证顺序稳定。

### 5. 权限行

| 角色 | 权限 |
| --- | --- |
| operator（2） | `print.template.read`、`print.template.write` |
| viewer（3） | `print.template.read` |

`admin` 使用 `*` 通配，无需新增行。模板管理权限只控制模板的增删改查；实际打印取数仍校验单据 / 库存 / 财务各自的数据权限。

## 能否重复执行

**不能。** 整段是一次性迁移：

- `CREATE TABLE` 在表已存在时会直接报错。
- 权限 `INSERT` 会撞 `(role_id, permission)` 主键。
- 两张索引表在重复执行时同样会报已存在。

本迁移也没有数据回填，重复执行没有任何收益。若只需补建某个索引，请单独执行对应 `CREATE INDEX`，不要整段重跑。

## 手工执行注意事项

优先让程序启动时自动迁移（会自行写 `schema_migration` 并更新 `user_version`）。确需手工执行时，按下面的顺序在**同一个事务**里完成：

```sql
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

-- 这里粘贴上面「完整语句」的全部内容（建表、索引、权限 INSERT）

INSERT INTO schema_migration (version, name, checksum, applied_at)
VALUES (8, 'v8_print_template', '0e23ff6e6a3a3c60', datetime('now', 'localtime'));

PRAGMA user_version = 8;
COMMIT;
```

要点：

1. **必须包在事务里**：`BEGIN IMMEDIATE` 之后任意语句失败，立即执行 `ROLLBACK`，避免留下半张表。
2. **`schema_migration` 不能漏**：程序启动时按这张表判断已执行的迁移。只建表不写记录，下次启动会再跑一遍并因「表已存在」失败。
3. **`checksum` 必须是 `0e23ff6e6a3a3c60`**：这是程序对 `kV8` 原文计算的 FNV-1a 校验值。写错会触发 `MIGRATION_CHECKSUM`，程序拒绝启动。
4. **`user_version` 必须置 8**：程序迁移会自动更新，手工执行容易漏。
5. **先备份**：执行前复制一份 `app.db`（连同 `-wal` / `-shm`，或先正常退出程序）。
6. 手工执行后可用下面的语句自查：

   ```sql
   PRAGMA user_version;                                        -- 8
   SELECT version, name, checksum FROM schema_migration WHERE version = 8;
   SELECT name FROM sqlite_master WHERE name LIKE 'print_template%';
   SELECT role_id, permission FROM app_role_permission WHERE permission LIKE 'print.template.%';
   ```
