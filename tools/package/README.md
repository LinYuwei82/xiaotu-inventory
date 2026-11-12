# macOS 打包（.dmg）

一条命令从 `macos-release` 构建产物生成可分发的 `.dmg`：

```bash
tools/package/make_dmg.sh
```

产物在 `build/package/`（已在 `.gitignore` 的 `build/` 下，不入库）：

```text
build/package/xiaotu-inventory-0.1.0.dmg
```

dmg 内包含：

- `小兔进销存.app`（bundle id `com.simem.xiaotu`，版本取自 `CMakeLists.txt` 与 `Info.plist`）
- `应用程序 -> /Applications` 软链

脚本会先跑 `cmake --preset macos-release` 与 `cmake --build --preset release`，再校验 `Info.plist` 版本、
复制 bundle、签名、`hdiutil create -format UDZO`，最后输出 dmg 的字节数与 SHA256。

## 签名与公证

截至本仓库当前状态：

- **本机没有可用的 Developer ID 证书**（`security find-identity -v -p codesigning` 返回 0 个有效身份），
  因此脚本默认使用 **ad-hoc 签名**（`codesign --force --deep --sign -`）。
- 仓库没有公证（notarization）流程，也没有内置公证凭据。

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

如果以后拿到 Developer ID Application 证书，可这样构建带签名的包：

```bash
XIAOTU_SIGN_IDENTITY="Developer ID Application: 你的名字 (TEAMID)" tools/package/make_dmg.sh
```

签名仍需配合 `notarytool` 提交公证并在用户侧 staple，才能做到下载后直接双击打开；
公证需要 Apple Developer 账号，超出当前仓库范围。

## 数据目录

单机版数据仍然写入：

```text
~/Library/Application Support/com.simem.xiaotu/app.db
```

需要隔离测试时可指定临时目录：

```bash
"/Volumes/小兔进销存/小兔进销存.app/Contents/MacOS/xiaotu-inventory" --data /tmp/xiaotu-dmg-test
```
