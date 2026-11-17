# macOS 打包（.dmg）

一条命令从 `macos-release` 构建产物生成可分发的 `.dmg`：

```bash
tools/package/make_dmg.sh
```

产物在 `build/package/`（已在 `.gitignore` 的 `build/` 下，不入库）：

```text
build/package/xiaotu-inventory-0.9.0.dmg
```

dmg 内包含：

- `小兔进销存.app`（bundle id `com.simem.xiaotu`，版本取自 `CMakeLists.txt` 与 `Info.plist`）
- `应用程序 -> /Applications` 软链

脚本会先跑 `cmake --preset macos-release` 与 `cmake --build --preset release`，再校验 `Info.plist` 版本、
复制 bundle、签名、`hdiutil create -format UDZO`，最后输出 dmg 的字节数与 SHA256。

## 图标

图标从 `shell/mac/icon/candidate-a-rabbit-barcode.svg` 生成，不手工拖拽：

```bash
tools/package/make_icons.sh
```

依赖 macOS 自带的 `sips`、`iconutil` 与 `python3`。产物：

- `shell/mac/icon/xiaotu-a-rabbit-barcode.icns`：16/32/64/128/256/512 及 2x（icp6 的 64 由脚本补入）。
- `packaging/win/xiaotu-a-rabbit-barcode.ico`：16/24/32/48/64/128/256，PNG 压缩。
- 旧 `shell/mac/icon/xiaotu.icns`（候选 B）保留用于追溯，不再被 CMake/Info.plist 引用。

Windows 两个 exe 与 Inno Setup 安装包都引用同一个 `.ico`；`xiaotu-shell.exe`、`xiaotu-server.exe`
的图标来自各自的 `version.rc`。

## 构建号与更新配置

打包时通过环境变量把构建号与更新配置注入 `Info.plist`：

```bash
XIAOTU_BUILD=2026111501 \
XIAOTU_UPDATE_MANIFEST_URL=https://updates.example.com/macos/stable/latest.json \
XIAOTU_UPDATE_FEED_URL=https://updates.example.com/macos/stable/appcast.xml \
XIAOTU_UPDATE_PUBKEY="$(cat ~/keys/update_signing.pub.b64)" \
tools/package/make_dmg.sh
```

- `XIAOTU_BUILD` 必填：纯数字且单调递增，缺失或非数字时脚本直接退出 1，不再沿用 CMake 缓存默认值。构建号不变时 Sparkle 会判为“没有更新”。
- 更新三项（`XIAOTU_UPDATE_MANIFEST_URL` / `XIAOTU_UPDATE_FEED_URL` / `XIAOTU_UPDATE_PUBKEY`）必须同时提供或同时留空：
  - 全部留空：脚本打印“更新功能关闭”警告，应用内 `status.enabled=false`，仍会正常出包；
  - 只给一部分：直接报错退出，不猜测意图。
- 两个地址必须是 `https://`；公钥是 Sparkle 的 `SUPublicEDKey`（base64，解码后 32 字节的 Ed25519 公钥）。任一项不合法直接退出，不会静默降级成无更新配置。
- 构建完成后脚本用 `PlistBuddy` 回读三键，与传入值不一致即失败，避免 CMake 缓存残留或转义问题。
- “有更新”的最小闭环还需要 `SUFeedURL` 指向的 appcast 与安装包（见下节）。

本地开发不经过本脚本：`cmake --preset macos-dev`（或直接 `macos-release`）允许不传构建号，但配置阶段会打印
“构建号 2026111301 是开发默认值，此产物不可用于发布”的警告。出包与 CI 走
`-DXIAOTU_RELEASE_BUILD=ON -DXIAOTU_BUILD=<纯数字>`：缺号、非数字或仍为默认值都会在 configure 阶段失败。

## 签名与公证

仓库已具备 Developer ID 证书，发布包按以下方式签名 + 公证 + staple：

```bash
XIAOTU_SIGN_IDENTITY="Developer ID Application: Yuwei Lin (BMCUC4B6XX)" \
XIAOTU_NOTARY_PROFILE="xiaotu-notary" \
tools/package/make_dmg.sh
```

- 脚本用 `codesign --options runtime`（hardened runtime）签名，再 `notarytool submit --wait`、`stapler staple`，最后用 `spctl -a -vv` 与 `stapler validate` 自检。
- `XIAOTU_NOTARY_PROFILE` 是 `xcrun notarytool store-credentials` 创建的钥匙串 profile；凭据不写进仓库。
- 不传 `XIAOTU_SIGN_IDENTITY` 时回落到 ad-hoc 签名并打印警告，只用于本地验证，不是可分发产物。
- 正式产物实测 `spctl` 输出 `source=Notarized Developer ID`，首次打开不再被 Gatekeeper 拦截。

## 数据目录

单机版数据仍然写入：

```text
~/Library/Application Support/com.simem.xiaotu/app.db
```

需要隔离测试时可指定临时目录：

```bash
"/Volumes/小兔进销存/小兔进销存.app/Contents/MacOS/xiaotu-inventory" --data /tmp/xiaotu-dmg-test
```

## 更新清单与 appcast

```bash
python3 -m venv tools/package/.venv
tools/package/.venv/bin/pip install -r tools/package/requirements.txt

# 生成并签名清单，同时生成 Sparkle appcast
tools/package/.venv/bin/python tools/package/update_manifest.py generate \
  --file build/package/xiaotu-inventory-0.9.0.dmg \
  --version 0.9.0 --build 2026111710 \
  --url https://dl.xiaotu.online/xiaotu/xiaotu-inventory-0.9.0.dmg \
  --schema-min 1 --schema-max 39 --api-min 1 --api-max 1 \
  --key ~/.config/xiaotu-updates/update_signing_2026.pem --key-id 2026-11 \
  --out build/package/latest.json --appcast build/package/appcast.xml

# 校验清单签名与安装包哈希
tools/package/.venv/bin/python tools/package/update_manifest.py verify \
  --manifest build/package/latest.json --pub ~/.config/xiaotu-updates/update_signing_2026.pub.pem \
  --file build/package/xiaotu-inventory-0.9.0.dmg
```

私钥不进入仓库与构建产物；`--appcast` 生成的是对 dmg 文件字节的 Ed25519 签名，供 Sparkle 校验。

## 发布到 OSS

```bash
PRODUCT=xiaotu OSS_BUCKET=xiaotu-update OSS_REGION=cn-hongkong \
tools/package/publish_oss.sh
```

一条命令完成：上传 dmg（immutable 长缓存）→ 生成并签 `latest.json` / `appcast.xml` → 本地验签 →
上传清单（60 秒缓存）→ purge CDN → 回写桶根 `index.html` 的字节数与 SHA256 → 下载一次预热 CDN。

- 凭据：优先读 `OSS_ACCESS_KEY_ID` / `OSS_ACCESS_KEY_SECRET`；未设置时读仓库外
  `~/.config/xiaotu-updates/oss-credentials.env`（权限必须 600/400）。
- 关闭下载页同步：`UPDATE_INDEX_PAGE=false`；关闭预热：`WARMUP=false`；只本地生成不碰 OSS：`OSS_SKIP=true`。
- 发布地址：`https://dl.xiaotu.online/xiaotu/updates/stable/latest.json` 与同目录 `appcast.xml`。

## 逐档放量

清单签名后不能直接改 `rollout_percent`。用 `rollout_oss.sh` 按计划重签、上传、purge 并等待 CDN 生效：

```bash
# 取回线上清单（发布脚本的临时目录不保留）
curl -fsS https://dl.xiaotu.online/xiaotu/updates/stable/latest.json \
  -o build/package/latest.json

# 预览：只重签 + 验签，不上传、不等待
tools/package/rollout_oss.sh --manifest build/package/latest.json --dry-run

# 默认 1% → 10% → 50% → 100%，每档 24 小时
PRODUCT=xiaotu OSS_BUCKET=xiaotu-update OSS_REGION=cn-hongkong \
  tools/package/rollout_oss.sh --manifest build/package/latest.json --yes
```

- 只改清单；appcast 不随比例变化，不重传。每档归档到 `build/package/rollout/<version>-<build>/`。
- 每档上传后轮询 CDN，确认 `rollout_percent` 生效才进入下一档；超时即停，当前档保持生效。
- Ctrl-C 停在当前档；重跑时从下一档开始即可。`--interval` 支持 `30s` / `90m` / `12h` / `2d`。
- 强制更新（`mandatory=true`）不受比例限制；网络版服务端实际比例取 `min(清单 rollout, 配置 rollout_percent)`。
