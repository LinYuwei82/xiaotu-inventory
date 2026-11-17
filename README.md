# 小兔进销存（1.0.0）

> 主仓库在 Gitee：`git@gitee.com:lin-yuwei168/xiaotu-inventory.git`。GitHub 的 `LinYuwei82/xiaotu-inventory` 是同源镜像：Linux 门禁在 Gitee Go，Windows 的 MSVC 构建与**未签名安装包**由镜像仓库的 `.github/workflows/windows-build.yml` 承担，macOS 全量构建仍在本地执行。
>
> 开发、提交与发版都以 Gitee 为准；镜像不接收 PR。
>
> 发布与署名：个人开发者 `si_mem`（已定，见 [docs/待决策清单.md](docs/待决策清单.md) 第 15 条）；版权 `Copyright © 2026 si_mem`。

macOS 优先的进销存软件：**一套 C++20 代码，两种运行形态**。

- 不带参数启动 = **单机版**：内嵌服务只监听 `127.0.0.1` 的随机端口，生成一次性 token，用系统 WKWebView 打开本地页面。
- `--server` 启动 = **网络版**：监听 `0.0.0.0`，用浏览器访问；使用用户名与口令登录（首次需先给内置管理员设置口令）。

当前版本 1.0.0（唯一来源：仓库根 `VERSION` 文件）。按 [docs/进销存技术方案.md](docs/进销存技术方案.md) 的里程碑口径：M1–M3 与 M5 已完成；M4 除针式套打真机验证外已完成（纸型、边距、缩放与打印列勾选已落地，真机验收仍为阻塞项）；M6 的 macOS `.dmg` 打包脚本与用户手册已完成，Windows 壳已可接入顶层构建、CI 产出未签名安装包，安装/卸载真机验收见 [docs/Windows验收清单.md](docs/Windows验收清单.md)；Windows 代码签名暂不购买证书，签名脚本与 CI 已就绪，详见 [docs/未签名分发注意事项.md](docs/未签名分发注意事项.md)。macOS `.dmg` 已用 Developer ID 签名、公证并 staple。

已打通：**商品（分类 / 条码 / 税率）/ 单位 / 仓库 / 职员 / 客户 / 供应商档案（支持 Excel 批量导入导出，客户供应商已合表到 partner 并支持合并与撤销）→ 库存、应收、应付、资金账户四类期初导入 → 采购入库、销售出库、采购退货、销售退货、其他入库、其他出库、调拨、盘点、成本调整（草稿与反审核）→ 销售订单确认与转出库（可记录业务员）→ 库存余额与流水 → 应收应付台账与核销（按 partner 维度归集）→ 收付款单与资金账户 → 往来对账单 → 15 个报表接口（14 类输出）的导出与打印 → 自定义打印模板**，并具备内置 `admin` 用户、三角色权限、审计日志（含来源 IP 与请求号）、单据幂等与乐观锁、登录失败锁定与强制改密、成本口径参数（含税价 / 不含税净额，可切换并标注切换日）、前端离线提示与表单草稿、打印纸型 / 边距 / 缩放与 Cmd+P、报表导出流式化（不再有 10 万行上限）与 macOS `.dmg` 打包。桌面壳提供撤销 / 重做 / 剪切 / 复制 / 粘贴 / 全选编辑菜单；单据录入时商品下拉显示编码、名称与规格，数量框旁显示单位；桌面版支持直接「导出 PDF」。成本重算保持只读核对，差异通过成本调整单落账。

## 0. 数据库结构版本

当前 schema 为 **v39**（启动时自动迁移，已发布迁移不允许修改，校验 checksum）。

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
| v13 | 成本调整单：重建 `doc_line`（明细行仓库、调整金额、调整原因，允许 `qty=0`）与 `stock_ledger`（金额非零）；`cost.adjust` 作为成本调整单审核 / 反审核的写权限，不写 `app_role_permission` 种子行，仅由 admin 的 `*` 覆盖 |
| v14 | `stock_balance` 增加两个非负触发器（`BEFORE INSERT` 与 `BEFORE UPDATE OF qty, amount_cents`）；不重建表、不改历史数据，历史负结存留给盘点单修正 |
| v15 | 盘点流水按方向拆分为 `STK_CNT_IN` / `STK_CNT_OUT`，反审核写对应 `_REVERSE`；按历史数量符号回填 |
| v16 | 其他出入库增加业务类型 `doc_header.biz_category`，历史单据统一回填 `OTHER` |
| v17 | 商品分类 `item_category`（`parent_id` + `path`）、`item.category_id` / `tax_rate_bp` / `barcode`，并预留 `item_barcode` 多码表 |
| v18 | 职员档案 `employee` 与单据业务员 `doc_header.salesperson_id` / `salesperson_name`（仅销售类单据） |
| v19 | 客户 / 供应商税号 `tax_no`，客户信用额度 `credit_limit_cents`（0 视为不限制） |
| v20 | 应收应付期初标记 `ar_ap_entry.opening`，`uk_ar_ap_doc` 放宽为只约束 `doc_id > 0` |
| v21 | `item_barcode.item_id` 的唯一索引改为普通索引，为一商品多码预留；条码全局唯一不变 |
| v22 | 新增 `app_setting` 键值表（公司名称/电话/地址）与 `setting.read` / `setting.write` 权限 |
| v23 | 负库存开关 `allow_negative_stock`（默认关），结存非负触发器改为按开关判断 |
| v24 | 会话超时参数 `session_idle_minutes` / `session_absolute_hours`（默认 30 分钟 / 12 小时） |
| v25 | 把已有 `item.barcode` 回填为主码行，一商品多码正式启用 |
| v26 | 单据行含税字段：`tax_rate_bp` / `price_includes_tax` / `net_cents` / `tax_cents`，历史行按 0 税回填 |
| v27 | 打印模板纸型与缩放：`paper_kind`（`SHEET` / `CONTINUOUS`）与 `scale_mode`（`FIT_WIDTH` / `ORIGINAL`），历史模板默认单页纸、按纸宽自适应 |
| v28 | 客户/供应商合表第一步：建 `partner`（客户/供应商双角色位、税号、信用额度、`credit_days` 占位、`merged_into`）与 `partner_legacy_map`；客户/供应商 1:1 搬入，旧表与 `C:` / `S:` / `T:` 键保持不动，不做合并 |
| v29 | `partner.origin` 来源标记（`AUTO_ARCHIVE` = 档案首次引用时业务补建，`MANUAL` = 历史迁移、手工或合并），默认 `MANUAL` 补齐老数据 |
| v30 | 客户/供应商合并：`partner_merge_log` 记录保留档案、前后快照、映射快照、源档案历史键与台账/核销水位；合并只写 `partner.merged_into` 与角色位，历史键不动 |
| v31 | 单据幂等与乐观锁：`doc_header.request_id`（非空唯一，部分索引）与 `version`（历史行 0）；重复提交返回首次单据，更新类操作按版本校验（冲突返回 `VERSION_CONFLICT`） |
| v32 | 登录安全：`app_user.must_change_password`（管理员重置口令后强制改密）、`failed_count` 与 `locked_until`（连续 5 次错口令锁定 15 分钟） |
| v33 | 库存成本口径：`cost_basis`（`INCLUSIVE` 含税价 / `NET` 不含税净额，默认含税）与 `cost_basis_changed_at`（最近切换时间），只影响新审核单据 |
| v34 | 审计来源：`audit_log.client_ip` 与 `request_id`，记录 HTTP 来源地址与客户端请求号；历史行为空串 |
| v35 | 批次/保质期预留：`doc_line` 与 `stock_ledger` 各加 `batch_no`、`expiry_date`（默认空串），当前不写入、不显示、不参与计算 |
| v36 | 历史销售单据 `doc_header.partner_id` 回填：只填 `partner_id = 0` 且 `customer_id > 0`、能按 `partner` 档案（含合并链）精确映射的行；有值不动，无档案与 `T:` 文本单位保持 0，不按名称硬匹配；不改结构，可单独回退 |
| v37 | 单据列表统一筛选与合计的索引：重建 `idx_doc_header_type_date` 为 `(doc_type, doc_date DESC, id DESC)`，新增 `idx_doc_header_type_status`（`doc_type, status`）；只改索引，不动表结构与数据 |
| v38 | 单据表头备注 `doc_header.remark`（`TEXT NOT NULL DEFAULT ''`，最多 200 个字符）；历史单据一律空串，不回填 |
| v39 | 单机版「本机免输」长期凭据 `local_remember_token`：只存 token 哈希、`issued_at`、`expires_at` 与 `last_used_at`；与 `app_session` 独立，改口令不自动清除 |

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
| OpenSSL | 3.0+ | Ed25519 清单验签（配置阶段 REQUIRED）；macOS 用 `openssl@3`，Windows 用 Chocolatey / vcpkg |

可选：安装 vcpkg 并设置 `VCPKG_ROOT` 后，`nlohmann/json`、`cpp-httplib` 会优先使用 vcpkg 版本；未安装时自动回落到 `third_party/` 内嵌头文件，构建不依赖网络。OpenSSL 不在回落范围内，缺失时配置会直接失败。

```bash
brew install cmake ninja ccache openssl@3
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"   # openssl@3 是 keg-only，需手动指路
```

Windows 只构建服务端与测试时另需 CMake 3.25+、Ninja、Visual Studio 2022 Build Tools（MSVC，C++20）与 OpenSSL 3.0（`choco install openssl`，或 `vcpkg` 并设置 `OPENSSL_ROOT_DIR`）；若要构建 `shell/win` 桌面壳，还需要 WebView2 SDK 与目标机器上的 WebView2 Runtime（见下方「Windows 桌面壳」）。
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

构建目标有三个业务目标：`xiaotu_core`（纯业务）、`xiaotu_server`（HTTP 服务与内嵌静态资源）、`xiaotu-inventory`（macOS WKWebView 壳），另有命令行入口 `xiaotu-server` 与六个测试目标 `core_smoke`、`config_smoke`、`session_smoke`、`platform_paths_smoke`、`xlsx_smoke`、`masterdata_smoke`。
Preset 对应关系：`macos-release` / `macos-dev`（构建名 `release` / `dev`）用于 macOS 全量构建；
`windows-release` / `windows-dev` 用于 Windows，`linux-ci` 用于 Linux 与 CI（构建目录 `build/ci-linux`）；三个非 macOS preset 均已设置 `XIAOTU_BUILD_SHELL=OFF`。
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

- **Ninja**：所有 preset 均使用 Ninja。
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

#### Windows 桌面壳（可选）

`shell/win` 是 Win32 + WebView2 工程，可用 `-DXIAOTU_BUILD_WIN_SHELL=ON` 接入顶层 CMake（默认 OFF、仅 Windows 生效）；CI 目前仍按独立工程编译壳，并用 Inno Setup 编出**未签名安装包**上传 artifact。签名是参数化空位：配置了证书 secret 才调用 signtool，没有就跳过并在产物说明里标注「未签名」。

前置条件：

- Windows 10/11 x64，Visual Studio 2022 Build Tools（含 C++ 工具集），CMake 3.25+、Ninja。
- **WebView2 SDK**：`shell\win\fetch-webview2.ps1` 会从 NuGet 下载 `Microsoft.Web.WebView2`（默认 1.0.4314.35），解压到 `shell\win\.cache\webview2`；也可手动把 nupkg 放到该目录后重跑脚本。
- **WebView2 Runtime**：开发机与目标机器都需要。Win11 自带；Win10 可从 <https://go.microsoft.com/fwlink/p/?LinkId=2124703> 安装，或随安装包引导安装。

```powershell
.\shell\win\fetch-webview2.ps1
cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shell\win\build
```

产物为 `shell\win\build\xiaotu-shell.exe`，需与 `xiaotu-server.exe` 同目录运行。细节见 `shell/win/README.md`。

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

[sales]
credit_limit_mode = "warn"

[update]
channel = "stable"
rollout_percent = 100
auto_check = true
prefetch = true
# manifest_url = "https://dl.xiaotu.online/xiaotu/server/updates/stable/latest.json"
# manifest_file = "/var/lib/xiaotu/update-manifest.json"
```

默认读取 `<数据目录>/config.toml`，也可用 `--config` 或 `XIAOTU_CONFIG` 指定。优先级：**命令行 > 环境变量 > config.toml > 内置默认**。配置文件支持 `[server]`、`[storage]`、`[log]`、`[sales]`、`[update]` 五个段，写错段名/键名会带行号报错。

`[sales]` 段只有一个键：`credit_limit_mode`，`warn`（默认，超信用提示后仍可审核）/ `reject`（超信用直接拒绝）。

`[update]` 服务于网络版更新链路，可用键与默认值：

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `manifest_url` | 空（不启用） | 厂商清单地址，必须 HTTPS；拉取后验签并缓存 10 分钟，失败回退旧缓存 |
| `manifest_file` | 空（不启用） | 本地/离线清单路径；与 `manifest_url` 同时配置时作为兜底 |
| `channel` | `stable` | 只接受 `stable` / `beta` |
| `min_supported_version` | 空 | 形如 `1.2.3`；低于它的桌面客户端在启动握手时直接展示“必须升级”阻塞页，网络版检查接口同时返回强制升级标记 |
| `rollout_percent` | `100` | 灰度比例，1–100；单机版与网络版都按安装 ID 稳定分桶，强制更新（`mandatory=true`）不受比例限制 |
| `auto_check` | `true` | 网络版服务端启动 30 秒后与每 24h±2h 自动检查更新；运行中不可切换，**改后需重启服务** |
| `prefetch` | `true` | 自动检查发现新版本时后台下载并校验安装包；失败按 60s / 600s 退避重试 2 次；运行中不可切换，**改后需重启服务** |
| `public_key_file` | 空 | 清单验签公钥文件 |
| `verifier_python` | `python3` | 清单验签脚本的解释器 |
| `verifier_script` | 空 | 清单验签脚本路径 |

`auto_check` 与 `prefetch` 只影响自动链路：关闭前者停止后台检查，手动「检查更新」仍可用；关闭后者不自动下载，用户仍可点「下载并校验」。两项在服务启动时读取，运行中不提供切换，修改后需重启；`GET /api/update/status` 只读回显当前取值。

服务端更新链路（网络版）：`GET /api/update/check` 免登录做轻量握手；`GET /api/update/status`、`POST /api/update/download`、`POST /api/update/download/cancel`、`POST /api/update/install` 需要登录，下载、取消与安装仅管理员可用。下载目录固定为 `<数据目录>/updates/`，安装包先按清单 `size` 与 sha256 校验、再用同一验证工具验签，失败会删除文件并写日志；下载前清掉旧包，后台检查确认已是最新且下载空闲时清空目录，取消会终止下载并删除半成品。安装需二次确认：服务端复验后解包定位新程序，返回响应后优雅退出，替换脚本把旧程序备份为 `<程序>.old`、覆盖新程序并按原参数重启；覆盖失败会回滚旧版本，日志写入 `<数据目录>/updates/staging-*/update-apply.log`。

完整说明、systemd 单元与运维步骤见 [docs/部署与配置.md](docs/部署与配置.md)，示例见 `deploy/config.toml.example`、`deploy/systemd/xiaotu.service`。

浏览器访问 `http://<本机 IP>:8080/`，用 `admin` 和新口令登录。局域网其他机器访问时把 IP 换成本机地址；若前面有 TLS 反向代理，带上 `X-Forwarded-Proto: https`，会话 Cookie 会自动加 `Secure`。

并发能力有可复现的压测脚本与结论：`tools/loadtest/mixed_load.py` 模拟 10–30 个用户混合读写，核对单据号与库存一致性，方法与实测数据见 [docs/并发压测.md](docs/并发压测.md)。

登录安全现状：

- 会话保存在数据库里，重启服务后仍然有效。**空闲 30 分钟**没有操作即失效，每次访问滑动续期（同一会话最多每分钟写一次库）；同时有**绝对 12 小时**上限，从登录签发时刻起算，期间无论是否活跃，到时都必须重新登录。两个值可在「系统参数」页调整（`session_idle_minutes` / `session_absolute_hours`，默认 30 / 12）。登出、改密、用户停用或删除都会让对应会话立即失效。数据库里只存令牌的散列，不存明文令牌。改数据目录或恢复另一个备份等于换了一个库，旧会话不再存在，需要重新登录。
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

程序启动时会检测数据目录并给出提示：命中 iCloud Drive（`~/Library/Mobile Documents/`）或 OneDrive 时，控制台、日志与界面顶部都会显示告警和迁移指引；只告警不拦截。

同一数据目录同一时间只允许一个实例运行（`<数据目录>/app.lock`，Windows 用命名互斥体、macOS 用 `flock`）。重复启动会提示已有实例的 PID 后直接退出；`--backup`、`--set-password` 与 `--partner-preview` 属于维护命令，不参与该锁，`--restore` 仍要求停服。

### 备份与恢复

- 迁移前自动备份：启动时若数据库版本低于代码内置版本，先用 `VACUUM INTO` 写 `<备份目录>/app-<时间>-pre-v<旧版本>.db`（默认 `<数据目录>/backups`）；备份失败则中止迁移，不带着未备份的数据升级。**粒度是「每次启动迁移前一张整库快照」，不是每版一张**；一次跨多版的升级只有升级前这一个还原点。需要回到中间版本时，用该快照恢复后，再按目标版本手工重放迁移，不能直接跳到中间版本。
- 手动备份：`<程序> --data <数据目录> --backup`，输出快照路径与字节数。
- 每日备份：`<程序> --data <数据目录> --backup-daily`，当天尚未成功备份时才执行，生成 `app-<时间>-daily.db` 并清理旧备份；保留最近 30 份日备份，每个自然月额外保留最新 1 份；manual 与 `pre-*` 备份不参与清理。
- 备份位置：默认 `<数据目录>/backups/`，可用 `[storage] backup_dir` 单独指定：空串或未配置表示沿用默认；相对路径按数据目录解析（不受启动 cwd 影响），绝对路径原样使用。备份与库同盘仍是最脆弱的位置——同一块盘坏了，库和备份一起没；单独指定到另一块物理盘或 NAS 才有意义，且仍建议保留异地副本。备份不加密，加密也不能替代异地。
- 恢复：`<程序> --data <数据目录> --restore <备份文件>`；先做 `PRAGMA integrity_check`，目标已存在时留一份 `pre-restore-*.db`，再覆盖并清理 `-wal` / `-shm`。恢复必须在服务停止时执行。复制或校验失败时会用 `pre-restore-*.db` 自动回滚，不会留下半截库。
- 恢复向导：登录后进入「系统 → 备份与恢复」，可列出备份（时间、大小、schema）并预检查（能否打开、完整性、是否本程序库、schema 兼容性），二次确认后提交。提交即进入维护态（其他写请求返回 503 `MAINTENANCE_MODE`）并优雅退出；下次启动在打开数据库之前复用 `restore_database` 完成替换，成功后写 `restore` 审计，结果在重启后的页面顶部展示。
- 恢复审计：成功与失败都写 `audit_log`（`action=restore`、`entity=database`），`entity_no` 为备份文件名，`detail` 记录来源（命令行 / 网页向导）、恢复到的 schema 版本、`pre-restore-*` 安全备份文件名（成功）或失败原因（失败）。命令行没有登录用户，`actor` 固定为 `cli`，不伪造用户身份；网页向导的 `actor` 为发起恢复的登录用户。
- 快照是一致性单文件，可直接复制到另一台机器后恢复。
- 往来单位归并预览（只读）：`<程序> --data <数据目录> --partner-preview`，可选 `--partner-preview-format text|json|csv`。命令在**迁移之前**执行，只 `SELECT customer / supplier / ar_ap_entry`，不升级结构、不写业务表；按税号、名称+电话、仅名称三档列出候选合并组，并单独列出同一名称存在多个 `partner_key` 的单位。规则来源见 `docs/客户供应商合表方案.md`。

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
├── third_party/                   # 内嵌依赖（SQLite / cpp-httplib / nlohmann-json / argon2 / miniz）
├── .workflow/MasterPipeline.yml   # Gitee Go 流水线
├── .github/workflows/windows-build.yml  # GitHub 镜像的 Windows MSVC 构建
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
├── shell/                         # 平台代码（可关闭）
│   ├── common/                    # 壳公共代码预留目录（当前无跟踪文件）
│   ├── mac/icon/                  # 图标源 SVG（A/B/C 候选）、A 的 .icns（现用）与旧 xiaotu.icns（B，保留）
│   ├── mac/webview.mm             # WKWebView 宿主
│   └── win/                       # Win32 + WebView2 壳工程（可用 XIAOTU_BUILD_WIN_SHELL 接入顶层，默认关闭）
├── web/                           # 前端（无构建步骤）
│   ├── index.html / app.js / styles.css
│   └── gen/                       # ← 生成物（列表页/表单页 + pages.json）
├── tests/                         # core_smoke / config_smoke / session_smoke / platform_paths_smoke / xlsx_smoke / masterdata_smoke
├── packaging/                     # Windows 分发打包脚本（win/）
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
    ├── nightly/                   # 定时任务（nightly.sh 与 launchd plist）
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

`tools/codegen/schema/editions.json` 描述社区版与全功能版的能力状态与额度，生成 `core/include/xiaotu/gen/edition_caps.h` 与 `web/gen/edition_caps.js`（由 `-DXIAOTU_EDITION` 选择生效版本）。运行时按同一份数据执行：`hidden` 能力在服务端直接拒绝（`FEATURE_UNAVAILABLE`，HTTP 403），社区版的 `report.profit` / `report.dashboard` / `deployment.network` 均按此处理；`max_items`、`max_documents`、`max_partners`、`max_warehouses`、`max_print_templates` 在新建路径上校验（`QUOTA_EXCEEDED`，HTTP 409，并说明上限），已有数据超限不回删、不锁读。前端读 `edition_caps.js` 隐藏无权入口，与后端拒绝形成两层。`/api/health` 返回 `api_version` 与 `capabilities`；请求头带 `X-API-Version` 时只接受当前版本与 N-1，其他版本返回 426。额度数值的机器可读来源是 `tools/codegen/schema/editions.json`，同一组数字还要同步写在 `docs/EDITIONS.md` 第 5 节：改额度时两处必须一起改，不一致视为文档缺陷。`print.custom_template` 在社区版为启用状态，额度由 `max_print_templates=1` 单独限制：社区版允许 1 套自定义模板，内置模板只读；全功能版不限。

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

2. **重新生成**：`tools/codegen/.venv/bin/python tools/codegen/generate.py`。DAO、服务、路由、前端页面、CMake 清单一次到位，`web/gen/pages.json` 自动追加该页（含 `permission` 与 `group`，schema 里写 `nav_group`），前端导航按 `group` 自动归组，无需手改。
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

`DocumentEngine::save_and_approve` 在一个写事务内完成：校验 → 取单据号 → 写主子表 → 按定义生成库存流水与余额 → 执行 `on_approve`。`reverse` 反向写流水、回滚余额、置为 `VOIDED`，**不删除原流水**；反审核入库类单据前会按 (仓库, 商品) 校验下游是否已消耗，撤回后结存不能为负，否则整单拒绝（`REVERSE_WOULD_NEGATIVE`），撤不回就不撤回。库存与往来都按定义条件执行，`affects_stock=false` / `finance_direction=0` 的单据天然不产生副作用。

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

当前库存策略：出库类单据（销售出库、采购退货、销售订单转出库）审核 / 转出前校验可用库存，不足则整单拒绝（`STOCK_INSUFFICIENT`）并回滚事务，不产生负数结存或半途写入。反审核入库类单据（含盘点盘盈、调拨入库腿、销售退货）同样先校验撤回后结存是否非负，已被下游消耗则拒绝（`REVERSE_WOULD_NEGATIVE`）。

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

权限点按十五组登记：`item`、`unit`、`warehouse`、`supplier`、`customer`、`employee`、`opening`、`document`、`stock`、`finance`、`print.template`、`setting`、`user`、`audit`、`cost`，共 26 个权限码，其中 22 个有 `app_role_permission` 种子行。`user.read`、`user.write`、`audit.read` 与 `cost.adjust` 四个码没有种子行，实际只有 admin 的 `*` 通配覆盖：operator 与 viewer 无法访问用户管理与审计日志，也不能审核成本调整单。未在 `app_role_permission` 声明的权限码默认拒绝；模板管理权限与出数数据权限相互独立。资金账户（`/api/fund-accounts`）与资金流水报表复用 `finance.read/write`，不新增 `fund.*` 权限组。系统参数由 operator 维护、viewer 只读（`setting.read/write`）。成本调整单的起草、改草稿与删除草稿用 `document.write`（操作员可提交申请），审核与反审核用 `cost.adjust`（仅管理员，见上）；查看调整差异仍只需 `stock.read`。

### 运行日志

- 位置：默认 `<数据目录>/logs/app.log`，可用 `XIAOTU_LOG_DIR` 覆盖；启动失败时退回标准错误。
- 级别：`XIAOTU_LOG_LEVEL=debug|info|warn|error`，默认 `info`。
- 格式：`2026-11-12 18:26:36.101 [INFO] [tid:21467] 消息`，线程安全；单文件 5 MB，滚动保留 `app.log.1`～`app.log.3`。
- 内容：启动版本/模式/数据目录、schema 版本、监听地址、HTTP 访问（方法、路径、状态、请求 id）与退出。
- 敏感信息：访问日志不记录查询串、Cookie、请求体与访问凭据；权威操作留痕仍在数据库 `audit_log`，与运行日志职责分离。

### 应收应付统计口径

`ar_ap_entry` 只在单据审核时写入。v7 迁移为历史销售单按名称回填了 `customer_id`，但**不会为升级前已审核的单据补建往来记录**。因此：

- 应收应付台账（`/api/ar-ap`）、收付款（`/api/settlements`）、账龄（`/api/reports/ar-ap-aging`）与对账单（`/api/reports/statement`）仅统计 **v7 迁移之后审核的单据**；升级前已审核单据不计入往来余额与账龄。台账列表默认排除已作废（`VOIDED`）记录，显式传 `status=VOIDED` 可回看历史。账龄按 `as_of` 做时点快照：业务日期晚于截止日的单据不计入，已核销金额按 `settlement_allocation` 与核销日期还原，不再使用当前的 `settled_cents`。收付款单可以为空分摊（预收/预付），资金流水见 `/api/reports/fund-ledger`，账户余额见 `/api/reports/fund-balances`；升级前的收付款记录 `account_id=0`，在流水中归入「未指定账户」，`account_id=all` 表示全部账户（期初为各账户期初合计），页面默认选「全部账户」。
- 上述接口在响应头返回 `X-Xiaotu-Data-Scope: ar_ap_since_v7`，返回体结构不变。
- 库存余额、库存流水、收发明细直接基于 `stock_ledger`，**不受影响**。
- 需要全量历史往来时，目前只能通过单据反审核后重新审核来生成台账，或后续单独做期初往来录入。

### 报表与对账单

共 15 个接口、14 类输出：

| 输出 | 接口 | 用途 |
| --- | --- | --- |
| 库存余额表 | `/api/reports/stock-balance` | 仓库 × 商品的当前数量与金额 |
| 库存流水明细 | `/api/reports/stock-ledger` | 按时间列出每笔出入库与成本金额，支持方向与业务类型筛选 |
| 收发明细 | `/api/reports/goods-flow` | 商品 × 仓库的期初、本期收入 / 发出、期末 |
| 进销存汇总 | `/api/reports/inventory-summary` | 按商品跨仓库汇总，数量与金额双口径 |
| 销售毛利 | `/api/reports/sales-profit` | 收入取单据金额（退货红字），成本取库存流水，已反审核不计入 |
| 销售报表（客户 / 业务员） | `/api/reports/sales-summary?dimension=customer\|salesperson` | 期间销售额、退货额、净额、成本、毛利与销售额占比 |
| 销售明细 | `/api/reports/sales-detail` | 逐行列出已审核的销售出库与退货，带业务员与单行毛利 |
| 经营看板 | `/api/reports/dashboard` | 今日 / 本月销售额与毛利、库存总额、应收欠款 Top N |
| 采购明细 | `/api/reports/purchase-detail` | 采购入库与退货逐行列出，退货数量与金额为负、单价保持正数 |
| 采购汇总 | `/api/reports/purchase-summary?by=supplier\|item` | 按供应商或商品汇总；入库与退货分列，入库均价只取入库行 |
| 其他出入库汇总 | `/api/reports/other-flow-summary` | 按业务类型（赠品 / 期初 / 领用 / 报损 / 其他）分列入库与出库 |
| 资金流水 | `/api/reports/fund-ledger` | 按账户列出期初、逐笔收款/付款与余额，可导出与打印 |
| 资金余额 | `/api/reports/fund-balances` | 各资金账户的当前余额；`account_id=all` 为全部账户合计 |
| 应收应付余额与账龄 | `/api/reports/ar-ap-aging` | 按往来单位与账龄区间汇总，受 v7 统计口径限制 |
| 对账单 | `/api/reports/statement` | 往来单位期初 + 期间单据 − 期间核销 = 期末 |


采购汇总的「入库均价」= 入库行金额合计 ÷ 入库行数量合计，只计采购入库，退货不参与；某分组只有退货时均价显示「—」，净数量为负时标注「净退货」。合计行同样按全量入库行计算，不对分组均价再平均。

库存流水、采购明细与销售明细改为服务端真分页：响应带 `total` / `limit` / `offset`，页面每页 200 行、显示「共 N 行，当前第 x–y 行」并可翻页；合计与导出按全量计算，导出强制 `offset=0`。导出改为流式写出（CSV 边查边分块推送，XLSX 工作表分批落临时文件后分块组装），**不再有 10 万行上限**，内存占用与导出行数无关。

销售报表（按客户 / 按业务员）只统计已审核的销售出库（SAL_OUT）与销售退货（SAL_RET），期间按单据日期闭区间；退货数量与金额取负、单价保留正数；成本取审核时写入 `stock_ledger` 的移动加权成本，毛利 = 净销售额 − 成本，净销售额为 0 时毛利率显示「—」。客户或业务员为空的单据单列「未指定」。页面、CSV、XLSX、打印与 PDF 五处输出同源，导出一律全量。

经营看板（`/api/reports/dashboard`）给出今日与本月销售额、退货额、成本、毛利，库存总额（按 `stock_balance.amount_cents` 合计）与应收欠款 Top N；销售口径与销售报表一致，退货按负数冲减。


库存余额表支持 `warehouse_id` / `item_id` / `hide_zero` 筛选，页面、CSV、XLSX 与打印共用同一条件。库存余额、收发明细、进销存汇总、销售毛利、应收应付账龄五张报表都带合计行，页面、CSV、XLSX 与打印四处口径一致：金额列直接汇总，毛利率按汇总后的收入与毛利重算；数量列因跨单位不可加总，合计行留空并在表头说明。账户余额一览也支持 `format=csv|xlsx` 导出。

三条报表口径已定（详见 `docs/待决策清单.md`）：`qty = 0` 但金额不为 0 的流水在收发明细与进销存汇总**单列「其他调整」**，不并入收入 / 发出；反审核回冲**按操作日入账**，页面与导出标注「反审核回冲（原单 YYYY-MM-DD）」；撤销收付款**保持事实发生口径**，对账单标注撤销行并给出「期间核销净额」。

账户余额一览走 `/api/reports/fund-balances`，汇总每个账户的期初、累计收入、累计支出与余额，并单列历史「未指定账户」。

每类都支持：

- **CSV**：接口加 `?format=csv` 或卡片「导出 CSV」，跟随当前筛选条件；带 BOM 的 UTF-8，Excel 直接打开不乱码；金额按 `1 元 = 100 分` 由整数分换算，两位小数，不使用浮点。
- **XLSX**：接口加 `?format=xlsx` 或卡片「导出 Excel」；服务端直接生成最小 OOXML 工作簿，不引入三方库，金额写为两位小数的数值单元格。
- **打印 / PDF**：卡片「打印」经 `/api/print/render` 按当前模板输出；桌面版可经原生桥直接「导出 PDF」（保存面板，不弹系统打印面板），也可打开系统打印面板另存为 PDF。
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
  校验通过后先 `VACUUM INTO` 备份到备份目录（默认 `<数据目录>/backups`，可用 `[storage] backup_dir` 指定；文件名含 `pre-import-<表>`），备份失败即中止，
  然后单事务写入；审计只记一条 `import`。
- **上限**：5000 行 / 5MB。服务端只对 `/import` 放宽请求体，其它接口仍是 1MB。
- **权限**：导入 `item.write` / `supplier.write` / `customer.write`；导出与模板 `*.read`。
- **错误清单**：页面展示前 100 条，可下载完整 CSV（行号、列名、原因）。
- **注意**：编码/电话建议在 Excel 里设为文本格式，否则前导零会被 Excel 丢掉。

### 自定义打印模板

- 存储：v8 新增 `print_template`（纸张、方向、页边距、字号、页眉页脚、页码、默认标记、`version` 乐观锁、软删除与审计）和 `print_template_column`（列顺序、可见、宽度、对齐、字号、加粗）两张表；v27 增加 `paper_kind`（`SHEET` 单页纸 / `CONTINUOUS` 连续纸）与 `scale_mode`（`FIT_WIDTH` 按纸宽自适应 / `ORIGINAL` 原始尺寸）。
- 服务：`PrintTemplateService` 提供列表、详情、新建、更新、软删除、设为默认与解析；保存时校验目标与列白名单，同一目标默认模板唯一，所有写操作写审计。
- 默认回退：显式 `template_id`（有效且目标匹配）→ 该目标数据库默认模板 → 代码内置默认模板 → 无列通用兜底；模板删除、停用或目标失效都不影响出数。
- 权限：模板管理看 `print.template.read` / `print.template.write`；实际取数仍校验单据、库存或财务各自的数据权限，两层分离。
- 页面：「打印模板」支持新建、从内置默认复制列、调整列显示与顺序、设置默认、编辑与删除；报表、对账单与单据打印应用模板。
- 版式：服务端 `plan_print_layout` 一次算出纸型、可打印宽度、表宽、缩放与告警，页面、打印与 PDF 共用同一份 `layout`，数字不再各处各算。`FIT_WIDTH` 只缩版式（CSS `zoom`），不改任何数值；缩放后有效字号低于 7pt 会告警，替代做法是取消勾选部分列——打印列勾选就是用来砍列，而不是把字缩到看不清。
- 纸型：A4 / A5 / 自定义宽高（针式 241×140、241×93 走自定义）；方向可选纵向或横向。**连续纸单独一条规则**：`paper_kind = CONTINUOUS` 时固定原始尺寸与套打位置，`scale_mode` 一律按 `ORIGINAL` 处理，不跟随按纸宽缩放；超宽只告警（改纸宽或精简列），不静默截断。这样针式套打位置不会因缩放漂移，与 A4 的缩放策略彻底分开。
- 不做分段接列：超宽报表只提示改横向或精简列，不把列拆到第二段/第二页。
- 边界：模板只保存展示配置，不保存 SQL、脚本、HTML 或查询条件；页眉页脚为纯文本，仅做占位符字符串替换。
- 状态：迁移、服务、API、前端与测试均已完成。

### 系统参数与诊断

- 页面「系统参数」（`/api/settings`，权限 `setting.read` / `setting.write`）：只开放公司名称、联系电话、公司地址三个键，落在 v22 的 `app_setting` 键值表；operator 可改、viewer 只读、admin 通配。Audit 记 `entity=setting`，每个变化的键一条，detail 为 `{"from":"…","to":"…"}`。
- 打印消费：三个值作为打印模板占位符 `{{company_name}}`、`{{company_phone}}`、`{{company_address}}`；模板抬头为空时默认打印公司名称，电话与地址由模板按需组合。
- 负库存开关 `allow_negative_stock`（默认关）：关闭时出库超库存由应用层 `ensure_available` 与数据库触发器双重拒绝；开启后允许数量为负，金额仍不允许为负。v23 把 v14 的两个触发器改为读取该键，只有显式 `1` 才放行，键缺失或为其他值一律拒绝，默认路径不存在绕过；修改写审计。
- 会话超时 `session_idle_minutes` / `session_absolute_hours`（默认 30 分钟 / 12 小时）：空闲窗口滑动续期，绝对上限按 `app_session.created_at` 判断；修改写审计。超时统一返回 401，前端沿用现有重新登录流程。
- 固定口径（页面只读展示、后端没有可改实现）：单据编号为「前缀 + YYYYMMDD + 4 位序号」，前缀按单据类型硬编码（如 `XSCK`/`CGRK`），本版**不提供配置**；日期固定 `YYYY-MM-DD`；金额固定人民币元 2 位小数；数量小数位跟随商品单位（0–4 位）。
- 页面「关于与诊断」（`/api/diagnostics`，登录即可看，无写权限）：应用版本、schema 版本与迁移条数、edition（full/社区版）、构建 profile、编译器与编译时间、数据目录、数据库文件大小、监听地址、当前用户与角色；不返回口令、密钥或会话令牌。页面提供「复制诊断信息」，按 `标签：值` 逐行复制。

### 往来对账单

- 页面「对账单」按往来单位输出期间对账单：期初 + 期间单据 − 期间核销 = 期末，逐行累计余额。
- 接口：`GET /api/reports/statement?partner_key=C:<客户 id>&direction=1&from=YYYY-MM-DD&to=YYYY-MM-DD`；
  权限码 `finance.read`，只读、不落表、无写权限。`direction` 省略时按 key 推断：`C:` / `T:` 应收、`S:` 应付、`P:` 按 `partner` 角色位，兼买卖时以台账方向为准。
- 往来键与归集：写入端产出 `P:{partner_id}`（新档案首次被单据或期初引用时按需补建 `partner` 与映射，不批量回填历史数据）；历史 `C:` / `S:` / `T:` 原样保留。补建是幂等的：同一编码或同一 legacy id 已被写入时复用已有记录，重复引用同一新档案只会产生一条 partner 与一条映射，审核不会失败。补建记录带 `origin = AUTO_ARCHIVE`，历史/手工/合并记录为 `MANUAL`，归并预览的文本、JSON、CSV 都会输出来源。账龄、对账单、台账筛选、收付款明细与欠款 Top N 在分组前折成 partner 维度，同一单位的历史 `C:` / `S:` 单与新 `P:` 单合成一行；`T:` 仍按名称独立。档案记录不存在或补建失败时才退回 `T:{名称}`，不会产出指向空档案的 `P:` 键。
- 数据来源与账龄一致：单据取 `ar_ap_entry`（作废单据不计入），核销取 `settlement`（撤销产生的负数记录计入）。
  同样受 v7 统计口径限制，响应头带 `X-Xiaotu-Data-Scope`。
- 支持 `?format=csv` / `?format=xlsx` 导出（带 BOM 的 UTF-8、金额按元两位小数）与打印视图（含单位名称、报表名、
  往来单位、筛选区间与打印时间，表头跨页重复）。

## 9. 已知限制与口径

- 用户与角色：`admin` / `operator` / `viewer` 三角色与最小用户 CRUD（`/api/users`）；口令为 Argon2id 散列，网络版走口令登录并校验 CSRF、连续 5 次错口令锁定 15 分钟，管理员重置口令后强制改密。会话落库（空闲 30 分钟滑动 + 绝对 12 小时，令牌只存散列，均可在系统参数页调整）。多角色（自定义角色与权限勾选）尚未实现，仍是三角色固定权限；仓库数据权限口径见下一条。
- 数据权限（当前版本的取舍）：**所有登录用户可见全部仓库数据，不做仓库隔离**，可见范围由角色权限（`item.read`、`stock.read`、`document.read` 等）控制。这不是遗漏，而是当前版本有意的取舍。后续版本的第一步只做「用户-仓库白名单」，且**只作用于列表与报表的默认视图过滤**，写操作仍按角色判定；不承诺「某仓数据对该用户绝对不可见」——那需要连导出、打印、审计一起封，属于另一个量级的改造。
- 版本能力：`-DXIAOTU_EDITION=community` 构建社区版，默认构建为全功能版。`hidden` 能力在服务端拒绝（403），五个规模额度在新建路径上校验（409 并提示上限），已有数据超限不回删、不锁读；前端按生成的 `web/gen/edition_caps.js` 隐藏入口。`report.custom` 与 `system.rbac` 目前没有可调用接口（自定义报表与多角色未实现），能力位仅作占位。
- 多角色不做：固定六个角色，兼岗通过调整角色分配解决，不做权限并集。
- 多计量单位不做：4 位小数已够表达，箱 / 瓶换算走条码或备注。
- 批次 / 序列号 / 保质期不做：单据行与库存流水只预留空的 `batch_no` / `expiry_date`，不写入、不显示、不参与计算。
- 数据权限白名单不做：所有登录用户可见全部仓库数据（口径见上一条）。
- 外部对接不做：Excel 导入导出即为对接面。
- License 与防拷贝不做加密狗：社区版免费、专业版靠功能差异分层，弱校验即可。
- Windows 自动更新不做：不接入 WinSparkle，首发手动下载安装包。
- 单据支持草稿态：存草稿只写单据与明细、不写库存流水，审核时才进单事务；反审核为整单反向。
- 销售订单只做需求确认与转出库来源：确认不动库存、不写流水、不产生往来，只有转出的销售出库单才产生库存与应收。
- 数量按 4 位定点整数存储（×10000），接口传十进制字符串、前端用字符串计算；`unit.qty_decimals` 决定录入精度（件 0 位、千克 3 位、米 2 位），展示时去掉末尾 0。金额仍为分，只有行金额按「数量 × 单价 ÷ 10000」四舍五入取整。
- 成本按移动加权平均，暂不支持其他计价方式；成本重算保持**只读核对**（`/api/stock/cost-recalc` 与库存页「成本重算核对」），不静默写回。金额差异可一键生成成本调整单草稿（`POST /api/cost-adjustments/drafts`）后人工审核，数量差异引导去做盘点；调整单反审核若会使结存金额为负则拒绝，需先处理后续出库或调整。
- 桌面版已支持直接「导出 PDF」（原生保存面板，不弹系统打印面板），导出走窗口模态打印会话（`runOperationModalForWindow`），避免 WKWebView 异步分页在无窗口会话下产出空白页。打印与 PDF 均按模板纸型（A4 / A5 / 自定义针式 241×140、241×93）与页边距设置 `NSPrintInfo`，PDF 的 MediaBox 与实际纸张一致；针式套打位置仍待真机验证。打印入口的查询串统一用 `URL` + `URLSearchParams` 组装。页码依赖打印引擎：WKWebView 实测不渲染 `@page` margin box、不支持 `counter(page)`，`position: fixed` 页脚只在末页出现且解析为 0；`show_page_no` 仅作提示性字段，不做前端分页与 PDF 后处理，网络版可在浏览器打印对话框勾选页眉和页脚。
- Windows：顶层 CMake 默认不构建 GUI 壳（`XIAOTU_BUILD_WIN_SHELL=OFF`），界面通过浏览器访问；`shell/win` 是 Win32 + WebView2 壳工程，可显式打开顶层开关，也可独立构建。打包脚本与验收脚本共用同一套壳路径解析顺序，找不到壳会报错而不是静默出无壳包。
- 更新策略：macOS 单机版已端到端跑通——启动 30 秒首查、之后每日一次（±2 小时抖动），检查失败按 1 分钟 / 10 分钟 / 1 小时后继续重试，只开 stable。清单与安装包托管在 `https://dl.xiaotu.online`（阿里云 OSS 香港 + CDN），`latest.json` / `appcast.xml` 做 Ed25519 签名，安装包由 Developer ID 签名 + 公证 + staple。自动检查与后台预下载两个开关已实现（默认开，关于页可改；首次后台下载前有流量提示，可取消，预下载失败按 60s/600s 退避、最多 3 次，同一时间只保留一个待装包）。壳内已按清单校验平台、通道、`min_os`、API 区间、schema 与 `rollout_percent`（按安装 UUID 稳定分桶，强制更新不受限）；`mandatory=true` 时登录后弹出不可跳过的阻塞提示，安装前按清单大小提示“将重新下载约 X MB”，并展示 `mandatory_deadline`；网络版还支持 `min_supported_version` 握手阻断。安装仍由内嵌的 Sparkle 2.10.1 在用户确认后完成，预下载的包不会交给 Sparkle 复用，安装时会重新下载。恢复模式与旧版本回退已实现（安装前保留 1 份 `.app`，连续 3 次启动失败后可一键回退，稳定启动 3 次后清理），但“替换 `.app` 后重启”的回退演练尚未真机执行。已提供 `tools/package/rollout_oss.sh` 自动逐档放量（默认 1% → 10% → 50% → 100%，每档 24 小时，自动重签、上传、purge 并轮询 CDN 生效；`--dry-run` 可预览）。beta 通道与静默安装不做。网络版服务端已实现同名的 `auto_check` / `prefetch` 与服务端调度，以及检查/状态/下载/安装接口，并接入厂商清单（`manifest_url`）与自动替换重启（确认后退出、备份、覆盖、按原参数重启，失败回滚）。Windows 首发仍为手动下载安装包，不接 WinSparkle。
- 导航使用 `#/<key>` 路由：**刷新保页**在单机版与网络版都有效；**前进 / 后退**只在 `--server` 的浏览器里有效，单机版 WKWebView 没有后退入口。单机版有意不开 `allowsBackForwardNavigationGestures`、也不加后退 / 前进菜单项：侧栏已经是完整导航路径，录入页的脏标记会拦住误跳，横扫手势还会和明细表的横向滚动冲突。未保存离开确认与提示 / 输入框都是页内自绘（`askDialog`），不依赖 `window.confirm` / `alert` / `prompt`（WKWebView 未实现对应面板）；单机版直接关闭窗口不会拦（WKWebView 不执行 `beforeunload`），网络版浏览器会提示，单机版请先保存再关窗。
- 商品条码支持**一商品多码**（v25 起正式启用）：`item_barcode` 保存全部码，其中一个为主码；`item.barcode` 只作主码镜像并由应用层同步，避免两处真相。条码全局唯一，空码不占位；商品表单可增删码并切换主码，扫码按任意码精确命中商品。导入导出用 `|` 分隔多个码、第一个为主码。升级时 v25 会把已有 `item.barcode` 回填为主码行，不丢码。箱码换算、一码多商品、校验位计算未实现。
- 含税价**第一步已落地**（v26）：单据行保存 `tax_rate_bp`（整数万分比，默认取商品税率、可逐行覆盖）、`price_includes_tax` 与算好的 `net_cents` / `tax_cents`。**逐行算税再汇总**，每行恒有「净额 + 税额 = 价税合计」，整数分四舍五入、不引入浮点；选逐行是因为每行展示的净额 / 税额能与合计逐分对上，汇总后再算税会出现「行税额之和 ≠ 合计税额」。退货红字沿用同一公式，数量与金额按红字取负后税额同步为负，冲回对称。单据页面、打印、CSV、XLSX 均输出「税额」与「价税合计」。含税行与不含税行的「金额」列含义不同（前者是价税合计、后者是不含税金额），因此该列头统一写作「金额（录入价）」，并且「价税合计」列常显，数值不做任何改动。库存成本口径由 v33 参数控制：默认按含税价（`amount_cents`），可切到不含税净额（`net_cents`），只影响切换后新审核的单据，报表标注切换日；历史单据的结存、流水与毛利不重算、不回填。
- 审计日志覆盖登录成功 / 失败（失败记录尝试的用户名与来源 IP）、导出（`format=csv|xlsx` 与 `/export`，记录路径与筛选条件）、手动备份与恢复、用户角色变更（`role_change`，含原角色与新角色）、系统参数修改（`entity=setting`，逐键记录原值与新值）、主数据导入、单据审核 / 反审核、收付款、成本超限确认等。每条审计记录来源 IP（`client_ip`）与客户端请求号（`request_id`），审计页可按这两列筛选，CSV / XLSX 导出同步带出；单据接口沿用前端携带的幂等键作为请求号，未带请求号的接口与 CLI 路径留空，不伪造。导出审计只记 200 成功的请求并在响应写出前落库；`config.toml` 仍由运维改文件后重启生效，不写审计。
- macOS `.dmg` 已用 Developer ID Application 签名并公证 + staple，Gatekeeper 不再拦截；Windows 安装包未签名，首次打开需手动放行 SmartScreen，完整客户指引见 [docs/客户安装说明.md](docs/客户安装说明.md)，发版 Checklist 见 [docs/未签名分发注意事项.md](docs/未签名分发注意事项.md)。
- 应收应付与账龄只统计 v7 迁移之后审核的单据，升级前已审核单据不计入，详见「应收应付统计口径」。
- 未接入 vcpkg 时依赖 `third_party/` 内嵌头文件；接入 vcpkg 后 `vcpkg.json` 会自动生效。

## 10. 打包（macOS .dmg）

一条命令从构建到出可分发的 dmg：

```bash
tools/package/make_dmg.sh
```

产物：`build/package/xiaotu-inventory-1.0.0.dmg`（版本取自仓库根 `VERSION` 文件），内含 `小兔进销存.app`
与指向 `/Applications` 的「应用程序」软链。脚本、签名参数与校验方式见
[tools/package/README.md](tools/package/README.md)。

包体实测（0.9.0）：dmg **6,633,958 B（6.33 MiB）**；`.app` 实占 13,844 KiB（约 13.5 MiB）；主可执行 10,153,072 B；内嵌 `Sparkle.framework` 3,032 KiB；图标 867,537 B。引入 Sparkle 与静态 OpenSSL 后，原「dmg ≤ 3 MiB」目标不再适用。

**签名与公证**：dmg 用 `Developer ID Application: Yuwei Lin (BMCUC4B6XX)` 签名（hardened runtime），并已公证 + staple。打包命令：

```bash
XIAOTU_SIGN_IDENTITY="Developer ID Application: Yuwei Lin (BMCUC4B6XX)" \
XIAOTU_NOTARY_PROFILE="xiaotu-notary" \
XIAOTU_BUILD=<单调递增构建号> \
XIAOTU_UPDATE_MANIFEST_URL=https://dl.xiaotu.online/xiaotu/updates/stable/latest.json \
XIAOTU_UPDATE_FEED_URL=https://dl.xiaotu.online/xiaotu/updates/stable/appcast.xml \
XIAOTU_UPDATE_PUBKEY=<Ed25519 公钥 base64> \
tools/package/make_dmg.sh
```

**发布**：`PRODUCT=xiaotu tools/package/publish_oss.sh` 一条命令完成上传 dmg、生成并签 `latest.json` / `appcast.xml`、本地验签、上传清单、purge CDN、同步下载页数字与预热 CDN；OSS 凭据只从环境变量或仓库外 600 权限文件读取。详见 [tools/package/README.md](tools/package/README.md)。

用户侧放行方式见 [docs/发行与安装说明.md](docs/发行与安装说明.md)。

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
- 云端只有 **Linux 运行器**（`build@gcc` 10.3，Ubuntu 20.04 基础镜像）。**Windows MSVC 构建不在 Gitee 云端覆盖**，改由 GitHub 镜像仓库的 `windows-build` 工作流承担；**macOS 全量构建（含 WKWebView 壳）**仍需本地执行。
- 首次开通必须在网页「新建流水线 → 可视化编排 → 保存」：平台会向仓库提交 `.workflow/` 下的标准文件。仓库里的 `MasterPipeline.yml` 已按平台格式写好，若被平台覆盖，用仓库版本替换即可；不要把「提交了 yml」当成已开通。
- Gitee 侧不使用 GitHub Actions，避免两套 Linux 流水线重复维护。**GitHub 镜像仓库**另有一条独立的 Windows 构建，见下节。

#### 触发规则

| 事件 | 是否触发 |
| --- | --- |
| 推送到 `main` | 是 |
| 指向 `main` 的 PR（创建、更新、评论） | 是 |
| 其他分支推送、Tag、PR 合并产生的推送 | 否（PR 合并后落在 `main` 上，按上一行触发） |

`strategy.blocking: true` 开启阻塞构建：上一次没跑完时，下一次排队等待，不并行消耗核分。流水线另设 `stepTimeout: 60`（分钟），卡死的构建会被取消，避免堵住排队队列。

#### GitHub 镜像的 Windows 构建

- 文件：`.github/workflows/windows-build.yml`；在 `push` 到 `main` 或推送 `v*` tag 时触发，在 `windows-latest` 上依次执行
  `cmake --preset windows-release` → `cmake --build --preset windows-release` → `ctest --test-dir build/windows`。
- 只做「编得过 + 用例过」，**不打包、不签名**。依赖 runner 自带的 VS2022 MSVC、CMake 与 Ninja；
  `windows-release` preset 不启用 ccache，`XIAOTU_BUILD_SHELL=OFF` 所以不编 WebView 壳。
- 镜像只推 `main` 和 tag，因此这条流水线是**合并后才发现**，不是 PR 事前拦截。需要事前拦截时，
  再把自建 Windows runner 挂到 Gitee Go 的主机组。

#### 耗时与免费额度

- 单次构建耗时以 Gitee Go 构建记录为准，不在文档里估算；主要时间在编译与链接。
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

Linux 上该脚本等价于 CI：使用 `linux-ci` preset（`XIAOTU_BUILD_SHELL=OFF`）配置构建、`ctest`、`generate.py --check`、平台代码泄漏检查。

平台泄漏检查（`tools/ci/check_platform_leaks.sh`）：`core/` 禁止出现 Apple 条件编译、框架或 WebView 引用；`server/` 只允许 `cli_main.cpp` 在 `#if defined(__APPLE__)` 内引用 WebView 壳。

## 12. 参考

- 用户手册（面向使用者）：`docs/用户手册.md`
- 技术方案：`docs/进销存技术方案.md`
- 数据库结构：`core/src/migrations.cpp`（v39）
- API：`/api/health`、`/api/session`、`/api/items`、`/api/items/import`、`/api/items/export`、`/api/items/template`、`/api/warehouses`、`/api/suppliers`、`/api/suppliers/import`、`/api/suppliers/export`、`/api/suppliers/template`、`/api/customers`、`/api/customers/import`、`/api/customers/export`、`/api/customers/template`、`/api/units`、`/api/users`、`/api/audit-logs`、`/api/ar-ap`、`/api/settlements`、`/api/settlements/{id}/reverse`、`/api/reports/stock-balance`、`/api/reports/stock-ledger`、`/api/reports/goods-flow`、`/api/reports/inventory-summary`、`/api/reports/sales-profit`、`/api/reports/ar-ap-aging`、`/api/reports/fund-ledger`、`/api/reports/fund-balances`、`/api/reports/statement`（报表接口加 `?format=csv` 或 `?format=xlsx` 导出）、`/api/documents`、`/api/documents/{id}`、`/api/documents/{id}/approve`、`/api/documents/{id}/reverse`、`/api/documents/{id}/convert`、`/api/documents/{id}/close`、`/api/cost-adjustments/drafts`、`/api/fund-accounts`、`/api/print-templates`、`/api/print-templates/{id}/default`、`/api/print/targets`、`/api/print/render`、`/api/stock/balances`、`/api/stock/ledger`、`/api/stock/cost-recalc`

`GET /api/documents` 的两种返回形态：不带 `keyword`、`date_from`、`date_to`、`status`、`amount_min`、`amount_max`、`page`、`page_size` 中任一参数时，仍返回原来的 JSON 数组（兼容旧调用）；带任一参数时返回 `{rows, total_count, total_amount_cents, page, page_size}`。关键字覆盖单号 / 往来单位 / 业务员 / 备注四项，日期与金额均为闭区间，`status` 取 `draft` / `approved` / `reversed`，`page_size` 上限 100（默认 20），合计与列表使用同一套 WHERE、不受分页影响。
