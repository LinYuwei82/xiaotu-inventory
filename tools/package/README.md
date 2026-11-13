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

截至本仓库当前状态：

- **本机没有可用的 Developer ID 证书**（`security find-identity -v -p codesigning` 返回 0 个有效身份），
  因此脚本默认使用 **ad-hoc 签名**（`codesign --force --deep --sign -`），并在结束时打印
  「未签名（ad-hoc），distributable 需 Developer ID 签名并公证」。
- 公证需要有效的 Developer ID 证书、Apple Developer 账号与 notarytool 凭据。脚本支持
  `XIAOTU_NOTARY_PROFILE`，未提供时会打印「跳过公证」，不会把未公证的包当作可分发产物。

后果：dmg 通过网络下载或拷贝后会被打上 `com.apple.quarantine` 隔离属性，Gatekeeper 会拦截首次打开，
提示「无法打开，因为 Apple 无法检查其是否包含恶意软件」。

用户侧处理方式：

1. macOS 15（Sequoia）及以后：打开「系统设置 → 隐私与安全性」，在底部点击「仍要打开」，
   然后再次双击应用确认。
2. macOS 13/14：右键（或 Control-点击）应用图标 →「打开」→ 在弹窗中再次点「打开」。
3. 命令行（任选其一）：

   ```bash
   xattr -d com.apple.quarantine /Applications/小兔进销存.app
   ```

   或先挂载 dmg，直接对 dmg 内的应用执行同样的命令。

拿到 Developer ID Application 证书后，可这样构建带签名与公证的包：

```bash
XIAOTU_SIGN_IDENTITY="Developer ID Application: 你的名字 (TEAMID)" \
XIAOTU_NOTARY_PROFILE="xiaotu-notary" \
tools/package/make_dmg.sh
```

`XIAOTU_NOTARY_PROFILE` 是 `xcrun notarytool store-credentials` 创建的 keychain profile。
未提供时脚本会明确打印「跳过公证」；只有提交成功并 `stapler staple` 后，dmg 才算可分发产物。

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
  --file build/package/xiaotu-inventory-1.4.2.dmg \
  --version 1.4.2 --build 2026111301 \
  --url https://cdn.example.com/releases/macos/1.4.2/xiaotu-inventory-1.4.2.dmg \
  --schema-min 28 --schema-max 32 --api-min 1 --api-max 1 \
  --key ~/keys/update_signing.pem --key-id 2026-01 \
  --out build/package/latest.json --appcast build/package/appcast.xml

# 校验清单签名与安装包哈希
tools/package/.venv/bin/python tools/package/update_manifest.py verify \
  --manifest build/package/latest.json --pub ~/keys/update_signing.pub.pem \
  --file build/package/xiaotu-inventory-1.4.2.dmg
```

私钥不进入仓库与构建产物；`--appcast` 生成的是对 dmg 文件字节的 Ed25519 签名，供 Sparkle 校验。
