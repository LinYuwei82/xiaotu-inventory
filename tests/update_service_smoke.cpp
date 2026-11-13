#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
  settings.verifier_python = "/usr/bin/true";

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

  std::filesystem::remove_all(dir);
  if (failures == 0) {
    std::cout << "update_service_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
