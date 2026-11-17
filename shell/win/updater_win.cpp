// 小兔进销存 Windows 桌面壳 —— 自动更新桥接层
//
// 实现：
//   WinHTTP 拉 latest.json → nlohmann::json 解析 → Ed25519 验签（OpenSSL）
//   → xiaotu::evaluate_manifest 判定 → WinHTTP 拉 setup.exe → BCrypt SHA256 校验
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

    // 24h±2h 间隔
    int jitter = (rand() % 4 - 2) * 3600;
    int wait = 86400 + jitter;
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
  UpdateManifest manifest;
  ManifestError err;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
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

  if (!xiaotu::parse_manifest(j, manifest, err)) {
    if (cb) cb("MANIFEST_INVALID", err.code + ":" + err.message);
    return;
  }

  // 只处理 platform=windows + channel 匹配的条目
  if (manifest.platform != "windows" || manifest.channel != status_.channel) {
    if (cb) cb("NO_UPDATE", "platform/channel mismatch");
    return;
  }

  UpdateQuery query;
  query.version = current_product_version();
  query.build = current_build_number();
  query.channel = status_.channel;
  query.platform = "windows";
  query.install_id = status_.install_id;

  UpdateDecision decision = xiaotu::evaluate_manifest(manifest, query, epochNow());

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.consecutive_failures = 0;
    status_.last_check_epoch = epochNow();
    if (decision.update_available || decision.mandatory) {
      status_.manifest_version = manifest.version;
      status_.manifest_build = manifest.build;
      status_.manifest_sha256 = manifest.sha256;
      status_.manifest_size = manifest.size;
      status_.mandatory = decision.mandatory;
      status_.mandatory_reason = decision.reason;
    }
    persistStatus();
  }

  if (decision.update_available || decision.mandatory) {
    if (cb) cb("UPDATE_AVAILABLE", manifest.version);
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

  UpdateManifest manifest;
  ManifestError merr;
  auto j = nlohmann::json::parse(body);
  if (!xiaotu::parse_manifest(j, manifest, merr)) {
    if (cb) cb("MANIFEST_INVALID", merr.code);
    return;
  }
  url = manifest.url;

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
