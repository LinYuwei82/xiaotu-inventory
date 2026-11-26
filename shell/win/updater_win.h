// 小兔进销存 Windows 桌面壳 —— 自动更新桥接层（P1：core/update_engine 薄端口）
//
// 架构（见 docs/更新模块拆分重构方案.md §5）：
//   状态机、清单判定、进度聚合、持久化全部在 core/update_engine；
//   本类只做三件事：
//     1. 提供平台端口实现（WinHTTP 传输 / BCrypt 哈希 / ShellExecute 安装 /
//        系统时钟 / updater_win.json 存储，含旧格式一次性迁移）；
//     2. 桥方法 → 引擎调用 + 旧返回码映射（对外签名与返回码不变）；
//     3. statusJson 适配器：引擎统一 status_json 补回旧壳独有键
//        （download_state / download_progress / pending_local_path），
//        前端 normalizeWinUpdateStatus 无需改动。
//
//   shell/win/main.cpp 通过 ICoreWebView2::AddHostObjectToScript 把 UpdaterWin 暴露给前端，
//   前端 JS 用 window.chrome.webview.hostObjects.updater_win.checkNow() 等调用。
//
// 依赖（全是系统自带或仓库已有，零新增第三方）：
//   - WinHTTP (winhttp.lib) —— HTTP GET
//   - BCrypt  (bcrypt.lib)  —— SHA256 计算
//   - OpenSSL (XIAOTU_HAS_OPENSSL=1) —— Ed25519 验签（随 core 一起链接）

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xiaotu::updater {

// 异步回调（一律在主线程调）
using Completion = std::function<void(const std::string& code, const std::string& detail)>;

class UpdaterWin {
 public:
  static UpdaterWin& instance();

  // 初始化：manifest_url 为空时更新功能关闭
  void start(const std::string& manifest_url, const std::string& channel);

  // 立即检查一次（绕过 24h 定时，遵守暂停与跳过）
  void checkNow(Completion cb);

  // 下载待更新安装包（后台线程，回调在主线程）
  void downloadUpdate(Completion cb);

  // 安装：静默跑 ISCC，覆盖并重启壳（回调前 ISCC 就会关当前进程）
  void installUpdate(Completion cb);

  // 取消后台下载
  void cancelPrefetch(Completion cb);

  // 更新总开关与后台预下载开关
  void setOptions(bool auto_check, bool prefetch, Completion cb);

  // 强制升级握手：读服务端 /api/health 的 min_supported_version
  void checkMinSupportedVersion(Completion cb);

  // JSON 序列化（给前端直接读；引擎 status_json + 旧键适配）
  std::string statusJson() const;

 private:
  UpdaterWin();
  ~UpdaterWin();

  // 启动 30 秒后首次自动检查，之后 24h±2h（失败按 check_retry_delay 退避）
  void autoCheckLoop();

  // 平台端口 + 引擎（定义在 cpp，见 §5.1）
  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::string manifest_url_;  // url_mutex_ 保护
  mutable std::mutex url_mutex_;

  std::thread auto_check_thread_;
  std::atomic<bool> auto_check_stop_{false};

  std::thread download_thread_;
  std::atomic<bool> download_running_{false};
};

}  // namespace xiaotu::updater
