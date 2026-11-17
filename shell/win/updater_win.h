// 小兔进销存 Windows 桌面壳 —— 自动更新桥接层
//
// 架构：
//   updater_win 负责三件事：
//     1. 启动后在后台拉取 latest.json（WinHTTP）
//     2. 调用 core 层纯函数（xiaotu::parse_manifest / evaluate_manifest / update_signature）
//        做版本判定、强制升级、灰度分桶、Ed25519 验签
//     3. 下载 setup.exe → SHA256 校验 → 调用 ISCC 静默覆盖重启
//
//   shell/win/main.cpp 通过 ICoreWebView2::AddHostObjectToScript 把 UpdaterWin 暴露给前端，
//   前端 JS 用 window.chrome.webview.hostObjects.updater_win.checkNow() 等调用。
//
// 依赖（全是系统自带或仓库已有，零新增第三方）：
//   - WinHTTP (winhttp.lib) —— HTTP GET
//   - BCrypt  (bcrypt.lib)  —— SHA256 计算
//   - OpenSSL (XIAOTU_HAS_OPENSSL=1) —— Ed25519 验签（随 core 一起链接）
//   - miniz   (third_party 内嵌)     —— 可选，当前 setup.exe 不需要解压

#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xiaotu::updater {

// 更新状态（给前端读的）
struct Status {
  bool enabled = true;              // 更新总开关
  bool auto_check = true;           // 自动检查
  bool prefetch = true;             // 后台预下载
  int64_t last_check_epoch = 0;     // 上次自动检查时间（UTC 秒）
  int64_t next_check_epoch = 0;     // 下次自动检查时间
  int consecutive_failures = 0;      // 清单获取/校验失败计数（用于退避）

  std::string channel = "stable";
  std::string install_id;           // 随机 UUID，用于灰度分桶
  std::string skipped_version;
  int64_t skip_until_epoch = 0;

  int64_t manifest_build = 0;       // 清单里声明的新版本 build
  std::string manifest_version;     // 清单里的版本号
  std::string manifest_sha256;      // 清单里的 SHA256（小写）
  int64_t manifest_size = 0;        // 清单里的安装包大小

  std::string pending_local_path;   // 后台下载到本地的 setup.exe 路径
  int64_t pending_size = 0;         // 待安装包大小
  bool mandatory = false;
  std::string mandatory_reason;
  std::string mandatory_deadline;

  std::string download_progress;    // "0/1024" 之类
  std::string download_state;       // "IDLE" / "RUNNING" / "DONE" / "FAILED" / "CANCELED" / "NO_PENDING"
};

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

  const Status& status() const { return status_; }

  // JSON 序列化（给前端直接读）
  std::string statusJson() const;

 private:
  UpdaterWin();
  ~UpdaterWin();

  // 启动 30 秒后首次自动检查，之后 24h±2h
  void scheduleInitialCheck();
  void autoCheckLoop();

  // 后台拉清单（WinHTTP），成功返回响应体
  bool fetchManifest(const std::string& url, std::string& out_body,
                     std::string& err_code, std::string& err_detail);

  // 用 BCrypt 算 SHA256（返回小写 hex）
  static std::string sha256File(const std::wstring& path);

  // 持久化 status_ 到 %LOCALAPPDATA%\com.simem.xiaotu\updater.json
  void persistStatus();
  void loadStatus();

  std::string manifest_url_;
  Status status_;
  mutable std::mutex status_mutex_;

  std::thread auto_check_thread_;
  std::atomic<bool> auto_check_stop_{false};

  std::thread download_thread_;
  std::atomic<bool> download_cancel_{false};
  std::atomic<bool> download_running_{false};
};

}  // namespace xiaotu::updater
