#include <httplib.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "xiaotu/update_service.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

std::filesystem::path write_manifest(const std::string& name, const std::string& body) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path);
  out << body;
  return path;
}

std::string manifest_json(const std::string& version, long long build, int schema_min,
                          int schema_max, int rollout) {
  return R"({
  "manifest_version": 1,
  "key_id": "2026-01",
  "platform": "macos",
  "channel": "stable",
  "version": ")" +
         version + R"(",
  "build": )" +
         std::to_string(build) + R"(,
  "min_os": "12.0",
  "url": "https://cdn.example.com/a.dmg",
  "sha256": "9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c",
  "size": 1024,
  "published_at": "2026-11-13T08:00:00Z",
  "release_notes_url": "https://example.com/notes",
  "mandatory": false,
  "schema_min": )" +
         std::to_string(schema_min) + R"(,
  "schema_max": )" + std::to_string(schema_max) + R"(,
  "api_min": 1,
  "api_max": 2,
  "rollout_percent": )" +
         std::to_string(rollout) + R"(,
  "signature": "c2ln"
})";
}

// v2 统一清单：两平台同一份；macOS build 固定 0，Windows 带构建号。
std::string manifest_v2_json(bool include_windows = true) {
  std::string body = R"({
  "manifest_version": 2,
  "product": "xiaotu",
  "channel": "stable",
  "key_id": "2026-11",
  "updated_at": "2026-11-18T04:48:54Z",
  "platforms": {
    "macos": {
      "version": "1.4.2",
      "build": 0,
      "min_os": "12.0",
      "artifacts": {"dmg": {
        "url": "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.2-macos-arm64.dmg",
        "size": 1024,
        "sha256": "9999999999999999999999999999999999999999999999999999999999999999"
      }}
    })";
  if (include_windows) {
    body += R"(,
    "windows": {
      "version": "1.4.3",
      "build": 2026111801,
      "min_os": "10.0",
      "rollout_percent": 100,
      "artifacts": {"setup": {
        "url": "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.3-win64-setup.exe",
        "size": 2048,
        "sha256": "8888888888888888888888888888888888888888888888888888888888888888"
      }}
    })";
  }
  body += R"(
  },
  "signature": "c2ln"
})";
  return body;
}

xiaotu::UpdateQuery query() {
  xiaotu::UpdateQuery q;
  q.version = "1.3.0";
  q.build = 2026100101;
  q.channel = "stable";
  q.platform = "macos";
  q.schema_version = 30;
  q.api_version = 1;
  q.os_version = "13.0";
  q.install_id = "install-1";
  return q;
}

}  // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "xiaotu-update-service-test";
  std::filesystem::create_directories(dir);

  xiaotu::UpdateSettings settings;
  xiaotu::UpdateCheckResult result = xiaotu::check_for_update(settings, query(), 0);
  check(!result.configured && result.error_code == "UPDATE_NOT_CONFIGURED",
        "未配置时返回 UPDATE_NOT_CONFIGURED");

  settings.manifest_file = (dir / "missing.json").string();
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.configured && result.error_code == "UPDATE_MANIFEST_UNREADABLE",
        "清单缺失时返回不可读");

  settings.manifest_file = write_manifest("bad.json", "{not json").string();
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.error_code == "UPDATE_MANIFEST_INVALID_JSON", "非法 JSON 被拒绝");

  // 验签可用的替身：/usr/bin/true 模拟验签通过；真实签名在 API 测试中覆盖。
  const auto pub_path = dir / "update.pub.pem";
  const auto script_path = dir / "update_manifest.py";
  std::ofstream(pub_path) << "test-public-key";
  std::ofstream(script_path) << "# test verifier";
  settings.public_key_file = pub_path.string();
  settings.verifier_script = script_path.string();
  // 验签桩要求"始终成功"：非 Windows 借 /usr/bin/true，Windows 没有该程序，
  // 用生成到临时目录的批处理代替（批处理会忽略追加的 verify 参数，直接返回 0）。
#if defined(_WIN32)
  const auto stub_path = dir / "stub-verifier.bat";
  std::ofstream(stub_path) << "@echo off\r\nexit /b 0\r\n";
  settings.verifier_python = stub_path.string();
#else
  settings.verifier_python = "/usr/bin/true";
#endif

  settings.manifest_file =
      write_manifest("stable.json", manifest_json("1.4.2", 2026111301, 28, 32, 100)).string();
  xiaotu::UpdateCheckResult unverified = xiaotu::check_for_update(
      xiaotu::UpdateSettings{.manifest_file = settings.manifest_file}, query(), 0);
  check(unverified.error_code == "UPDATE_UNVERIFIED", "未配置公钥时拒绝信任清单");

  xiaotu::UpdateSettings unavailable = settings;
  unavailable.verifier_script = (dir / "missing-verifier.py").string();
  check(xiaotu::check_for_update(unavailable, query(), 0).error_code ==
            "UPDATE_VERIFY_UNAVAILABLE",
        "验签工具缺失时拒绝");

  settings.channel = "stable";
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.manifest_ok && result.decision.update_available &&
            result.decision.code == "UPDATE_AVAILABLE",
        "合法清单返回可用更新");

  settings.channel = "beta";
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.error_code == "UPDATE_CHANNEL_UNAVAILABLE", "通道不匹配时拒绝");

  settings.channel = "stable";
  settings.min_supported_version = "1.4.0";
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.decision.mandatory && result.decision.reason == "MIN_SUPPORTED",
        "低于最低支持版本时强制");

  settings.min_supported_version.clear();
  settings.rollout_percent = 1;
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.manifest_ok, "灰度配置下清单仍可解析");

  auto wrong_schema = query();
  wrong_schema.schema_version = 27;
  result = xiaotu::check_for_update(settings, wrong_schema, 0);
  check(result.decision.code == "INCOMPATIBLE_SCHEMA", "schema 不兼容时拒绝");

  settings.manifest_file =
      write_manifest("rollout.json", manifest_json("1.4.2", 2026111301, 28, 32, 1)).string();
  settings.rollout_percent = 1;
  result = xiaotu::check_for_update(settings, query(), 0);
  check(!result.decision.update_available && result.decision.code == "NOT_IN_ROLLOUT",
        "灰度未命中时不下发");

  // 验签工具本身失败（非零退出）必须判为验签失败，而不是当成通过：
  // 这一条覆盖的是「子进程启动/退出码回传」链路，与上面的签名内容无关。
  // 同时要求把工具的 stderr 带进错误信息，避免只剩一个退出码。
#if defined(_WIN32)
  const auto fail_stub_path = dir / "fail-verifier.bat";
  std::ofstream(fail_stub_path) << "@echo off\r\necho 清单签名无效：测试桩 >&2\r\nexit /b 1\r\n";
  settings.verifier_python = fail_stub_path.string();
#else
  const auto fail_stub_path = dir / "fail-verifier.sh";
  {
    std::ofstream out(fail_stub_path);
    out << "#!/bin/sh\n"
        << "echo '清单签名无效：测试桩' >&2\n"
        << "exit 1\n";
  }
  std::filesystem::permissions(fail_stub_path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add);
  settings.verifier_python = fail_stub_path.string();
#endif
  result = xiaotu::check_for_update(settings, query(), 0);
  check(result.error_code == "UPDATE_SIGNATURE_INVALID", "验签工具返回非零时判为验签失败");
  check(result.error_message.find("清单签名无效") != std::string::npos,
        "验签失败信息应包含工具的 stderr 输出");

  // 远端厂商清单：拉取 → 验签 → 写缓存；TTL 内不重复拉取；失败回退上一次缓存。
  const auto cache_dir = dir / "vendor-cache";
  std::atomic<int> hits{0};
  std::string remote_body = manifest_json("1.5.0", 2026120101, 28, 32, 100);
  httplib::Server vendor;
  vendor.Get("/latest.json", [&](const httplib::Request&, httplib::Response& res) {
    ++hits;
    res.set_content(remote_body, "application/json");
  });
  vendor.Get("/bad.json", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{not json", "application/json");
  });
  const int vendor_port = vendor.bind_to_any_port("127.0.0.1");
  check(vendor_port > 0, "远端清单测试服务启动");
  std::thread vendor_thread([&vendor] { vendor.listen_after_bind(); });

  xiaotu::UpdateSettings remote;
  remote.manifest_url =
      "http://127.0.0.1:" + std::to_string(vendor_port) + "/latest.json";
  remote.manifest_cache_file = (cache_dir / "latest.json").string();
  remote.manifest_ttl_seconds = 600;
  remote.public_key_file = pub_path.string();
  remote.verifier_script = script_path.string();
#if defined(_WIN32)
  remote.verifier_python = (dir / "stub-verifier.bat").string();
#else
  remote.verifier_python = "/usr/bin/true";
#endif

  const std::int64_t base = static_cast<std::int64_t>(std::time(nullptr));
  result = xiaotu::check_for_update(remote, query(), base);
  check(result.manifest_ok && result.manifest_source == "network",
        "远端清单拉取并验签后按 network 返回");
  check(hits.load() == 1, "首次检查只拉取一次");
  check(result.manifest_path == remote.manifest_cache_file, "判定使用落盘的缓存清单");

  result = xiaotu::check_for_update(remote, query(), base + 10);
  check(result.manifest_ok && result.manifest_source == "cache" && !result.manifest_stale,
        "缓存有效期内直接读缓存且不标记过期");
  check(hits.load() == 1, "缓存有效期内不重复访问厂商更新源");

  result = xiaotu::check_for_update(remote, query(), base + 700);
  check(result.manifest_ok && result.manifest_source == "network" && hits.load() == 2,
        "缓存到期后重新拉取");

  // 远端内容损坏：不能覆盖上一次验签通过的缓存，按旧缓存继续判定并给出警告。
  remote.manifest_url =
      "http://127.0.0.1:" + std::to_string(vendor_port) + "/bad.json";
  remote.manifest_ttl_seconds = 0;
  result = xiaotu::check_for_update(remote, query(), base + 2000);
  check(result.manifest_ok && result.manifest_source == "cache" && result.manifest_stale,
        "远端清单损坏时回退旧缓存");
  check(result.manifest_warning.find("不是合法 JSON") != std::string::npos,
        "回退时说明远端清单损坏原因");
  check(hits.load() == 2, "损坏响应只影响本次刷新，不额外下载");

  // 网络彻底不可用：继续使用旧缓存；没有缓存才报错。
  vendor.stop();
  vendor_thread.join();
  result = xiaotu::check_for_update(remote, query(), base + 3000);
  check(result.manifest_ok && result.manifest_source == "cache" && result.manifest_stale,
        "厂商更新源不可达时回退旧缓存");
  check(result.manifest_warning.find("拉取更新清单失败") != std::string::npos,
        "网络失败时说明具体原因");

  xiaotu::UpdateSettings no_cache;
  no_cache.manifest_url =
      "http://127.0.0.1:" + std::to_string(vendor_port) + "/latest.json";
  no_cache.manifest_cache_file = (dir / "empty-cache" / "latest.json").string();
  no_cache.public_key_file = pub_path.string();
  no_cache.verifier_script = script_path.string();
  no_cache.verifier_python = remote.verifier_python;
  result = xiaotu::check_for_update(no_cache, query(), base + 4000);
  check(!result.manifest_ok && result.error_code == "UPDATE_MANIFEST_UNREACHABLE",
        "无缓存且网络不可用时明确报错");

  // v2 统一清单：按查询平台拍平；v2 不再按 schema / API 区间拦截。
  xiaotu::UpdateSettings v2_settings = settings;
  v2_settings.channel = "stable";
  v2_settings.min_supported_version.clear();
  v2_settings.rollout_percent = 100;
  v2_settings.verifier_python = remote.verifier_python;
  v2_settings.manifest_file = write_manifest("v2.json", manifest_v2_json()).string();

  auto mac_query = query();
  mac_query.version = "1.4.1";
  mac_query.build = 2026100101;
  mac_query.schema_version = 3;  // v2 清单忽略 schema 区间
  result = xiaotu::check_for_update(v2_settings, mac_query, 0);
  check(result.manifest_ok && result.manifest.platform == "macos" &&
            result.manifest.version == "1.4.2" && result.manifest.build == 0,
        "v2 清单按 macOS 段拍平");
  check(result.decision.code == "UPDATE_AVAILABLE", "v2 macOS 版本更高时下发更新");

  auto mac_same = mac_query;
  mac_same.version = "1.4.2";
  result = xiaotu::check_for_update(v2_settings, mac_same, 0);
  check(result.decision.code == "NO_UPDATE", "v2 macOS 同版本无更新");

  auto win_query = mac_query;
  win_query.platform = "windows";
  win_query.version = "1.4.3";
  win_query.build = 2026111800;
  result = xiaotu::check_for_update(v2_settings, win_query, 0);
  check(result.manifest_ok && result.manifest.platform == "windows" &&
            result.manifest.build == 2026111801,
        "v2 清单按 Windows 段拍平");
  check(result.decision.code == "UPDATE_AVAILABLE", "v2 Windows 同版本更高构建时下发");

  auto linux_query = mac_query;
  linux_query.platform = "linux";
  result = xiaotu::check_for_update(v2_settings, linux_query, 0);
  check(result.manifest_ok && result.decision.code == "PLATFORM_MISMATCH",
        "非桌面平台结构校验后报 PLATFORM_MISMATCH");

  v2_settings.manifest_file =
      write_manifest("v2-macos-only.json", manifest_v2_json(false)).string();
  result = xiaotu::check_for_update(v2_settings, win_query, 0);
  check(!result.manifest_ok && result.error_code == "MANIFEST_PLATFORM_MISSING",
        "v2 清单缺少请求平台段时拒绝");

  std::filesystem::remove_all(dir);
  if (failures == 0) {
    std::cout << "update_service_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
