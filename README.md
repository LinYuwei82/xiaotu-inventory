# 小兔进销存（M0 骨架）

macOS 优先的进销存软件骨架：**一套 C++20 代码，两种运行形态**。

- 不带参数启动 = **单机版**：内嵌服务只监听 `127.0.0.1` 的随机端口，生成一次性 token，用系统 WKWebView 打开本地页面。
- `--server` 启动 = **网络版**：监听 `0.0.0.0`，用浏览器访问（启动日志会输出带 token 的一次性访问链接）。

当前为 M0 骨架，已打通：**新建商品 → 录销售出库单 → 查看库存与流水 → 反审核回滚**。

## 1. 环境要求

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| macOS | 13+ | WKWebView 与 ATS 本地网络例外 |
| Xcode Command Line Tools | 最新 | AppleClang，支持 C++20 |
| CMake | 3.25+ | 使用 Presets |
| Ninja | 任意 | 构建提速 |
| ccache | 任意 | 编译缓存 |
| Python | 3.10+ | 仅代码生成器需要 |
| Jinja2 | 见 `tools/codegen/requirements.txt` | 仅代码生成器需要 |

可选：安装 vcpkg 并设置 `VCPKG_ROOT` 后，`nlohmann/json`、`cpp-httplib` 会优先使用 vcpkg 版本；未安装时自动回落到 `third_party/` 内嵌头文件，构建不依赖网络。

```bash
brew install cmake ninja ccache
```

## 2. 构建

```bash
# 发布态：前端内嵌进二进制
cmake --preset macos-release
cmake --build --preset release

# 开发态：前端由本地 dev server 提供，改 web/ 刷新即可
cmake --preset macos-dev
cmake --build --preset dev
```

产物：

```text
build/release/shell/xiaotu-inventory.app
build/dev/shell/xiaotu-inventory.app
```

macOS 产物是 `.app` bundle（图标、Info.plist、前端资源都在 bundle 内），可执行文件位于 `Contents/MacOS/xiaotu-inventory`。

运行测试：

```bash
ctest --test-dir build/release --output-on-failure
```

### 构建提速

- **Ninja**：两个 preset 均使用 Ninja。
- **ccache**：`CMAKE_C(XX)_COMPILER_LAUNCHER=ccache`，SQLite amalgamation 等大文件只编译一次。
- **vcpkg binary caching**：preset 已设置
  `VCPKG_BINARY_SOURCES=clear;files,<仓库>/.vcpkg-cache,readwrite`，
  依赖二进制缓存在仓库内 `.vcpkg-cache/`（已 gitignore），换构建目录不再重编依赖。
- **开发态不内嵌前端**：`-DXIAOTU_DEV_WEB=ON` 时跳过资源嵌入，缩短链接与启动前的编译时间。

## 3. 运行

### 单机版

```bash
open ./build/release/shell/xiaotu-inventory.app
```

程序会：定位数据目录 → 初始化/迁移数据库 → 绑定 `127.0.0.1:随机端口` → 生成一次性 token → 打开 WKWebView 加载 `http://127.0.0.1:<port>/#t=<token>`。关闭窗口即退出。

### 网络版

```bash
./build/release/shell/xiaotu-inventory.app/Contents/MacOS/xiaotu-inventory --server --port 8080
```

启动日志中的「访问凭据」链接即为一次性登录地址（10 分钟内有效、只能使用一次）。局域网其他机器访问时，把 URL 中的 `127.0.0.1` 换成本机 IP。

### 开发态（前端不重新编译）

```bash
# 终端 A：任意静态服务器提供 web/
python3 -m http.server 5173 --directory web

# 终端 B：开发态构建的骨架
open ./build/dev/shell/xiaotu-inventory.app
```

WebView 会打开 `http://127.0.0.1:5173/?api=http://127.0.0.1:<port>#t=<token>`，页面通过 `?api=` 找到后端；改 `web/` 下的文件后刷新窗口即可。也可用 `XIAOTU_DEV_URL` 环境变量指定其他地址。

### 数据位置

- macOS：`~/Library/Application Support/com.simem.xiaotu/app.db`
- `--data <目录>` 可覆盖（测试用）。

**仓库、数据库文件、构建产物都不要放在 iCloud Drive、OneDrive 等同步目录**：同步客户端会并发读写与整文件替换，SQLite 的锁在这些目录上不可靠，可能损坏数据库；构建产物被同步也会拖慢构建。推荐仓库放 `~/Developer/`。

## 4. 走通一次完整链路

界面操作：

1. 「商品」→ 新增：编码 `T001`，名称 `测试商品`，单位 `件`。
2. 「销售出库」→ 选择仓库（内置「主仓库」）→ 添加明细行：选商品、数量 `5`、单价 `12.34` → 保存并审核。
3. 「库存」→ 看到该商品数量 `-5`、流水一条 `SAL_OUT`；点「反审核」后数量回到 `0`，流水追加一条 `REVERSE`（原流水不被删除）。

等价的命令行验证（网络版）：

```bash
./build/release/shell/xiaotu-inventory.app/Contents/MacOS/xiaotu-inventory --server --port 18080 --data /tmp/xt-data > /tmp/xt.log 2>&1 &
TOKEN=$(grep -o '#t=[0-9a-f]*' /tmp/xt.log | head -1 | cut -d= -f2)
curl -s -c /tmp/xt.cookies -H 'Content-Type: application/json' \
  -d "{\"token\":\"$TOKEN\"}" http://127.0.0.1:18080/api/session
curl -s -b /tmp/xt.cookies -H 'Content-Type: application/json' \
  -d '{"code":"T001","name":"测试商品"}' http://127.0.0.1:18080/api/items
curl -s -b /tmp/xt.cookies -H 'Content-Type: application/json' \
  -d '{"doc_type":"SAL_OUT","warehouse_id":1,"doc_date":"2026-11-12",
       "lines":[{"item_id":<上一步返回的 id>,"qty":5,"price_cents":1234}]}' \
  http://127.0.0.1:18080/api/documents
curl -s -b /tmp/xt.cookies http://127.0.0.1:18080/api/stock/balances
```

## 5. 目录结构

```text
xiaotu-inventory/
├── CMakeLists.txt / CMakePresets.json / vcpkg.json
├── cmake/
│   ├── embed_resources.cmake      # 前端产物 → C++ 资源
│   └── generated_sources.cmake    # 生成器输出的源文件清单（自动生成）
├── core/                          # 纯业务，无平台代码
│   ├── include/xiaotu/
│   │   ├── db.h                   # 单写连接 + 读连接池 + 事务
│   │   ├── document.h             # 单据定义与通用引擎
│   │   ├── doc_number.h           # 单据号生成器
│   │   ├── crud.h                 # 软删除、审计字段、唯一性校验
│   │   ├── context.h              # 请求上下文（操作人）
│   │   ├── gen/                   # ← 生成物（DAO / Service）
│   │   └── ...
│   └── src/                       # 手写实现 + src/gen/（生成物）
├── server/                        # HTTP 层
│   ├── include/xiaotu/
│   │   ├── services.h             # 服务组合根
│   │   ├── http_guard.h           # guard("权限码", handler)
│   │   ├── http_json.h / session_store.h / permissions.h
│   │   └── gen/                   # ← 生成物（路由声明）
│   └── src/                       # 手写路由 + src/gen/（生成物）
├── shell/                         # 唯一允许出现平台代码的地方
│   ├── main.cpp                   # 参数、模式、启动顺序
│   ├── common/platform_paths.*     # 数据目录（应用标识 com.simem.xiaotu）
│   ├── mac/icon/                  # 图标源 SVG（候选）与 xiaotu.icns
│   └── mac/webview.mm             # WKWebView 宿主
├── web/                           # 前端（无构建步骤）
│   ├── index.html / app.js / styles.css
│   └── gen/                       # ← 生成物（列表页/表单页 + pages.json）
├── tests/core_smoke.cpp
└── tools/codegen/                  # 代码生成器（Python + Jinja2）
    ├── generate.py
    ├── schema/masterdata.json
    └── templates/*.j2
```

## 6. 代码生成器

### 6.1 生成什么

`tools/codegen/schema/masterdata.json` 描述表结构（字段、类型、必填、唯一、列表/表单展示、API 路径、权限码），生成：

| 产物 | 位置 |
| --- | --- |
| DAO（行结构体 + list/find/insert/update/soft_delete） | `core/include/xiaotu/gen/*_dao.h`、`core/src/gen/*_dao.cpp` |
| 服务层骨架（必填/唯一校验 + 事务） | `core/include/xiaotu/gen/*_service.h`、`core/src/gen/*_service.cpp` |
| HTTP 路由（带权限码） | `server/include/xiaotu/gen/*_routes.h`、`server/src/gen/*_routes.cpp` |
| 前端列表页/表单页 + 页面清单 | `web/gen/*_page.js`、`web/gen/pages.json` |
| CMake 源文件清单 | `cmake/generated_sources.cmake` |

所有生成文件首行带固定标记 `AUTO-GENERATED by tools/codegen ... DO NOT EDIT.`。

### 6.2 生成 / 校验

```bash
python3 -m venv tools/codegen/.venv
tools/codegen/.venv/bin/pip install -r tools/codegen/requirements.txt

# 生成（可重复执行；内容不变则不写盘）
tools/codegen/.venv/bin/python tools/codegen/generate.py --schema masterdata

# 只检查 schema 与生成物是否漂移（不写盘）
tools/codegen/.venv/bin/python tools/codegen/generate.py --schema masterdata --check
```

### 6.3 手写与生成的边界

| 目录 | 归属 | 规则 |
| --- | --- | --- |
| `core/*/gen/`、`server/*/gen/`、`web/gen/`、`cmake/generated_sources.cmake` | 生成 | 禁止手改，重新生成会被覆盖 |
| `core/src/`、`core/include/xiaotu/`（不含 `gen/`）、`server/src/`、`server/include/xiaotu/`（不含 `gen/`）、`web/*.*` | 手写 | 业务扩展写这里 |
| `tools/codegen/schema/*.json` | 数据源 | 改字段先改 schema，再生成 |
| `tools/codegen/templates/*.j2` | 模板 | 改生成规则时修改，并重新生成全部表 |

需要扩展某张表的业务规则时，**组合**生成的服务（或在生成服务外层写扩展类），不要改生成文件；若规则是通用的，改模板让所有表受益。

### 6.4 新增一张表的完整流程

以新增「客户」表为例：

1. **改 schema**：在 `tools/codegen/schema/masterdata.json` 的 `tables` 里追加：

   ```json
   {
     "table": "customer", "key": "customer", "entity": "客户",
     "api": "/api/customers",
     "permissions": { "read": "customer.read", "write": "customer.write" },
     "order_by": "code",
     "fields": [
       { "name": "code", "label": "编码", "type": "string", "required": true, "unique": true, "in_list": true, "in_form": true },
       { "name": "name", "label": "名称", "type": "string", "required": true, "unique": false, "in_list": true, "in_form": true }
     ]
   }
   ```

2. **重新生成**：`tools/codegen/.venv/bin/python tools/codegen/generate.py --schema masterdata`。DAO、服务、路由、前端页面、CMake 清单一次到位，`web/gen/pages.json` 自动追加该页，前端导航无需手改。
3. **加迁移**：在 `core/src/migrations.cpp` 新增 `v2` 脚本（`all_migrations()` 里追加），建表必须包含生成器依赖的列：
   `id INTEGER PRIMARY KEY`、业务字段、`is_deleted INTEGER NOT NULL DEFAULT 0`、
   `created_at/created_by/updated_at/updated_by TEXT NOT NULL`，唯一列建唯一索引。
   **已发布的迁移脚本不允许修改**（启动时会校验 checksum）。
4. **加权限码**：在 `server/src/permissions.cpp` 的角色表里，把 `customer.read` / `customer.write` 加给相应角色（未声明的权限码默认拒绝）。
5. **重新构建**：`cmake --build --preset release`（`CONFIGURE_DEPENDS` 会自动重新配置）。
6. **验证**：界面出现「客户」菜单，或
   `curl -b cookies http://127.0.0.1:<port>/api/customers`。

## 7. 单据同构引擎

`core/include/xiaotu/document.h` 用「单据定义」描述一类单据，通用流程只实现一次：

```cpp
struct DocumentDefinition {
  std::string doc_type;           // SAL_OUT
  std::string doc_no_prefix;      // XSCK
  StockDirection stock_direction; // In / Out / None
  bool affects_stock;             // 是否动库存
  PriceSource price_source;       // Manual（M0）
  CostHook cost_hook;             // 成本钩子：qty、price、结存数量/金额 → 结转金额
  std::function<void(sqlite3*, int64_t)> on_approve;  // 审核附加动作（往来账等）
  std::function<void(sqlite3*, int64_t)> on_reverse;  // 反审核附加动作
};
```

`DocumentEngine::save_and_approve` 在一个写事务内完成：校验 → 取单据号 → 写主子表 → 按定义生成库存流水与余额 → 执行 `on_approve`。`reverse` 反向写流水、回滚余额、置为 `VOIDED`，**不删除原流水**。

已内置定义：`SAL_OUT`（销售出库，移动加权平均成本）、`PUR_IN`（采购入库，按单据金额入账）。新增采购入库只需补一条前端入口，无需写流程代码；新增退货/调拨/盘点同理。

当前库存策略：M0 允许负库存（无期初入库也能走通链路），负库存策略待产品确认后由定义/参数控制。

## 8. 横切逻辑

| 机制 | 位置 | 用法 |
| --- | --- | --- |
| 权限码 | `server/src/permissions.cpp` + `http_guard.h` | 路由写成 `guard("item.read", handler)`，中间件统一校验；未声明即拒绝 |
| 审计字段 | `core/src/context.cpp` + `crud.h` | `audit_stamp()` 自动取当前用户与时间，生成代码写入 created/updated 字段 |
| 单据号 | `core/src/doc_number.cpp` | `allocate_doc_no()` 统一格式与取号，写事务内完成，允许跳号不允许重号 |
| 软删除 | `core/src/crud.cpp` | `soft_delete()` 置 `is_deleted=1` 并刷新修改人；查询统一过滤 |
| 请求上下文 | `core/include/xiaotu/context.h` | 中间件按请求填充 `current_context()`（用户、角色、request id） |

## 9. M0 已知限制（对应方案待确认项）

- 无用户/角色表，仅一次性 token 换取内置 `local-admin` 会话；网络版正式登录属于 M5。
- 单据保存即审核，没有草稿态；反审核为整单反向。
- 数量按整数件（精度策略见方案 Q4），无多单位、批次、序列号、价格体系。
- 成本按移动加权平均，暂不支持其他计价方式。
- 无打印、无 Excel 导出、无报表。
- 无请求幂等键（`request_id`）与 CSRF token；当前依赖一次性 token + HttpOnly Cookie + `SameSite=Lax`。
- 未接入 vcpkg 时依赖 `third_party/` 内嵌头文件；接入 vcpkg 后 `vcpkg.json` 会自动生效。

## 10. 参考

- 技术方案：`docs/进销存技术方案.md`
- 数据库结构：`core/src/migrations.cpp`（v1）
- API：`/api/health`、`/api/session`、`/api/items`、`/api/warehouses`、`/api/documents`、`/api/documents/{id}/reverse`、`/api/stock/balances`、`/api/stock/ledger`
