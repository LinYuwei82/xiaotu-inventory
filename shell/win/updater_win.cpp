// 小兔进销存 Windows 桌面壳 —— 自动更新桥接层（P1：core/update_engine 薄端口）
//
// 实现（见 docs/更新模块拆分重构方案.md §5）：
//   状态机与五条内置规则全部在 core/update_engine；本文件只提供：
//   - WinTransport：WinHTTP 拉清单 / 下载（写 .part → 进度回调 → 原子改名）
//   - WinHasher：BCrypt SHA256（小写 hex）
//   - WinInstaller（mote 模式，无协助器）：安装版 ShellExecuteW 静默跑 setup.exe
//     （/RELAUNCH=1 替换与重启交给 Inno），壳 PostMessage WM_CLOSE 优雅退出
//     （shutdown_child 经 Job Object 带走 server 树）；绿色版 explorer /select
//     打开 zip 所在目录返回 MANUAL，用户手动解压覆盖。
//     安装版/绿色版由 HKCU 卸载键判定形态，引擎侧 prefer_portable_artifact 透传。
//   - SystemClock：UTC 秒
//   - WinStore：读写 %LOCALAPPDATA%\com.simem.xiaotu\updater_win.json，
//     检测旧格式键（download_state / pending_local_path / download_progress）
//     时按 §5.3 一次性迁移为引擎 schema 并回写（输入侧兼容，1.1 删除）
//   桥方法按 §5.4 映射旧返回码；statusJson 直接输出引擎 status_json（§2.4，P3）。

#include "updater_win.h"

#include <windows.h>

#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <vector>

#include "xiaotu/update.h"
#include "xiaotu/update_engine.h"
#include "xiaotu/log.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fs = std::filesystem;

namespace xiaotu::updater {

// ---- 二进制内构建号标记（供 guard.py 二次校验）
//
// 发布脚本 publish_windows.ps1 从 CMakeCache 读出 XIAOTU_BUILD 写入清单，
// guard.py 上传前扫描绿色包 / setup.exe 里的 EXE 字节，比对清单 build 与
// 本机构建号一致——防止脚本读错 CMakeCache 导致同版本反复提示。
// 这里放一条不会被 LTO 扔掉的静态字符串 "XIAOTU_BUILD=<digits>"。
#ifndef XIAOTU_BUILD_STR
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define XIAOTU_BUILD_STR STRINGIFY(XIAOTU_BUILD)
#endif
namespace {
constexpr const char kBuildStamp[] = "XIAOTU_BUILD=" XIAOTU_BUILD_STR;
constexpr const char *const kBuildStampRef = kBuildStamp;  // 防优化器丢弃
}

// ---- 辅助工具 ----

static std::wstring exeDirectory() {
  wchar_t buf[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return L"";
  std::wstring path(buf, len);
  auto pos = path.find_last_of(L"\\");
  if (pos == std::wstring::npos) return L"";
  return path.substr(0, pos);
}

static std::wstring appDataDir() {
  wchar_t buf[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring base = (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : exeDirectory();
  std::wstring dir = base + L"\\com.simem.xiaotu";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

static std::wstring downloadDir() {
  std::wstring dir = appDataDir() + L"\\downloads";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

// 安装版/绿色版判定：Inno 安装必写 HKCU\...\Uninstall\{AppId}_is1，
// 绿色版没有卸载键。AppId 与 packaging/win/xiaotu-inventory.iss 同源。
static bool isInstalledEdition() {
  HKEY key = nullptr;
  const std::wstring sub =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
      L"{6E2A9B4D-5C1F-4F3B-9D2E-7A8B1C3D5E6F}_is1";
  const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &key);
  if (rc == ERROR_SUCCESS) RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

// 读 UTF-8 文件到字符串；失败返回空串（调用方按「无结果」处理）。
static std::string readFileUtf8(const std::wstring& path) {
  FILE* f = nullptr;
  _wfopen_s(&f, path.c_str(), L"rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(f);
    return {};
  }
  std::string text(static_cast<std::size_t>(sz), '\0');
  const size_t got = fread(text.data(), 1, text.size(), f);
  fclose(f);
  text.resize(got);
  return text;
}

static std::string generateUuid() {
  // 简单随机 UUID（install_id 只用于灰度分桶，不需要真正 GUID）
  static thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<uint32_t> dist;
  char buf[37];
  snprintf(buf, sizeof(buf),
           "%08x-%04x-%04x-%04x-%08x%04x",
           dist(gen), dist(gen) & 0xffff, (dist(gen) & 0x0fff) | 0x4000,
           (dist(gen) & 0x3fff) | 0x8000, dist(gen), dist(gen) & 0xffff);
  return buf;
}

static std::int64_t epochNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---- WinHTTP 拉清单/下载 ----

static bool winhttpGet(const std::string& url, std::string& out_body,
                       std::string& err_code, std::string& err_detail,
                       std::int64_t* total_bytes = nullptr,
                       std::function<bool(const std::string& chunk)> on_chunk = nullptr) {
  // url 拆 host + path
  std::string host = url;
  auto pos = host.find("://");
  if (pos != std::string::npos) host = host.substr(pos + 3);
  std::string path = "/";
  auto slash = host.find('/');
  if (slash != std::string::npos) {
    path = host.substr(slash);
    host = host.substr(0, slash);
  }
  bool is_https = (url.find("https://") == 0);
  std::wstring host_w(host.begin(), host.end());
  std::wstring path_w(path.begin(), path.end());

  HINTERNET session = WinHttpOpen(L"XiaotuShell/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    err_code = "HTTP_OPEN_FAILED";
    err_detail = std::to_string(GetLastError());
    return false;
  }
  HINTERNET connect = WinHttpConnect(session, host_w.c_str(), is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    err_code = "HTTP_CONNECT_FAILED";
    err_detail = std::to_string(GetLastError());
    return false;
  }
  HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_w.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         is_https ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    err_code = "HTTP_REQUEST_FAILED";
    err_detail = std::to_string(GetLastError());
    return false;
  }

  bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
         && WinHttpReceiveResponse(request, nullptr);

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &status_size, WINHTTP_NO_HEADER_INDEX);

  if (!ok || status >= 400) {
    char buf[128];
    snprintf(buf, sizeof(buf), "HTTP %lu", status);
    err_code = "HTTP_ERROR";
    err_detail = buf;
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  // 读 Content-Length（可选）
  DWORD content_len = 0;
  DWORD cl_size = sizeof(content_len);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &content_len, &cl_size, WINHTTP_NO_HEADER_INDEX);
  if (total_bytes) *total_bytes = content_len;

  std::vector<char> chunk_buf(64 * 1024);
  bool read_failed = false;
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(request, chunk_buf.data(), static_cast<DWORD>(chunk_buf.size()), &got)) {
      // 读失败不能当成正常结束：否则会写出截断的安装包，
      // 最后以 SHA256 不符收场，掩盖真正的网络错误。
      read_failed = true;
      break;
    }
    if (got == 0) break;
    if (on_chunk) {
      if (!on_chunk(std::string(chunk_buf.data(), got))) {
        // 取消
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        err_code = "CANCELED";
        err_detail = "";
        return false;
      }
    }
    out_body.append(chunk_buf.data(), got);
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  if (read_failed) {
    err_code = "HTTP_READ_FAILED";
    err_detail = std::to_string(GetLastError());
    return false;
  }
  return true;
}

// ---- BCrypt SHA256 ----

static std::string sha256File(const std::wstring& path) {
  BCRYPT_ALG_HANDLE algo = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  BYTE* hash_out = nullptr;
  DWORD hash_len = 0;
  std::string result;

  if (BCryptOpenAlgorithmProvider(&algo, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
  DWORD cb = 0;
  // 拿不到摘要长度就不能继续，否则会按 0 字节算出一个空哈希
  if (BCryptGetProperty(algo, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len),
                        sizeof(hash_len), &cb, 0) != 0 || hash_len == 0) {
    BCryptCloseAlgorithmProvider(algo, 0);
    return "";
  }
  if (BCryptCreateHash(algo, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(algo, 0);
    return "";
  }

  FILE* f = nullptr;
  _wfopen_s(&f, path.c_str(), L"rb");
  if (!f) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algo, 0);
    return "";
  }
  std::vector<char> buf(64 * 1024);
  size_t n = 0;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
  }
  fclose(f);

  hash_out = new BYTE[hash_len];
  BCryptFinishHash(hash, hash_out, hash_len, 0);

  char hex[65];
  for (DWORD i = 0; i < hash_len; ++i) snprintf(hex + i * 2, 3, "%02x", hash_out[i]);
  result = hex;

  delete[] hash_out;
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algo, 0);
  return result;
}

// ---- 平台端口 + 引擎（方案 §5.1） ----

struct UpdaterWin::Impl
    : xiaotu::ITransport,
      xiaotu::IHasher,
      xiaotu::IInstaller,
      xiaotu::IClock,
      xiaotu::IStore {
  std::unique_ptr<xiaotu::UpdateEngine> engine;

  static std::wstring statusPath() { return appDataDir() + L"\\updater_win.json"; }

  // ITransport::fetch —— 拉清单；错误串沿用旧壳 "CODE:detail" 口径
  bool fetch(const std::string& url, std::string& body, std::string& err) override {
    std::string code, detail;
    if (winhttpGet(url, body, code, detail)) return true;
    err = code + (detail.empty() ? "" : ":" + detail);
    return false;
  }

  // ITransport::download —— 写 dest.part → 进度回调 → 原子改名；
  // 失败/取消留下的半截文件由引擎统一删除（规则 3）。
  bool download(const std::string& url, const fs::path& dest,
                const xiaotu::ProgressFn& on_progress, const xiaotu::CancelFn& cancel,
                std::string& err) override {
    const std::wstring dest_w = dest.native();
    const std::wstring part_w = dest_w + L".part";
    FILE* f = nullptr;
    _wfopen_s(&f, part_w.c_str(), L"wb");
    if (!f) {
      err = "FILE_OPEN";
      return false;
    }
    std::int64_t downloaded = 0;
    std::int64_t total = 0;
    std::string body_unused;  // 逐块写文件，不累积
    std::string code, detail;
    const auto on_chunk = [&](const std::string& chunk) -> bool {
      if (cancel && cancel()) return false;
      fwrite(chunk.data(), 1, chunk.size(), f);
      downloaded += static_cast<std::int64_t>(chunk.size());
      if (on_progress) on_progress(downloaded, total);
      return true;
    };
    bool ok = winhttpGet(url, body_unused, code, detail, &total, on_chunk);
    fclose(f);
    if (!ok) {
      _wremove(part_w.c_str());
      err = code + (detail.empty() ? "" : ":" + detail);
      return false;
    }
    _wremove(dest_w.c_str());
    if (_wrename(part_w.c_str(), dest_w.c_str()) != 0) {
      err = "RENAME_FAILED";
      return false;
    }
    return true;
  }

  // IHasher::sha256_file
  bool sha256_file(const fs::path& p, std::string& hex, std::string& err) override {
    hex = sha256File(p.native());
    if (hex.empty()) {
      err = "SHA256_COMPUTE_FAILED";
      return false;
    }
    return true;
  }

  // IInstaller::install —— mote 模式（无协助器，参考 mote/shell/win/updater_win.cpp）：
  // 安装版：ShellExecuteW 直接静默跑 setup.exe，文件替换与重启全部外包给
  //   Inno Setup（/CLOSEAPPLICATIONS 释放文件锁、/RELAUNCH=1 装完自动拉起新壳）。
  //   返回 STARTED 后壳 PostMessage WM_CLOSE 优雅退出（Job Object 带走 server 树）。
  // 绿色版：explorer /select 打开更新包所在目录，返回 MANUAL——用户关闭本程序后
  //   手动解压覆盖，重开即完成更新。引擎 load() 会把 INSTALLING 回退 DONE、
  //   pending 保留，覆盖前重开仍会继续提示待安装。
  xiaotu::InstallResult install(const xiaotu::PendingPackage& pkg) override {
    const std::wstring path = pkg.path.native();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return {false, "FAILED", "PENDING_MISSING"};
    }
    const bool is_zip = path.size() >= 4 &&
                        _wcsicmp(path.c_str() + path.size() - 4, L".zip") == 0;
    if (is_zip) {
      // 绿色版：只打开目录并选中 zip，不做自动替换（避免重新引入协助器）。
      const std::wstring params = L"/select,\"" + path + L"\"";
      const auto rc = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                    params.c_str(), nullptr, SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        return {false, "FAILED", "OPEN_FAILED=" + std::to_string(reinterpret_cast<INT_PTR>(rc))};
      }
      return {true, "MANUAL", ""};
    }
    // 安装版：静默安装，替换+重启交给 Inno。
    const auto rc = ShellExecuteW(
        nullptr, L"open", path.c_str(),
        L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1",
        nullptr, SW_SHOW);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
      return {false, "FAILED", "SHELLEXEC_FAILED=" + std::to_string(reinterpret_cast<INT_PTR>(rc))};
    }
    return {true, "STARTED", ""};
  }

  // IClock::now_epoch
  std::int64_t now_epoch() const override { return epochNow(); }

  // IStore::load —— 读 updater_win.json；旧格式键按 §5.3 一次性迁移为引擎 schema。
  // 迁移结果回写由引擎 load() 末尾的 persist 完成（新文件不再有旧键）。
  bool load(std::string& out_json, std::string& err) override {
    FILE* f = nullptr;
    _wfopen_s(&f, statusPath().c_str(), L"rb, ccs=UTF-8");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    std::string text(static_cast<std::size_t>(sz), '\0');
    const size_t got = fread(text.data(), 1, text.size(), f);
    fclose(f);
    text.resize(got);
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(text);
    } catch (...) {
      err = "STATUS_FILE_CORRUPT";
      return false;  // 损坏文件按空状态起步，下次 persist 覆盖
    }
    if (!j.is_object()) return false;
    // 旧格式判定：有 download_state、没有引擎的 state 键。
    if (!j.contains("state") && j.contains("download_state")) {
      std::string ds;
      if (j.contains("download_state") && j["download_state"].is_string())
        ds = j["download_state"].get<std::string>();
      const char* state = "IDLE";
      if (ds == "RUNNING") state = "DOWNLOADING";
      else if (ds == "DONE") state = "DONE";
      else if (ds == "FAILED") state = "FAILED";
      j["state"] = state;
      if (j.contains("pending_local_path") && j["pending_local_path"].is_string())
        j["pending_path"] = j["pending_local_path"];
      // download_progress "a/b (x%)" → bytes_done / bytes_total；
      // DONE 态旧壳不落盘进度，按 pending_size 补齐（避免重启后显示 0 B）。
      long long done = 0, total = 0;
      if (j.contains("download_progress") && j["download_progress"].is_string()) {
#pragma warning(push)
#pragma warning(disable : 4996)  // 输入为自己读回的 JSON 字符串，sscanf 无越界风险
        sscanf(j["download_progress"].get<std::string>().c_str(), "%lld/%lld", &done, &total);
#pragma warning(pop)
      }
      std::int64_t pending_size = 0;
      if (j.contains("pending_size") && j["pending_size"].is_number_integer())
        pending_size = j["pending_size"].get<std::int64_t>();
      if (std::string(state) == "DONE" && pending_size > 0) { done = pending_size; total = pending_size; }
      if (done > 0) j["bytes_done"] = done;
      if (total > 0) j["bytes_total"] = total;
      j.erase("download_state");
      j.erase("pending_local_path");
      j.erase("download_progress");
    }
    out_json = j.dump();
    return true;
  }

  // IStore::save
  bool save(const std::string& json, std::string& err) override {
    FILE* f = nullptr;
    _wfopen_s(&f, statusPath().c_str(), L"wb, ccs=UTF-8");
    if (!f) {
      err = "STATUS_FILE_WRITE_FAILED";
      return false;
    }
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    return true;
  }
};

// ---- UpdaterWin 单例 ----

UpdaterWin::UpdaterWin() : impl_(std::make_unique<Impl>()) {
  xiaotu::EngineConfig cfg;
  cfg.platform = "windows";
  cfg.channel = "stable";
  // install_id 只用于灰度分桶：先给一个随机值，load() 读到落盘 id 会覆盖，
  // 首次运行生成的这个随引擎首次 persist 固定下来。
  cfg.install_id = generateUuid();
  // 旧壳行为对齐（§5.2）：checkNow 无手动冷却（引擎已移除该机制）；
  // 自动间隔 24h 兜底，实际节奏由本壳 autoCheckLoop 的 24h±2h 抖动 / 失败退避控制；
  // 旧 v2 路径不查 min_os，保持行为不变。
  cfg.auto_check_interval_seconds = 86400;
  cfg.ignore_min_os = true;
  cfg.download_dir = fs::path(downloadDir());
  cfg.public_key_base64 = XIAOTU_UPDATE_PUBKEY;
  // 安装版壳优先 setup，绿色版壳优先 portable.zip（清单缺省回落 setup）。
  cfg.prefer_portable_artifact = !isInstalledEdition();
  if (cfg.public_key_base64.empty())
    log_warn("更新：未配置 XIAOTU_UPDATE_PUBKEY，跳过 Ed25519 验签");

  xiaotu::UpdateEngine::Deps deps;
  deps.transport = impl_.get();
  deps.hasher = impl_.get();
  deps.installer = impl_.get();
  deps.clock = impl_.get();
  deps.store = impl_.get();
  impl_->engine = std::make_unique<xiaotu::UpdateEngine>(deps, std::move(cfg));
  // load() 内含：旧格式迁移、崩溃恢复（下载/安装中断回退）、
  // 已装覆盖的残留 pending 清理（1.0.4「装完仍提示待安装」回归点）。
  impl_->engine->load();
}

UpdaterWin::~UpdaterWin() {
  auto_check_stop_ = true;
  if (auto_check_thread_.joinable()) auto_check_thread_.join();
  if (download_thread_.joinable()) download_thread_.join();
  if (install_thread_.joinable()) install_thread_.join();
}

UpdaterWin& UpdaterWin::instance() {
  static UpdaterWin inst;
  return inst;
}

void UpdaterWin::start(const std::string& manifest_url, const std::string& channel) {
  {
    std::lock_guard<std::mutex> lock(url_mutex_);
    manifest_url_ = manifest_url;
  }
  if (manifest_url.empty()) return;
  impl_->engine->configure(manifest_url, channel);
  // 只启动一个自动检查线程：线程保存在成员里，析构时 join。重复调用 start 不会再起线程。
  if (auto_check_thread_.joinable()) return;
  auto_check_stop_ = false;
  auto_check_thread_ = std::thread([this] { autoCheckLoop(); });
}

void UpdaterWin::autoCheckLoop() {
  // 启动 30s 后首次检查
  for (int i = 0; i < 30 && !auto_check_stop_; ++i)
    std::this_thread::sleep_for(std::chrono::seconds(1));
  while (!auto_check_stop_) {
    std::string url;
    {
      std::lock_guard<std::mutex> lock(url_mutex_);
      url = manifest_url_;
    }
    if (url.empty()) break;

    // 只在 auto_check=true 时自动检查（开关位由引擎落盘，跨重启保持）
    {
      const auto st = impl_->engine->status();
      if (!st.value("auto_check", true) || !st.value("enabled", true)) {
        // 等 1 分钟再看
        for (int i = 0; i < 60 && !auto_check_stop_; ++i)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
    }

    checkNow(nullptr);  // 后台，回调为空

    // 与 macOS 壳 / core 同一口径：失败按 1 分钟 → 10 分钟 → 1 小时退避，
    // 成功一次即回到 24h±2h。否则清单 404 时仍按天重试，界面会长时间停在旧状态。
    const int failures = impl_->engine->status().value("consecutive_failures", 0);
    int wait;
    if (failures > 0) {
      wait = static_cast<int>(xiaotu::check_retry_delay(failures));
    } else {
      int jitter = (rand() % 4 - 2) * 3600;
      wait = 86400 + jitter;
    }
    for (int i = 0; i < wait && !auto_check_stop_; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void UpdaterWin::checkNow(Completion cb) {
  std::string url;
  {
    std::lock_guard<std::mutex> lock(url_mutex_);
    url = manifest_url_;
  }
  if (url.empty()) {
    if (cb) cb("UPDATE_UNAVAILABLE", "未配置清单地址");
    return;
  }

  const bool judged = impl_->engine->check(/*manual=*/true);
  const auto st = impl_->engine->status();
  const std::string state = st.value("state", "");
  const std::string last_result = st.value("last_result", "");
  const std::string last_error = st.value("last_error", "");

  if (judged) {
    // 判定码映射（§5.4）：可用更新回版本号；其余「无更新」类（含已跳过、
    // 灰度未命中、通道不符）统一 NO_UPDATE，与旧壳前端文案一致。
    if (last_result == "UPDATE_AVAILABLE") {
      // 与 macOS 壳对齐：prefetch 开启时检出更新即后台预取（无头会话/无人
      // 点面板也能走通 下载→验签→pending 链路），安装仍须用户显式确认。
      // downloadUpdate 自带并发与状态防护，重复触发返回 ALREADY_RUNNING。
      if (st.value("prefetch", true)) downloadUpdate(nullptr);
      if (cb) cb("UPDATE_AVAILABLE", st.value("version", ""));
    } else {
      if (cb) cb("NO_UPDATE", "");
    }
    return;
  }

  // 未得到判定：下载/安装进行中（引擎拒绝重入）按无更新回，不改写检查记录。
  if (state == "DOWNLOADING" || state == "INSTALLING") {
    if (cb) cb("NO_UPDATE", "");
    return;
  }

  // 检查失败：把引擎错误串映射回旧壳返回码（§5.4）。
  std::string code;
  if (last_error == "MANIFEST_JSON_INVALID") {
    code = "MANIFEST_PARSE_ERROR";
  } else if (last_error == "MANIFEST_SIGNATURE_INVALID") {
    code = "SIGNATURE_INVALID";
  } else if (last_error.rfind("MANIFEST_", 0) == 0) {
    code = "MANIFEST_INVALID";  // parse_manifest 的稳定错误码
  } else {
    code = "FETCH_FAILED";  // HTTP_* 等传输错误
  }
  if (cb) cb(code, last_error);
}

void UpdaterWin::downloadUpdate(Completion cb) {
  if (download_running_) {
    // 已有下载在跑：前端应继续等 downloadUpdate 完成事件，不要重复发起。
    if (cb) cb("ALREADY_RUNNING", "");
    return;
  }
  // 上一轮线程已结束但还没回收：先 join，再复用成员（不能 detach，否则析构时可能访问已销毁的 this）。
  if (download_thread_.joinable()) download_thread_.join();

  const auto st = impl_->engine->status();
  const std::string state = st.value("state", "IDLE");
  // 旧壳口径：清单没给出待更新包（sha/url 缺失、无待更新版本）→ NO_PENDING_UPDATE。
  if (state != "UPDATE_AVAILABLE" && state != "FAILED" && state != "DONE") {
    log_warn("更新：下载被拒，没有待更新版本");
    if (cb) cb("NO_PENDING_UPDATE", "");
    return;
  }
  if (st.value("manifest_setup_url", "").empty() || st.value("manifest_sha256", "").empty()) {
    if (cb) cb("NO_PENDING_UPDATE", "");
    return;
  }

  download_running_ = true;
  download_thread_ = std::thread([this, cb] {
    const bool ok = impl_->engine->download_pending();
    download_running_ = false;
    const auto st2 = impl_->engine->status();
    const std::string last_result = st2.value("last_result", "");
    if (ok) {
      log_info("更新：下载完成并通过 SHA256 校验");
      if (cb) cb("DOWNLOADED", "");
    } else if (last_result == "CANCELED") {
      if (cb) cb("CANCELED", "");
    } else {
      log_warn("更新：下载失败 " + st2.value("last_error", ""));
      if (cb) cb("FAILED", st2.value("last_error", ""));
    }
  });
}

void UpdaterWin::cancelPrefetch(Completion cb) {
  if (!download_running_) {
    if (cb) cb("NOT_RUNNING", "");
    return;
  }
  // 引擎置取消标志，transport 的分块回调读到后中断；半截文件由引擎删除。
  impl_->engine->cancel_download();
  if (cb) cb("CANCELED", "");
}

void UpdaterWin::installUpdate(Completion cb) {
  // 与 macOS（Sparkle）单入口对齐：界面上不再单独提供「下载并校验」，
  // 包还没下载（预取关闭、下载失败或还没轮到）时，这里先补下载再安装。
  const auto st = impl_->engine->status();
  const std::string state = st.value("state", "IDLE");
  const bool not_ready =
      (state == "UPDATE_AVAILABLE" || state == "FAILED") &&
      st.value("pending_path", "").empty();
  if (!not_ready) {
    start_install(std::move(cb));
    return;
  }
  if (install_thread_.joinable()) install_thread_.join();
  install_thread_ = std::thread([this, cb] {
    // 后台预取正在跑：等它结束，成功就直接用它的产物，不再重复下载。
    while (download_running_) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool have_pkg = !impl_->engine->status().value("pending_path", "").empty();
    if (have_pkg || impl_->engine->download_pending()) {
      start_install(cb);
      return;
    }
    const auto s = impl_->engine->status();
    const std::string lr = s.value("last_result", "");
    log_warn("更新：安装前补下载失败 " + s.value("last_error", ""));
    if (cb) cb(lr == "CANCELED" ? "CANCELED" : "FAILED", s.value("last_error", ""));
  });
}

void UpdaterWin::start_install(Completion cb) {
  // 引擎在进入 INSTALLING 前强制重校验大小 + SHA256（规则 2）：
  // 下载完成后文件可能被杀软隔离、截断或清理，安装器带 /CLOSEAPPLICATIONS，
  // 一旦拉起就会先关掉本程序——坏包照跑就表现为「点安装直接闪退、回不来」。
  const auto res = impl_->engine->try_install();
  if (res.started) {
    // MANUAL=绿色版已打开目录待用户手动覆盖；STARTED=setup 已启动静默安装。
    log_info("更新：安装流程已启动 code=" + res.code);
    if (cb) cb(res.code, "");
    return;
  }
  if (res.code == "NO_PENDING_UPDATE") {
    if (cb) cb("NO_PENDING_UPDATE", "");
  } else if (res.code == "VERIFY_FAILED") {
    log_warn("更新：安装前校验失败，已丢弃坏包 " + res.error);
    if (cb) cb("NO_PENDING_UPDATE", "PACKAGE_INVALID");
  } else if (res.code == "UNAVAILABLE") {
    if (cb) cb("UPDATE_UNAVAILABLE", res.error);
  } else {
    log_warn("更新：启动安装程序失败 " + res.error);
    if (cb) cb("OPEN_FAILED", res.error);
  }
}

void UpdaterWin::setOptions(bool auto_check, bool prefetch, Completion cb) {
  // enabled 恒为 true：旧壳没有关总开关的入口，start() 即启用。
  impl_->engine->set_options(true, auto_check, prefetch);
  if (cb) cb("UPDATED", "");
}

std::string UpdaterWin::statusJson() const {
  // P3 起直接输出引擎统一 status_json（方案 §2.4）：
  // 旧壳键 download_state/download_progress/pending_local_path 的适配已删除，
  // 前端 normalizeUpdateStatus 按引擎字段归一。
  return impl_->engine->status_json();
}

void UpdaterWin::checkMinSupportedVersion(Completion cb) {
  // 简单实现：当前 shell 不直接查 health endpoint；让前端调壳内服务端的 /api/health
  // 或在 shell 里加一个直连。暂留 TODO。
  if (cb) cb("UNAVAILABLE", "checkMinSupportedVersion 未实现");
}

std::string UpdaterWin::installResultJson() const {
  // 读取协助器（1.0.19–1.0.23，已随协助器链路删除）留下的
  // apply-state.json + install-result.json，仅用于旧壳升级上来后的
  // 「更新成功」一次性提示；新壳安装不再产生这两份文件。1.1 删除本读取。
  // interrupted：apply-state 停在 installing 超过 600s（协助器/安装器中途没命）。
  const std::wstring updates = appDataDir() + L"\\updates";
  nlohmann::json out = {{"apply", nullptr}, {"result", nullptr}, {"interrupted", false}};
  const std::string apply_text = readFileUtf8(updates + L"\\apply-state.json");
  if (!apply_text.empty()) {
    auto j = nlohmann::json::parse(apply_text, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
      std::int64_t age = 0;
      HANDLE h = CreateFileW((updates + L"\\apply-state.json").c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        FILETIME write_time;
        if (GetFileTime(h, nullptr, nullptr, &write_time)) {
          FILETIME now_ft;
          GetSystemTimeAsFileTime(&now_ft);
          ULARGE_INTEGER now_u, then_u;
          now_u.LowPart = now_ft.dwLowDateTime;
          now_u.HighPart = now_ft.dwHighDateTime;
          then_u.LowPart = write_time.dwLowDateTime;
          then_u.HighPart = write_time.dwHighDateTime;
          age = static_cast<std::int64_t>((now_u.QuadPart - then_u.QuadPart) / 10000000);
        }
        CloseHandle(h);
      }
      j["age_seconds"] = age;
      out["apply"] = j;
      if (j.value("state", "") == "installing" && age > 600) out["interrupted"] = true;
    }
  }
  const std::string result_text = readFileUtf8(updates + L"\\install-result.json");
  if (!result_text.empty()) {
    auto j = nlohmann::json::parse(result_text, nullptr, false);
    if (!j.is_discarded() && j.is_object()) out["result"] = j;
  }
  return out.dump();
}

void UpdaterWin::ackInstallResult() {
  // 一次性展示：删掉两份文件即完成 ack。
  const std::wstring updates = appDataDir() + L"\\updates";
  DeleteFileW((updates + L"\\install-result.json").c_str());
  DeleteFileW((updates + L"\\apply-state.json").c_str());
}

}  // namespace xiaotu::updater
