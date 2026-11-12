# 小兔进销存（M2）

> 主仓库在 Gitee：`git@gitee.com:lin-yuwei168/xiaotu-inventory.git`。GitHub 的 `LinYuwei82/xiaotu-inventory` 是同源镜像，只是快照：不接收 PR，也不跑 CI。
>
> 开发、提交与发版都以 Gitee 为准；自动门禁在 Gitee Go，仓库里没有 `.github/workflows/`。

macOS 优先的进销存软件：**一套 C++20 代码，两种运行形态**。

- 不带参数启动 = **单机版**：内嵌服务只监听 `127.0.0.1` 的随机端口，生成一次性 token，用系统 WKWebView 打开本地页面。
- `--server` 启动 = **网络版**：监听 `0.0.0.0`，用浏览器访问；使用用户名与口令登录（首次需先给内置管理员设置口令）。

当前 M2 收口中，已打通：**商品 / 单位 / 仓库 / 客户 / 供应商档案（支持 Excel 批量导入导出）→ 采购入库、销售出库、采购退货、销售退货、其他入库、其他出库、调拨、盘点、成本调整（草稿与反审核）→ 销售订单确认与转出库 → 库存余额与流水 → 应收应付台账与核销 → 收付款单与资金账户 → 往来对账单 → 八类报表导出与打印 → 自定义打印模板**，并具备内置 `admin` 用户、三角色权限、审计日志与 macOS `.dmg` 打包。成本重算保持只读核对，差异通过成本调整单落账。

## 0. 数据库结构版本

当前 schema 为 **v13**（启动时自动迁移，已发布迁移不允许修改，校验 checksum）。

| 版本 | 内容 |
| --- | --- |
| v1 | 初始表：商品、仓库、单据主子表、库存流水与余额 |
| v2 | 用户与角色、角色权限表，内置 admin / operator / viewer |
| v3 | 供应商档案 |
| v4 | 计量单位 |
| v5 | 审计日志 |
| v6 | 库存调拨与盘点单据 |
| v7 | 客户档案、应收应付台账与核销，历史销售单回填 `customer_id` |
| v8 | 自定义打印模板主表与列配置子表，新增 `print.template.*` 权限 |
| v9 | 销售订单：`order_status`、`converted_qty`、`source_order_line_id` 与索引 |
| v10 | 会话表 `app_session`：令牌散列、用户、有效期与索引 |
| v11 | 数量改为 4 位定点整数（×10000），`unit.qty_decimals` 控制单位精度，重建 `doc_line` 允许盘点实盘为 0 |
| v12 | 资金账户 `fund_account`；`settlement` 增加账户、结算方式与经办人，老记录 `account_id=0` 表示未指定 |
| v13 | 成本调整单：重建 `doc_line`（明细行仓库、调整金额、调整原因，允许 `qty=0`）与 `stock_ledger`（金额非零），新增 `cost.adjust` 权限码（仅管理员） |

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

Windows 只构建服务端与测试时另需 CMake 3.25+、Ninja、Visual Studio 2022 Build Tools（MSVC，C++20）；
不需要 Xcode、ccache 或 vcpkg，`cpp-httplib`、`nlohmann/json`、SQLite、Argon2 均已内嵌在 `third_party/`。

## 2. 构建

```bash
# 发布态：前端内嵌进二进制
cmake --preset macos-release
cmake --build --preset release

# 社区版（额度与能力见 docs/EDITIONS.md；不带此参数即全功能版）
cmake --preset macos-release -DXIAOTU_EDITION=community
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
另有一个不依赖 WebView 的命令行产物 `build/release/server/xiaotu-server`，用于网络版与浏览器开发。

构建目标有三个业务目标：`xiaotu_core`（纯业务）、`xiaotu_server`（HTTP 服务与内嵌静态资源）、`xiaotu-inventory`（macOS WKWebView 壳），另有三个测试目标 `core_smoke`、`config_smoke`、`session_smoke`。
Preset 对应关系：`macos-release` / `macos-dev`（构建名 `release` / `dev`）用于 macOS 全量构建；
`windows-release` / `windows-dev` 用于 Windows，二者已设置 `XIAOTU_BUILD_SHELL=OFF`。
依赖全部内嵌在 `third_party/`（SQLite amalgamation、cpp-httplib、nlohmann/json、phc-winner-argon2 参考实现，均为 CC0 / Apache-2.0 / MIT 许可），构建不依赖网络；安装 vcpkg 后自动优先使用 vcpkg 版本。

单机版壳默认构建。只做服务端 / 业务逻辑开发时，可在任意平台关闭：

```bash
cmake --preset macos-release -DXIAOTU_BUILD_SHELL=OFF
cmake --build --preset release
```

关闭后不配置、不构建 `shell/` 目标，只保留 `core`、`server` 与测试；服务端入口为
`build/release/server/xiaotu-server`。Windows 上请使用下面的 `windows-*` preset 或同样的
`-DXIAOTU_BUILD_SHELL=OFF`。

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

### Windows：只构建服务端与测试

适用于在 Windows 上开发业务逻辑、跑测试、用浏览器访问页面，不需要 WKWebView 与 `.app` 打包。

```powershell
git clone git@gitee.com:lin-yuwei168/xiaotu-inventory.git
cd xiaotu-inventory
git switch -c <机器名>/<主题>   # 例如 macmini/报表 或 macbook/打印模板

cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

产物为 `build\windows\server\xiaotu-server.exe`。启动网络版：

```powershell
.\build\windows\server\xiaotu-server.exe --server --port 8080 --data D:\xt-data
```

首次先设置管理员口令，再启动并用浏览器访问 `http://localhost:8080/` 登录：

```powershell
$env:XIAOTU_PASSWORD = '你的口令'
.\build\windows\server\xiaotu-server.exe --data D:\xt-data --set-password admin
```

前端开发态用 `windows-dev`：

```powershell
cmake --preset windows-dev
cmake --build --preset windows-dev
python -m http.server 5173 --directory web   # 终端 A
.\build\windows-dev\server\xiaotu-server.exe  # 终端 B，无参数 = 浏览器单机模式
```

终端 B 会打印 `http://127.0.0.1:5173/?api=...#t=...`，直接打开即可；改 `web/` 后刷新页面。
`windows-*` preset 已设置 `XIAOTU_BUILD_SHELL=OFF`；不用 preset 时手动加
`-DXIAOTU_BUILD_SHELL=OFF` 即可。

## 3. 运行

### 单机版

```bash
open ./build/release/shell/xiaotu-inventory.app
```

程序会：定位数据目录 → 初始化/迁移数据库 → 绑定 `127.0.0.1:随机端口` → 生成一次性 token → 打开 WKWebView 加载 `http://127.0.0.1:<port>/#t=<token>`。关闭窗口即退出。

### 网络版

首次启动前先给内置管理员设置口令（口令长度 8–128，经 Argon2id 散列存储）：

```bash
XIAOTU_PASSWORD='你的口令' ./build/release/server/xiaotu-server \
  --data /tmp/xt-data --set-password admin
```

然后启动服务：

```bash
./build/release/server/xiaotu-server --server --port 8080 --data /tmp/xt-data
```

端口、监听地址、数据目录、日志级别/目录也可以写在 `config.toml` 里，避免每次拼参数：

```toml
[server]
listen = "0.0.0.0"
port = 8080

[storage]
data_dir = "/var/lib/xiaotu"

[log]
level = "info"
dir = "/var/log/xiaotu"
```

默认读取 `<数据目录>/config.toml`，也可用 `--config` 或 `XIAOTU_CONFIG` 指定。优先级：**命令行 > 环境变量 > config.toml > 内置默认**。配置文件只支持 `[server]`、`[storage]`、`[log]` 三个段的上述键，写错段名/键名会带行号报错。完整说明、systemd 单元与运维步骤见 [docs/部署与配置.md](docs/部署与配置.md)，示例见 `deploy/config.toml.example`、`deploy/systemd/xiaotu.service`。

浏览器访问 `http://<本机 IP>:8080/`，用 `admin` 和新口令登录。局域网其他机器访问时把 IP 换成本机地址；若前面有 TLS 反向代理，带上 `X-Forwarded-Proto: https`，会话 Cookie 会自动加 `Secure`。

并发能力有可复现的压测脚本与结论：`tools/loadtest/mixed_load.py` 模拟 10–30 个用户混合读写，核对单据号与库存一致性，方法与实测数据见 [docs/并发压测.md](docs/并发压测.md)。

登录安全现状：

- 会话保存在数据库里，重启服务后仍然有效。有效期 8 小时，每次访问滑动续期（同一会话最多每分钟写一次库）；登出、改密、用户停用或删除都会让对应会话立即失效。数据库里只存令牌的散列，不存明文令牌。改数据目录或恢复另一个备份等于换了一个库，旧会话不再存在，需要重新登录。
- 登录失败按「来源 IP + 用户名」限流：5 分钟内连续 5 次失败后返回 `429`，成功登录即清零。
- 会话 Cookie 为 `HttpOnly`、`SameSite=Strict`；写操作必须带登录响应里的 `csrf`，放在 `X-CSRF-Token` 请求头，否则返回 `403 CSRF_INVALID`。
- 网络版禁用了启动 token 登录，只能用用户名与口令；单机版仍走一次性 token 自动登录。

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

### 备份与恢复

- 迁移前自动备份：启动时若数据库版本低于代码内置版本，先用 `VACUUM INTO` 写 `<数据目录>/backups/app-<时间>-pre-v<旧版本>.db`；备份失败则中止迁移，不带着未备份的数据升级。
- 手动备份：`<程序> --data <数据目录> --backup`，输出快照路径与字节数。
- 恢复：`<程序> --data <数据目录> --restore <备份文件>`；先做 `PRAGMA integrity_check`，目标已存在时留一份 `pre-restore-*.db`，再覆盖并清理 `-wal` / `-shm`。恢复必须在服务停止时执行。
- 快照是一致性单文件，可直接复制到另一台机器后恢复。

## 4. 走通一次完整链路

界面操作（左侧按「采购 / 销售 / 库存 / 财务 / 报表 / 基础资料 / 系统」分组，同一时间展开一组；报表是独立入口）：

1. 「基础资料 → 商品」→ 新增：编码 `T001`，名称 `测试商品`，单位 `件`。
2. 「采购入库」→ 选择仓库（内置「主仓库」）→ 添加明细行：选商品、数量 `5`、单价 `10.00` → 保存并审核，库存变为 `5`，流水一条 `PUR_IN`。
3. 「销售出库」→ 添加明细行：数量 `2`、单价 `12.34` → 保存并审核，库存变为 `3`，流水追加 `SAL_OUT`，应收台账同步生成。
4. 「库存」看到数量 `3`；对该销售出库点「反审核」后数量回到 `5`，流水追加一条 `REVERSE`（原流水不被删除）。

等价的命令行验证（网络版）：

```bash
XIAOTU_PASSWORD='AdminPassw0rd!' \
  ./build/release/server/xiaotu-server --data /tmp/xt-data --set-password admin
./build/release/server/xiaotu-server --server --port 18080 --data /tmp/xt-data > /tmp/xt.log 2>&1 &
curl -s -c /tmp/xt.cookies -o /tmp/login.json -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"AdminPassw0rd!"}' http://127.0.0.1:18080/api/login
CSRF=$(python3 -c 'import json;print(json.load(open("/tmp/login.json"))["csrf"])')
curl -s -b /tmp/xt.cookies -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -d '{"code":"T001","name":"测试商品"}' http://127.0.0.1:18080/api/items
curl -s -b /tmp/xt.cookies -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -d '{"doc_type":"PUR_IN","warehouse_id":1,"doc_date":"2026-11-12",
       "lines":[{"item_id":<上一步返回的 id>,"qty":"5","price_cents":1000}]}' \
  http://127.0.0.1:18080/api/documents
curl -s -b /tmp/xt.cookies -H 'Content-Type: application/json' \
  -d '{"doc_type":"SAL_OUT","warehouse_id":1,"doc_date":"2026-11-12",
       "lines":[{"item_id":<商品 id>,"qty":"2","price_cents":1234}]}' \
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
│   │   ├── print_template.h       # 打印目标注册表、内置默认模板与模板服务
│   │   ├── doc_number.h           # 单据号生成器
│   │   ├── crud.h                 # 软删除、审计字段、唯一性校验
│   │   ├── context.h              # 请求上下文（操作人）
│   │   ├── permissions.h          # 角色权限（启动时从 app_role_permission 载入）
│   │   ├── edition.h              # 版本能力与额度判定
│   │   ├── user_service.h         # 用户 CRUD
│   │   ├── gen/                   # ← 生成物（DAO / Service / 版本能力表）
│   │   └── ...
│   └── src/                       # 手写实现 + src/gen/（生成物）
├── server/                        # HTTP 层
│   ├── include/xiaotu/
│   │   ├── services.h             # 服务组合根
│   │   ├── http_guard.h           # guard("权限码", handler)
│   │   ├── platform_paths.h       # 数据目录（应用标识 com.simem.xiaotu）
│   │   ├── http_json.h / session_store.h
│   │   └── gen/                   # ← 生成物（路由声明）
│   └── src/                       # 手写路由（含 print_routes.cpp）+ cli_main.cpp + src/gen/（生成物）
├── shell/                         # 唯一允许出现平台代码的地方（可关闭）
│   ├── mac/icon/                  # 图标源 SVG（候选）与 xiaotu.icns
│   └── mac/webview.mm             # WKWebView 宿主
├── web/                           # 前端（无构建步骤）
│   ├── index.html / app.js / styles.css
│   └── gen/                       # ← 生成物（列表页/表单页 + pages.json）
├── tests/                         # core_smoke / config_smoke / session_smoke
├── deploy/                        # config.toml 示例与 systemd 单元
├── docs/                          # 用户手册、技术方案、部署与配置、并发压测等
└── tools/
    ├── codegen/                   # 代码生成器（Python + Jinja2）
    │   ├── generate.py
    │   ├── schema/masterdata.json # 主数据表结构
    │   ├── schema/editions.json   # 版本能力与额度（社区版 / 全功能版）
    │   └── templates/*.j2
    ├── ci/                        # check.sh 与平台代码泄漏检查
    ├── loadtest/mixed_load.py     # 并发压测脚本
    └── package/                   # macOS .dmg 打包
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

`tools/codegen/schema/editions.json` 描述社区版与全功能版的能力状态与额度，生成 `core/include/xiaotu/gen/edition_caps.h`（由 `-DXIAOTU_EDITION` 选择生效版本）。额度数值的机器可读来源是 `tools/codegen/schema/editions.json`，同一组数字还要同步写在 `docs/EDITIONS.md` 第 5 节：改额度时两处必须一起改，不一致视为文档缺陷。

所有生成文件首行带固定标记 `AUTO-GENERATED by tools/codegen ... DO NOT EDIT.`。

### 6.2 生成 / 校验

```bash
python3 -m venv tools/codegen/.venv
tools/codegen/.venv/bin/pip install -r tools/codegen/requirements.txt

# 生成（可重复执行；内容不变则不写盘）
tools/codegen/.venv/bin/python tools/codegen/generate.py

# 只检查 schema 与生成物是否漂移（不写盘）
tools/codegen/.venv/bin/python tools/codegen/generate.py --check
```

`--schema masterdata` / `--schema editions` 可只处理单个定义；不带参数等价于 `--schema all`。

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

2. **重新生成**：`tools/codegen/.venv/bin/python tools/codegen/generate.py`。DAO、服务、路由、前端页面、CMake 清单一次到位，`web/gen/pages.json` 自动追加该页（含 `permission` 与 `nav_group`），前端导航按 `nav_group` 自动归组，无需手改。
3. **加迁移**：在 `core/src/migrations.cpp` 新增一版脚本（`all_migrations()` 里追加，v2 起已有用户与角色表），建表必须包含生成器依赖的列：
   `id INTEGER PRIMARY KEY`、业务字段、`is_deleted INTEGER NOT NULL DEFAULT 0`、
   `created_at/created_by/updated_at/updated_by TEXT NOT NULL`，唯一列建唯一索引。
   **已发布的迁移脚本不允许修改**（启动时会校验 checksum）。
4. **加权限码**：在迁移脚本的 `app_role_permission` 种子里把 `customer.read` / `customer.write` 授予相应角色（未声明的权限码默认拒绝）。
5. **重新构建**：`cmake --build --preset release`（`CONFIGURE_DEPENDS` 会自动重新配置）。
6. **验证**：界面「基础资料」组下出现「客户」菜单，或
   `curl -b cookies http://127.0.0.1:<port>/api/customers`。

## 7. 单据同构引擎

`core/include/xiaotu/document.h` 用「单据定义」描述一类单据，通用流程只实现一次：

```cpp
struct DocumentDefinition {
  std::string doc_type;           // SAL_OUT
  std::string doc_no_prefix;      // XSCK
  StockMode stock_mode;           // None / In / Out / Transfer / Stocktake
  bool affects_stock;             // 是否动库存
  DocumentRole role;              // Transaction / Order（订单只确认与转出库）
  PartnerKind partner_kind;       // None / Supplier / Customer
  int finance_direction;          // 1 应收，-1 应付，0 不生成往来
  bool finance_red;               // 退货红字
  PriceSource price_source;       // Manual
  CostHook cost_hook;             // 成本钩子：qty、price、结存数量/金额 → 结转金额
  std::function<void(sqlite3*, int64_t)> on_approve;  // 审核附加动作
  std::function<void(sqlite3*, int64_t)> on_reverse;  // 反审核附加动作
};
```

`DocumentEngine::save_and_approve` 在一个写事务内完成：校验 → 取单据号 → 写主子表 → 按定义生成库存流水与余额 → 执行 `on_approve`。`reverse` 反向写流水、回滚余额、置为 `VOIDED`，**不删除原流水**。库存与往来都按定义条件执行，`affects_stock=false` / `finance_direction=0` 的单据天然不产生副作用。

已内置九种单据：

| 单据 | 类型 | 影响库存 | 产生往来 |
| --- | --- | --- | --- |
| 采购入库 | `PUR_IN` | 入库 | 应付（蓝字） |
| 采购退货 | `PUR_RET` | 出库 | 应付（红字） |
| 销售出库 | `SAL_OUT` | 出库 | 应收（蓝字） |
| 销售退货 | `SAL_RET` | 入库 | 应收（红字） |
| 其他入库 | `OTH_IN` | 入库（手工单价） | 否 |
| 其他出库 | `OTH_OUT` | 出库（移动加权平均） | 否 |
| 库存调拨 | `STK_TRF` | 源仓出、目标仓入 | 否 |
| 库存盘点 | `STK_CNT` | 按实盘差额调整 | 否 |
| 销售订单 | `SAL_ORDER` | 否（确认不占库存） | 否（转出库时才由 `SAL_OUT` 产生应收） |

其他入库按录入单价入账，其他出库按当前移动加权平均结转并校验可用库存；两者都不绑定往来单位、不产生应收应付。

并发验收：库存 10 件时，8 张各 3 件的销售出库草稿同时审核，自动化用例断言恰好 3 张成功、5 张 `STOCK_INSUFFICIENT`，结存为 1、流水与应收只随成功单据增长；另有并发反审核用例，同一张已审核出库单被 8 个线程同时反审核，断言只有 1 次成功、库存与流水只回滚一次。两者均由 `Db` 的单写连接与 `BEGIN IMMEDIATE` 事务保证，不依赖调用方加锁。

销售订单状态：`DRAFT` 草稿 → `APPROVED + CONFIRMED` 已确认 → `APPROVED + CONVERTED` 已转出库 / `APPROVED + CLOSED` 已关闭。转出库在单事务内校验「已转 + 本次 ≤ 订单数量」，超量拒绝（`ORDER_QTY_EXCEEDED`），并生成正式销售出库单；反审核出库单会释放对应订单数量。

当前库存策略：出库类单据（销售出库、采购退货、销售订单转出库）审核 / 转出前校验可用库存，不足则整单拒绝（`STOCK_INSUFFICIENT`）并回滚事务，不产生负数结存或半途写入。

## 8. 横切逻辑

| 机制 | 位置 | 用法 |
| --- | --- | --- |
| 权限码 | `core/src/permissions.cpp` + `http_guard.h` | 路由写成 `guard("item.read", handler)`；角色权限在启动时从 `app_role_permission` 载入，未声明即拒绝 |
| 版本能力 | `core/src/edition.cpp` + `tools/codegen/schema/editions.json` | 服务层调用 `EditionPolicy::require/check_quota`，路由与界面不出现版本字符串 |
| 审计字段 | `core/src/context.cpp` + `crud.h` | `audit_stamp()` 自动取当前用户与时间，生成代码写入 created/updated 字段 |
| 单据号 | `core/src/doc_number.cpp` | `allocate_doc_no()` 统一格式与取号，写事务内完成，允许跳号不允许重号 |
| 软删除 | `core/src/crud.cpp` | `soft_delete()` 置 `is_deleted=1` 并刷新修改人；查询统一过滤 |
| 请求上下文 | `core/include/xiaotu/context.h` | 中间件按请求填充 `current_context()`（用户、角色、request id） |

### 角色与权限点

| 角色 | 权限 |
| --- | --- |
| admin（1） | `*` 通配 |
| operator（2） | 主数据读写（`item/unit/warehouse/supplier/customer` 的 `.read` + `.write`）、`document.read/write`、`stock.read`、`finance.read/write`、`print.template.read/write` |
| viewer（3） | 上述所有 `.read`（含 `document.read`、`stock.read`、`finance.read`、`print.template.read`），无任何写权限 |

权限点按十一组登记：`item`、`unit`、`warehouse`、`supplier`、`customer`、`document`、`stock`、`finance`、`print.template`、`user`、`audit`。其中 `user.read/write` 与 `audit.read` 没有 `app_role_permission` 种子行，实际只有 admin 的 `*` 通配覆盖，operator 与 viewer 都无法访问用户管理与审计日志。未在 `app_role_permission` 声明的权限码默认拒绝；模板管理权限与出数数据权限相互独立。资金账户（`/api/fund-accounts`）与资金流水报表复用 `finance.read/write`，不新增 `fund.*` 权限组。成本调整单的起草、改草稿与删除草稿用 `document.write`（操作员可提交申请），审核与反审核用 `cost.adjust`——该码同样没有种子行，只有 admin 的 `*` 覆盖；查看调整差异仍只需 `stock.read`。

### 运行日志

- 位置：默认 `<数据目录>/logs/app.log`，可用 `XIAOTU_LOG_DIR` 覆盖；启动失败时退回标准错误。
- 级别：`XIAOTU_LOG_LEVEL=debug|info|warn|error`，默认 `info`。
- 格式：`2026-11-12 18:26:36.101 [INFO] [tid:21467] 消息`，线程安全；单文件 5 MB，滚动保留 `app.log.1`～`app.log.3`。
- 内容：启动版本/模式/数据目录、schema 版本、监听地址、HTTP 访问（方法、路径、状态、请求 id）与退出。
- 敏感信息：访问日志不记录查询串、Cookie、请求体与访问凭据；权威操作留痕仍在数据库 `audit_log`，与运行日志职责分离。

### 应收应付统计口径

`ar_ap_entry` 只在单据审核时写入。v7 迁移为历史销售单按名称回填了 `customer_id`，但**不会为升级前已审核的单据补建往来记录**。因此：

- 应收应付台账（`/api/ar-ap`）、收付款（`/api/settlements`）、账龄（`/api/reports/ar-ap-aging`）与对账单（`/api/reports/statement`）仅统计 **v7 迁移之后审核的单据**；升级前已审核单据不计入往来余额与账龄。收付款单可以为空分摊（预收/预付），资金流水见 `/api/reports/fund-ledger`，账户余额见 `/api/reports/fund-balances`；升级前的收付款记录 `account_id=0`，在流水中归入「未指定账户」。
- 上述接口在响应头返回 `X-Xiaotu-Data-Scope: ar_ap_since_v7`，返回体结构不变。
- 库存余额、库存流水、收发明细直接基于 `stock_ledger`，**不受影响**。
- 需要全量历史往来时，目前只能通过单据反审核后重新审核来生成台账，或后续单独做期初往来录入。

### 报表与对账单

共八类输出：

| 输出 | 接口 | 用途 |
| --- | --- | --- |
| 库存余额表 | `/api/reports/stock-balance` | 仓库 × 商品的当前数量与金额 |
| 库存流水明细 | `/api/reports/stock-ledger` | 按时间列出每笔出入库与成本金额 |
| 收发明细 | `/api/reports/goods-flow` | 商品 × 仓库的期初、本期收入 / 发出、期末 |
| 进销存汇总 | `/api/reports/inventory-summary` | 按商品跨仓库汇总，数量与金额双口径 |
| 销售毛利 | `/api/reports/sales-profit` | 收入取单据金额（退货红字），成本取库存流水，已反审核不计入 |
| 资金流水 | `/api/reports/fund-ledger` | 按账户列出期初、逐笔收款/付款与余额，可导出与打印 |
| 应收应付余额与账龄 | `/api/reports/ar-ap-aging` | 按往来单位与账龄区间汇总，受 v7 统计口径限制 |
| 对账单 | `/api/reports/statement` | 往来单位期初 + 期间单据 − 期间核销 = 期末 |

账户余额一览走 `/api/reports/fund-balances`，汇总每个账户的期初、累计收入、累计支出与余额，并单列历史「未指定账户」。

每类都支持：

- **CSV**：接口加 `?format=csv` 或卡片「导出 CSV」，跟随当前筛选条件；带 BOM 的 UTF-8，Excel 直接打开不乱码；金额按 `1 元 = 100 分` 由整数分换算，两位小数，不使用浮点。
- **XLSX**：接口加 `?format=xlsx` 或卡片「导出 Excel」；服务端直接生成最小 OOXML 工作簿，不引入三方库，金额写为两位小数的数值单元格。
- **打印**：卡片「打印」，经 `/api/print/render` 按当前模板输出；单机版 WKWebView 通过原生桥接打开系统打印面板，可另存为 PDF。
- **权限**：导出与打印和查看同权限（`stock.read` / `finance.read`），不额外开放写权限。

### 主数据批量导入导出

商品、供应商、客户三张列表页支持 Excel 批量维护：

- **导出当前全部**：`GET /api/items/export?format=xlsx|csv`（供应商/客户同理），第 1 行列头，无前言，导出的文件可直接改完再导回。
- **下载模板**：`GET /api/items/template`，只有表头；列名用中文，导入按表头文字匹配，不依赖列顺序。
- **导入**：`POST /api/items/import`，请求体是 `.xlsx` 原始二进制，文件名放 `X-File-Name`（`encodeURIComponent` 编码）。
  仅接受真 xlsx（内部是 ZIP + deflate，已内嵌 miniz 只编 inflate）；CSV 不用于导入。
- **重复策略（已定 A）**：按编码 upsert。编码存在则只覆盖非空单元格，**空单元格一律保持原值**；
  「启用」留空不会改成启用，也不会把停用档案重新启用。同一文件内编码重复直接整批拒绝。
- **事务与备份**：先全量校验，任何一行有错整批不写并返回「行号 + 列名 + 原因」；
  校验通过后先 `VACUUM INTO` 备份到 `<数据目录>/backups`（文件名含 `pre-import-<表>`），备份失败即中止，
  然后单事务写入；审计只记一条 `import`。
- **上限**：5000 行 / 5MB。服务端只对 `/import` 放宽请求体，其它接口仍是 1MB。
- **权限**：导入 `item.write` / `supplier.write` / `customer.write`；导出与模板 `*.read`。
- **错误清单**：页面展示前 100 条，可下载完整 CSV（行号、列名、原因）。
- **注意**：编码/电话建议在 Excel 里设为文本格式，否则前导零会被 Excel 丢掉。

### 自定义打印模板

- 存储：v8 新增 `print_template`（纸张、方向、页边距、字号、页眉页脚、页码、默认标记、`version` 乐观锁、软删除与审计）和 `print_template_column`（列顺序、可见、宽度、对齐、字号、加粗）两张表。
- 服务：`PrintTemplateService` 提供列表、详情、新建、更新、软删除、设为默认与解析；保存时校验目标与列白名单，同一目标默认模板唯一，所有写操作写审计。
- 默认回退：显式 `template_id`（有效且目标匹配）→ 该目标数据库默认模板 → 代码内置默认模板 → 无列通用兜底；模板删除、停用或目标失效都不影响出数。
- 权限：模板管理看 `print.template.read` / `print.template.write`；实际取数仍校验单据、库存或财务各自的数据权限，两层分离。
- 页面：「打印模板」支持新建、从内置默认复制列、调整列显示与顺序、设置默认、编辑与删除；报表、对账单与单据打印应用模板。
- 边界：模板只保存展示配置，不保存 SQL、脚本、HTML 或查询条件；页眉页脚为纯文本，仅做占位符字符串替换。
- 状态：迁移、服务、API、前端与测试均已完成。

### 往来对账单

- 页面「对账单」按往来单位输出期间对账单：期初 + 期间单据 − 期间核销 = 期末，逐行累计余额。
- 接口：`GET /api/reports/statement?partner_key=C:<客户 id>&direction=1&from=YYYY-MM-DD&to=YYYY-MM-DD`；
  权限码 `finance.read`，只读、不落表、无写权限。`direction` 省略时按 key 前缀推断（`C:` 应收、`S:` 应付）。
- 数据来源与账龄一致：单据取 `ar_ap_entry`（作废单据不计入），核销取 `settlement`（撤销产生的负数记录计入）。
  同样受 v7 统计口径限制，响应头带 `X-Xiaotu-Data-Scope`。
- 支持 `?format=csv` 导出（带 BOM 的 UTF-8、金额按元两位小数）与打印视图（含单位名称、报表名、
  往来单位、筛选区间与打印时间，表头跨页重复）。

## 9. 已知限制与口径

- 用户与角色：`admin` / `operator` / `viewer` 三角色与最小用户 CRUD（`/api/users`）；口令为 Argon2id 散列，网络版走口令登录并校验 CSRF、限制登录失败次数。会话落库（8 小时滑动过期，令牌只存散列）。幂等键仍未实现；仓库数据权限口径见下一条。
- 数据权限（1.0 的取舍）：**所有登录用户可见全部仓库数据，不做仓库隔离**，可见范围由角色权限（`item.read`、`stock.read`、`document.read` 等）控制。这不是遗漏，而是 1.0 有意的取舍。1.1 的第一步只做「用户-仓库白名单」，且**只作用于列表与报表的默认视图过滤**，写操作仍按角色判定；不承诺「某仓数据对该用户绝对不可见」——那需要连导出、打印、审计一起封，属于另一个量级的改造。
- 版本能力：`-DXIAOTU_EDITION=community` 构建社区版（当前已接 `max_users=1`），默认构建为全功能版；其余额度项随对应模块逐个接入。
- 单据支持草稿态：存草稿只写单据与明细、不写库存流水，审核时才进单事务；反审核为整单反向。
- 销售订单只做需求确认与转出库来源：确认不动库存、不写流水、不产生往来，只有转出的销售出库单才产生库存与应收。
- 数量按 4 位定点整数存储（×10000），接口传十进制字符串、前端用字符串计算；`unit.qty_decimals` 决定录入精度（件 0 位、千克 3 位、米 2 位），展示时去掉末尾 0。金额仍为分，只有行金额按「数量 × 单价 ÷ 10000」四舍五入取整。
- 成本按移动加权平均，暂不支持其他计价方式；成本重算保持**只读核对**（`/api/stock/cost-recalc` 与库存页「成本重算核对」），不静默写回。金额差异可一键生成成本调整单草稿（`POST /api/cost-adjustments/drafts`）后人工审核，数量差异引导去做盘点；调整单反审核若会使结存金额为负则拒绝，需先处理后续出库或调整。
- 无内置 PDF 导出与针式打印模板：当前通过系统打印面板「另存为 PDF」，A4 / 针式两种尺寸的输出属于 M4。
- Windows 只构建服务端与测试，没有 GUI 壳（`XIAOTU_BUILD_SHELL=OFF`），界面通过浏览器访问。
- 导航使用 `#/<key>` 路由：**刷新保页**在单机版与网络版都有效；**前进 / 后退**只在 `--server` 的浏览器里有效，单机版 WKWebView 没有后退入口。单机版有意不开 `allowsBackForwardNavigationGestures`、也不加后退 / 前进菜单项：侧栏已经是完整导航路径，录入页的脏标记会拦住误跳，横扫手势还会和明细表的横向滚动冲突。未保存离开确认是页内自绘的确认层，不依赖 `window.confirm`（WKWebView 未实现对应面板）；单机版直接关闭窗口不会拦（WKWebView 不执行 `beforeunload`），网络版浏览器会提示，单机版请先保存再关窗。
- 无 Developer ID 证书，`.dmg` 为 ad-hoc 签名且未公证，Gatekeeper 会拦截首次打开，处理方式见第 10 节。
- 应收应付与账龄只统计 v7 迁移之后审核的单据，升级前已审核单据不计入，详见「应收应付统计口径」。
- 未接入 vcpkg 时依赖 `third_party/` 内嵌头文件；接入 vcpkg 后 `vcpkg.json` 会自动生效。

## 10. 打包（macOS .dmg）

一条命令从构建到出可分发的 dmg：

```bash
tools/package/make_dmg.sh
```

产物：`build/package/xiaotu-inventory-0.1.0.dmg`（版本取自 `CMakeLists.txt`），内含 `小兔进销存.app`
与指向 `/Applications` 的「应用程序」软链。脚本、签名参数与校验方式见
[tools/package/README.md](tools/package/README.md)。

**签名现状**：仓库当前没有 Developer ID 证书，脚本默认做 ad-hoc 签名，也未做公证。用户通过网络下载或
拷贝 dmg 后，Gatekeeper 会拦截首次打开。处理方式：

1. macOS 15+：系统设置 → 隐私与安全性 → 底部「仍要打开」，再双击确认；
2. macOS 13/14：右键应用 →「打开」→ 再次「打开」；
3. 命令行：`xattr -d com.apple.quarantine /Applications/小兔进销存.app`。

拿到 Developer ID 证书后可设置 `XIAOTU_SIGN_IDENTITY` 重新打包；要免拦截仍需额外公证与 staple，当前不在仓库范围内。

## 11. 开发流程与并行约定

多台机器同时开发时：

- `main` 是可运行的主干，双方都向 `main` 提交；个人主题分支命名为 `macmini/<主题>` 或 `macbook/<主题>`，完成后合回 `main`。
- 每次动手前先 `git fetch && git pull --ff-only`；提交前再确认远端没有新提交，避免长驻未提交改动。
- 冲突当场本地解决，不使用 `--force`，不 rebase 改写别人已经推上去的提交。
- 热点文件（`README.md`、`web/app.js`、`core/src/migrations.cpp`、`core/src/document.cpp`）的改动尽量集中在一笔提交内，缩短并行窗口。

每次提交前至少通过：

```bash
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
tools/codegen/.venv/bin/python tools/codegen/generate.py --check
```

涉及接口的改动另做一次接口冒烟；涉及迁移的改动必须纯增量、先发表结构设计，已发布迁移不得修改。一笔提交只做一件事，提交信息说明「为什么」。

### CI

#### 平台与边界

- 执行平台为 **Gitee Go**，流水线文件是仓库根目录的 `.workflow/MasterPipeline.yml`；检查逻辑只来自 `tools/ci/check.sh`，Gitee Go 侧不复制步骤。
- 云端只有 **Linux 运行器**（`build@gcc` 10.3，Ubuntu 20.04 基础镜像）。**macOS 全量构建（含 WKWebView 壳）和 Windows MSVC 构建不在云端覆盖**，仍需本地执行；需要自动门禁时接 Gitee Go 自建主机组，把对应机器挂进来。
- 首次开通必须在网页「新建流水线 → 可视化编排 → 保存」：平台会向仓库提交 `.workflow/` 下的标准文件。仓库里的 `MasterPipeline.yml` 已按平台格式写好，若被平台覆盖，用仓库版本替换即可；不要把「提交了 yml」当成已开通。
- 原 GitHub Actions 工作流已删除，避免两套流水线重复维护。

#### 触发规则

| 事件 | 是否触发 |
| --- | --- |
| 推送到 `main` | 是 |
| 指向 `main` 的 PR（创建、更新、评论） | 是 |
| 其他分支推送、Tag、PR 合并产生的推送 | 否（PR 合并后落在 `main` 上，按上一行触发） |

`strategy.blocking: true` 开启阻塞构建：上一次没跑完时，下一次排队等待，不并行消耗核分。

#### 耗时与免费额度

- 单次构建耗时以 Gitee Go 构建记录为准，不在文档里估算。本项目编译目标 60 余个，主要时间在编译与链接。
- 免费额度：**单仓库一次性 200 分钟** + **个人/组织每月 1000 核分**；2C4G 任务按每分钟 2 核分折算，约等于每月 500 分钟。实际扣减以平台构建记录为准。

#### 构建缓存

构建缓存是**平台侧配置**：网页上填的值只存在平台，不会提交回仓库，`.workflow/MasterPipeline.yml` 因此没有缓存字段。在可视化编排里设置：

1. 打开流水线 → 编辑 → `linux_verify` 任务 → **构建缓存**；
2. 填两项：`$HOME/.cache/ccache`（ccache 编译缓存）和 `build/ci-linux`（CMake 构建目录）；
3. 保存。**不要写 `~/.cache/ccache`**：多数 CI 缓存实现不展开波浪号，会建出字面名为 `~` 的目录而永不命中；`check.sh` 打印 `CCACHE_DIR` 的解析结果与仓库根目录下是否存在字面 `~` 目录，可据此判断。

`check.sh` 里 `CCACHE_DIR` 的默认值就是 `$HOME/.cache/ccache`，与平台要填的路径对应。规则：只有构建成功才回传缓存，默认 30 天失效；命中后日志里的 ccache 命中率会明显上升。若缓存中的 CMake 生成器与本次检测到的生成器不一致，脚本会清空 `build/ci-linux` 重新配置。

#### 失败排查

1. 先看日志开头的「工具链」块：`cmake`、`ninja`、`make`、`ccache` 的实际路径与版本，以及最终选用的生成器。
2. `cmake: 缺失` → 镜像没预装 CMake。镜像基于 Ubuntu 20.04，自带的 CMake 可能是 3.16，低于项目要求的 3.25，所以流水线按版本判断：缺失或低于 3.25 时安装固定版本 `cmake==3.31.6`。安装失败通常是 pip 不可用或无外网，此时改用镜像自带的 3.25 以上版本，或接自建主机组。
3. 日志出现 `生成器: Unix Makefiles` 说明镜像里没有 `ninja`，脚本已自动降级，构建应照常通过；此时若失败，看是否缺 `make`。
4. 编译或测试失败：本地执行 `bash tools/ci/check.sh` 复现，检查内容与云端一致。
5. `generate.py --check` 失败：本地重新生成后提交，见「代码生成器」章节。
6. 流水线没被触发：确认目标分支是 `main`；「任务等待中」是阻塞构建在排队，等上一条结束即可。

#### 本地复现

```bash
bash tools/ci/check.sh           # 全量（macOS 含 windows-release）
bash tools/ci/check.sh --quick   # macOS 跳过 windows-release
```

Linux 上该脚本等价于 CI：`XIAOTU_BUILD_SHELL=OFF` 配置构建、`ctest`、`generate.py --check`、平台代码泄漏检查。

平台泄漏检查（`tools/ci/check_platform_leaks.sh`）：`core/` 禁止出现 Apple 条件编译、框架或 WebView 引用；`server/` 只允许 `cli_main.cpp` 在 `#if defined(__APPLE__)` 内引用 WebView 壳。

## 12. 参考

- 用户手册（面向使用者）：`docs/用户手册.md`
- 技术方案：`docs/进销存技术方案.md`
- 数据库结构：`core/src/migrations.cpp`（v13）
- API：`/api/health`、`/api/session`、`/api/items`、`/api/items/import`、`/api/items/export`、`/api/items/template`、`/api/warehouses`、`/api/suppliers`、`/api/suppliers/import`、`/api/suppliers/export`、`/api/suppliers/template`、`/api/customers`、`/api/customers/import`、`/api/customers/export`、`/api/customers/template`、`/api/units`、`/api/users`、`/api/audit-logs`、`/api/ar-ap`、`/api/settlements`、`/api/settlements/{id}/reverse`、`/api/reports/stock-balance`、`/api/reports/stock-ledger`、`/api/reports/goods-flow`、`/api/reports/inventory-summary`、`/api/reports/sales-profit`、`/api/reports/ar-ap-aging`、`/api/reports/statement`（报表接口加 `?format=csv` 或 `?format=xlsx` 导出）、`/api/documents`、`/api/documents/{id}`、`/api/documents/{id}/approve`、`/api/documents/{id}/reverse`、`/api/documents/{id}/convert`、`/api/documents/{id}/close`、`/api/cost-adjustments/drafts`、`/api/fund-accounts`、`/api/print-templates`、`/api/print-templates/{id}/default`、`/api/print/targets`、`/api/print/render`、`/api/stock/balances`、`/api/stock/ledger`、`/api/stock/cost-recalc`
