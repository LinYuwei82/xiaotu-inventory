// 小兔进销存 Windows 桌面壳 —— 自动更新桥接层
//
// 实现：
//   WinHTTP 拉 manifest.json（Schema v2 统一清单）→ nlohmann::json 解析
//   → Ed25519 验签（OpenSSL，覆盖整个清单对象）→ 取 platforms.windows 判定
//   → WinHTTP 拉 artifacts.setup 安装包 → BCrypt SHA256 校验
//   → ShellExecute 跑 ISCC /VERYSILENT → ISCC 自动关壳、覆盖、启新壳

#include "updater_win.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>

#include "xiaotu/update.h"
#include "xiaotu/update_signature.h"
#include "xiaotu/log.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fs = std::filesystem;

namespace xiaotu::updater {

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
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(request, chunk_buf.data(), static_cast<DWORD>(chunk_buf.size()), &got) || got == 0) break;
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
  return true;
}

// ---- BCrypt SHA256 ----

std::string UpdaterWin::sha256File(const std::wstring& path) {
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

// ---- v2 统一清单解析 ----
//
// manifest.json 是三平台共用的统一清单：签名覆盖整个对象，客户端只取
// platforms.windows 与其中需要的键；未知字段、未知 artifacts key 一律忽略。
// 安装版读 artifacts.setup（绿色包 artifacts.portable 由下载页消费）。

namespace {

struct V2Windows {
  std::string version;
  std::int64_t build = 0;
  std::string setup_url;
  std::string setup_sha256;
  std::int64_t setup_size = 0;
  int rollout_percent = 100;
  bool mandatory = false;
  std::string mandatory_reason;
  std::string mandatory_deadline;
};

// 非抛出的字段读取：类型不对视为缺省（未知/缺省一律忽略）。
bool v2_read_string(const nlohmann::json& object, const char* key, std::string& out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) return false;
  out = it->get<std::string>();
  return true;
}

bool v2_read_int(const nlohmann::json& object, const char* key, std::int64_t& out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_integer()) return false;
  out = it->get<std::int64_t>();
  return true;
}

bool v2_read_bool(const nlohmann::json& object, const char* key, bool& out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_boolean()) return false;
  out = it->get<bool>();
  return true;
}

// 提取 platforms.windows；失败时 err_code 给出稳定错误码。
bool parse_v2_windows(const nlohmann::json& manifest, V2Windows& out, std::string& err_code) {
  const auto platforms = manifest.find("platforms");
  if (platforms == manifest.end() || !platforms->is_object()) {
    err_code = "PLATFORMS_MISSING";
    return false;
  }
  const auto windows = platforms->find("windows");
  if (windows == platforms->end() || !windows->is_object()) {
    err_code = "PLATFORM_WINDOWS_MISSING";
    return false;
  }
  const auto artifacts = windows->find("artifacts");
  if (artifacts == windows->end() || !artifacts->is_object() ||
      artifacts->find("setup") == artifacts->end() || !(*artifacts)["setup"].is_object()) {
    err_code = "ARTIFACT_SETUP_MISSING";
    return false;
  }
  const nlohmann::json& setup = (*artifacts)["setup"];

  // 必填键：version / build / artifacts.setup 的 url、sha256、size。
  if (!v2_read_string(*windows, "version", out.version) ||
      !v2_read_int(*windows, "build", out.build) ||
      !v2_read_string(setup, "url", out.setup_url) ||
      !v2_read_string(setup, "sha256", out.setup_sha256) ||
      !v2_read_int(setup, "size", out.setup_size)) {
    err_code = "FIELD_MISSING_OR_INVALID";
    return false;
  }
  // 可选键：rollout_percent 缺省全量，mandatory 缺省 false。
  std::int64_t rollout = 100;
  if (v2_read_int(*windows, "rollout_percent", rollout) &&
      rollout >= 1 && rollout <= 100) {
    out.rollout_percent = static_cast<int>(rollout);
  }
  v2_read_bool(*windows, "mandatory", out.mandatory);
  v2_read_string(*windows, "mandatory_reason", out.mandatory_reason);
  v2_read_string(*windows, "mandatory_deadline", out.mandatory_deadline);
  return true;
}

}  // namespace

// ---- UpdaterWin 单例 ----

UpdaterWin::UpdaterWin() {
  loadStatus();
  if (status_.install_id.empty()) status_.install_id = generateUuid();
  if (status_.channel.empty()) status_.channel = "stable";
}

UpdaterWin::~UpdaterWin() {
  auto_check_stop_ = true;
  if (auto_check_thread_.joinable()) auto_check_thread_.join();
  download_cancel_ = true;
  if (download_thread_.joinable()) download_thread_.join();
}

UpdaterWin& UpdaterWin::instance() {
  static UpdaterWin inst;
  return inst;
}

void UpdaterWin::persistStatus() {
  nlohmann::json j;
  j["enabled"] = status_.enabled;
  j["auto_check"] = status_.auto_check;
  j["prefetch"] = status_.prefetch;
  j["last_check_epoch"] = status_.last_check_epoch;
  j["install_id"] = status_.install_id;
  j["channel"] = status_.channel;
  j["skipped_version"] = status_.skipped_version;
  j["skip_until_epoch"] = status_.skip_until_epoch;
  j["pending_local_path"] = status_.pending_local_path;
  j["pending_size"] = status_.pending_size;
  std::wstring path = appDataDir() + L"\\updater_win.json";
  FILE* f = nullptr;
  _wfopen_s(&f, path.c_str(), L"wb, ccs=UTF-8");
  if (f) {
    auto text = j.dump(2);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
  }
}

void UpdaterWin::loadStatus() {
  std::wstring path = appDataDir() + L"\\updater_win.json";
  FILE* f = nullptr;
  _wfopen_s(&f, path.c_str(), L"rb, ccs=UTF-8");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return; }
  std::string text(sz, '\0');
  fread(text.data(), 1, sz, f);
  fclose(f);
  try {
    auto j = nlohmann::json::parse(text);
    if (j.contains("enabled")) status_.enabled = j["enabled"];
    if (j.contains("auto_check")) status_.auto_check = j["auto_check"];
    if (j.contains("prefetch")) status_.prefetch = j["prefetch"];
    if (j.contains("last_check_epoch")) status_.last_check_epoch = j["last_check_epoch"];
    if (j.contains("install_id")) status_.install_id = j["install_id"];
    if (j.contains("channel")) status_.channel = j["channel"];
    if (j.contains("skipped_version")) status_.skipped_version = j["skipped_version"];
    if (j.contains("skip_until_epoch")) status_.skip_until_epoch = j["skip_until_epoch"];
    if (j.contains("pending_local_path")) status_.pending_local_path = j["pending_local_path"];
    if (j.contains("pending_size")) status_.pending_size = j["pending_size"];
  } catch (...) { /* ignore corrupt */ }
}

std::string UpdaterWin::statusJson() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  nlohmann::json j;
  j["enabled"] = status_.enabled;
  j["auto_check"] = status_.auto_check;
  j["prefetch"] = status_.prefetch;
  j["channel"] = status_.channel;
  j["last_check_epoch"] = status_.last_check_epoch;
  j["next_check_epoch"] = status_.next_check_epoch;
  j["manifest_version"] = status_.manifest_version;
  j["manifest_build"] = status_.manifest_build;
  j["manifest_sha256"] = status_.manifest_sha256;
  j["manifest_size"] = status_.manifest_size;
  j["pending_local_path"] = status_.pending_local_path;
  j["pending_size"] = status_.pending_size;
  j["mandatory"] = status_.mandatory;
  j["mandatory_reason"] = status_.mandatory_reason;
  j["mandatory_deadline"] = status_.mandatory_deadline;
  j["download_state"] = status_.download_state;
  j["download_progress"] = status_.download_progress;
  j["consecutive_failures"] = status_.consecutive_failures;
  return j.dump();
}

void UpdaterWin::start(const std::string& manifest_url, const std::string& channel) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    manifest_url_ = manifest_url;
    if (!channel.empty()) status_.channel = channel;
    if (manifest_url.empty()) return;
    status_.enabled = true;
    persistStatus();
  }
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
      std::lock_guard<std::mutex> lock(status_mutex_);
      url = manifest_url_;
    }
    if (url.empty()) break;

    // 只在 auto_check=true 时自动检查
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!status_.auto_check || !status_.enabled) {
        // 等 1 分钟再看
        for (int i = 0; i < 60 && !auto_check_stop_; ++i)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
    }

    checkNow(nullptr);  // 后台，回调为空

    // 与 macOS 壳 / core 同一口径：失败按 1 分钟 → 10 分钟 → 1 小时退避，
    // 成功一次即回到 24h±2h。否则清单 404 时仍按天重试，界面会长时间停在旧状态。
    int failures = 0;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      failures = status_.consecutive_failures;
    }
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

bool UpdaterWin::fetchManifest(const std::string& url, std::string& out_body,
                               std::string& err_code, std::string& err_detail) {
  return winhttpGet(url, out_body, err_code, err_detail);
}

void UpdaterWin::checkNow(Completion cb) {
  if (manifest_url_.empty()) {
    if (cb) cb("UPDATE_UNAVAILABLE", "未配置清单地址");
    return;
  }

  std::string body, err_code, err_detail;
  if (!fetchManifest(manifest_url_, body, err_code, err_detail)) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.consecutive_failures++;
      persistStatus();
    }
    if (cb) cb("FETCH_FAILED", err_code + ":" + err_detail);
    return;
  }

  // 解析 + Ed25519 验签（trust anchor 公钥由 CMake 编译期注入 XIAOTU_UPDATE_PUBKEY）
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.consecutive_failures++;
      persistStatus();
    }
    if (cb) cb("MANIFEST_PARSE_ERROR", e.what());
    return;
  }

  if (!std::string(XIAOTU_UPDATE_PUBKEY).empty()) {
    if (!xiaotu::verify_manifest_signature(j, XIAOTU_UPDATE_PUBKEY)) {
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.consecutive_failures++;
        persistStatus();
      }
      if (cb) cb("SIGNATURE_INVALID", "");
      return;
    }
  } else {
    log_warn("更新：未配置 XIAOTU_UPDATE_PUBKEY，跳过 Ed25519 验签");
  }

  // v2：channel 在顶层；platform 由只取 platforms.windows 隐含，无需再比。
  std::string manifest_channel;
  if (!v2_read_string(j, "channel", manifest_channel) || manifest_channel != status_.channel) {
    if (cb) cb("NO_UPDATE", "channel mismatch");
    return;
  }

  V2Windows pw;
  std::string pw_err;
  if (!parse_v2_windows(j, pw, pw_err)) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.consecutive_failures++;
      persistStatus();
    }
    if (cb) cb("MANIFEST_INVALID", pw_err);
    return;
  }

  // 更新判定（v2 规则）：先比 version；同版本再比 build，build <= 本机 → 无更新。
  xiaotu::ProductVersion remote_version;
  xiaotu::ProductVersion local_version;
  if (!xiaotu::parse_product_version(pw.version, remote_version) ||
      !xiaotu::parse_product_version(xiaotu::current_product_version(), local_version)) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.consecutive_failures++;
      persistStatus();
    }
    if (cb) cb("MANIFEST_INVALID", "MANIFEST_VERSION_INVALID");
    return;
  }
  const int version_cmp = xiaotu::compare_product_version(remote_version, local_version);
  bool update_available =
      version_cmp > 0 || (version_cmp == 0 && pw.build > current_build_number());

  if (update_available && !pw.mandatory) {
    // 灰度分桶与跳过记录沿用 core 同一规则；强制更新不受两者限制。
    if (!xiaotu::rollout_allows(pw.rollout_percent,
                                xiaotu::rollout_bucket(status_.install_id, pw.build))) {
      update_available = false;
    } else if (status_.skipped_version == pw.version &&
               epochNow() < status_.skip_until_epoch) {
      update_available = false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.consecutive_failures = 0;
    status_.last_check_epoch = epochNow();
    if (update_available) {
      status_.manifest_version = pw.version;
      status_.manifest_build = pw.build;
      status_.manifest_sha256 = pw.setup_sha256;
      status_.manifest_size = pw.setup_size;
      status_.mandatory = pw.mandatory;
      status_.mandatory_reason = pw.mandatory_reason;
      status_.mandatory_deadline = pw.mandatory_deadline;
    } else {
      // 无更新（含已跳过、灰度未命中、通道不符）：必须清掉上一次的待更新信息，
      // 否则前端按 manifest_version 判定会把“已是最新”误报成“发现新版本”，
      // 且残留的 mandatory 会让用户反复看到强制升级弹窗。
      status_.manifest_version.clear();
      status_.manifest_build = 0;
      status_.manifest_sha256.clear();
      status_.manifest_size = 0;
      status_.mandatory = false;
      status_.mandatory_reason.clear();
      status_.mandatory_deadline.clear();
      status_.pending_local_path.clear();
      status_.pending_size = 0;
      if (status_.download_state != "RUNNING") status_.download_state = "IDLE";
    }
    persistStatus();
  }

  if (update_available) {
    if (cb) cb("UPDATE_AVAILABLE", pw.version);
  } else {
    if (cb) cb("NO_UPDATE", "");
  }
}

void UpdaterWin::downloadUpdate(Completion cb) {
  if (download_running_) {
    if (cb) cb("ALREADY_RUNNING", "");
    return;
  }
  // 上一轮线程已结束但还没回收：先 join，再复用成员（不能 detach，否则析构时可能访问已销毁的 this）。
  if (download_thread_.joinable()) download_thread_.join();

  std::string url, expected_sha256;
  std::int64_t expected_size = 0;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (status_.manifest_sha256.empty()) {
      if (cb) cb("NO_PENDING_UPDATE", "");
      return;
    }
    expected_sha256 = status_.manifest_sha256;
    expected_size = status_.manifest_size;
  }

  // 重拉清单拿 url（因为 status_ 里没存 url）
  std::string body;
  auto on_chunk = [&](const std::string& chunk) -> bool { body.append(chunk); return true; };
  std::string err_code, err_detail;
  if (!winhttpGet(manifest_url_, body, err_code, err_detail, nullptr, on_chunk)) {
    if (cb) cb("FETCH_FAILED", err_code + ":" + err_detail);
    return;
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    if (cb) cb("MANIFEST_INVALID", e.what());
    return;
  }
  V2Windows pw;
  std::string pw_err;
  if (!parse_v2_windows(j, pw, pw_err)) {
    if (cb) cb("MANIFEST_INVALID", pw_err);
    return;
  }
  url = pw.setup_url;

  download_cancel_ = false;
  download_running_ = true;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.download_state = "RUNNING";
    persistStatus();
  }

  download_thread_ = std::thread([this, url, expected_sha256, expected_size, cb] {
    // 目标文件放在下载目录，文件名用 build 号
    std::wstring dir = downloadDir();
    std::wstring target = dir + L"\\xiaotu-inventory-setup.exe.part";
    std::wstring final_name = dir + L"\\xiaotu-inventory-setup.exe";

    std::int64_t downloaded = 0;
    std::string err_code, err_detail;
    bool ok = true;

    // 先写 .part，下完再 rename
    FILE* f = nullptr;
    _wfopen_s(&f, target.c_str(), L"wb");
    if (!f) {
      ok = false; err_code = "FILE_OPEN"; err_detail = "";
    }

    if (ok) {
      std::int64_t total = expected_size;
      std::string body_unused;  // 下载时逐块 on_chunk 直接写文件，不累积
      auto on_chunk = [&](const std::string& chunk) -> bool {
        if (download_cancel_) return false;
        fwrite(chunk.data(), 1, chunk.size(), f);
        downloaded += chunk.size();
        // 更新进度
        {
          std::lock_guard<std::mutex> lock(status_mutex_);
          char prog[64];
          if (total > 0) snprintf(prog, sizeof(prog), "%lld/%lld (%.0f%%)", downloaded, total,
                                   downloaded * 100.0 / total);
          else snprintf(prog, sizeof(prog), "%lld bytes", downloaded);
          status_.download_progress = prog;
        }
        return true;
      };

      ok = winhttpGet(url, body_unused, err_code, err_detail, &total, on_chunk);
      fclose(f);
      if (download_cancel_) {
        _wremove(target.c_str());
        ok = false; err_code = "CANCELED"; err_detail = "";
      }
    }

    if (ok) {
      // rename
      _wremove(final_name.c_str());
      if (_wrename(target.c_str(), final_name.c_str()) != 0) {
        ok = false; err_code = "RENAME_FAILED";
      }
    }

    if (ok) {
      // SHA256 校验
      std::string actual = sha256File(final_name);
      if (actual.empty()) {
        ok = false; err_code = "SHA256_COMPUTE_FAILED";
      } else if (actual != expected_sha256) {
        ok = false; err_code = "SHA256_MISMATCH";
        err_detail = actual;
        _wremove(final_name.c_str());
      }
    }

    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      download_running_ = false;
      if (ok) {
        status_.download_state = "DONE";
        status_.pending_local_path = [&]() -> std::string {
          int n = WideCharToMultiByte(CP_UTF8, 0, final_name.c_str(), -1, nullptr, 0, nullptr, nullptr);
          std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
          if (n > 0) WideCharToMultiByte(CP_UTF8, 0, final_name.c_str(), -1, s.data(), n, nullptr, nullptr);
          return s;
        }();
        status_.pending_size = downloaded;
      } else {
        status_.download_state = "FAILED";
      }
      persistStatus();
    }

    if (cb) {
      std::string code = ok ? "DOWNLOADED" : (err_code == "CANCELED" ? "CANCELED" : "FAILED");
      cb(code, err_detail);
    }
  });
}

void UpdaterWin::cancelPrefetch(Completion cb) {
  if (!download_running_) {
    if (cb) cb("NOT_RUNNING", "");
    return;
  }
  download_cancel_ = true;
  if (cb) cb("CANCELED", "");
}

void UpdaterWin::installUpdate(Completion cb) {
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    std::string utf8 = status_.pending_local_path;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n > 0) { path.resize(n); MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, path.data(), n); }
  }
  if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (cb) cb("NO_PENDING_UPDATE", "");
    return;
  }
  // ISCC 静默安装：/VERYSILENT 静默、/RESTARTAPPLICATIONS 关旧壳启新壳
  std::wstring params = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /RESTARTAPPLICATIONS";
  ShellExecuteW(nullptr, L"open", path.c_str(), params.c_str(), nullptr, SW_HIDE);
  if (cb) cb("STARTED", "");
}

void UpdaterWin::setOptions(bool auto_check, bool prefetch, Completion cb) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.auto_check = auto_check;
    status_.prefetch = prefetch;
    persistStatus();
  }
  if (cb) cb("UPDATED", "");
}

void UpdaterWin::checkMinSupportedVersion(Completion cb) {
  // 简单实现：当前 shell 不直接查 health endpoint；让前端调壳内服务端的 /api/health
  // 或在 shell 里加一个直连。暂留 TODO。
  if (cb) cb("UNAVAILABLE", "checkMinSupportedVersion 未实现");
}

}  // namespace xiaotu::updater
