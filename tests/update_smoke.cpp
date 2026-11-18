#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "xiaotu/update.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

nlohmann::json base_manifest() {
  return nlohmann::json{
      {"manifest_version", 1},
      {"key_id", "2026-01"},
      {"platform", "macos"},
      {"channel", "stable"},
      {"version", "1.4.2"},
      {"build", 2026111301},
      {"min_os", "12.0"},
      {"url", "https://cdn.example.com/releases/macos/1.4.2/xiaotu-1.4.2.dmg"},
      {"sha256",
       "9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c9f2c"},
      {"size", 48234496},
      {"published_at", "2026-11-13T08:00:00Z"},
      {"release_notes_url", "https://example.com/releases/1.4.2"},
      {"mandatory", false},
      {"mandatory_reason", nullptr},
      {"mandatory_deadline", nullptr},
      {"schema_min", 28},
      {"schema_max", 32},
      {"api_min", 1},
      {"api_max", 2},
      {"rollout_percent", 100},
      {"signature", "c2lnbmF0dXJl"}};
}

// v2 统一清单（manifest.json）：macOS 只带 version、build 固定 0；
// Windows 带构建号，且 setup 与 portable 两个产物同时存在。
nlohmann::json v2_base_manifest() {
  const std::string mac_sha(64, '9');
  const std::string win_sha(64, '8');
  const std::string zip_sha(64, '7');
  nlohmann::json body{
      {"manifest_version", 2},
      {"product", "xiaotu"},
      {"channel", "stable"},
      {"key_id", "2026-11"},
      {"updated_at", "2026-11-18T04:48:54Z"},
      {"signature", "c2lnbmF0dXJl"}};
  body["platforms"]["macos"] = {
      {"version", "1.4.2"},
      {"build", 0},
      {"min_os", "12.0"},
      {"artifacts",
       {{"dmg",
         {{"url", "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.2-macos-arm64.dmg"},
          {"size", 48234496},
          {"sha256", mac_sha}}}}}};
  body["platforms"]["windows"] = {
      {"version", "1.4.3"},
      {"build", 2026111801},
      {"min_os", "10.0"},
      {"rollout_percent", 50},
      {"artifacts",
       {{"setup",
         {{"url", "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.3-win64-setup.exe"},
          {"size", 2655925},
          {"sha256", win_sha}}},
        {"portable",
         {{"url", "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.3-win64-portable.zip"},
          {"size", 702247},
          {"sha256", zip_sha}}}}}};
  return body;
}

xiaotu::UpdateQuery query() {
  xiaotu::UpdateQuery value;
  value.version = "1.3.0";
  value.build = 2026100101;
  value.channel = "stable";
  value.platform = "macos";
  value.schema_version = 30;
  value.api_version = 1;
  value.os_version = "13.0";
  value.install_id = "11111111-2222-3333-4444-555555555555";
  return value;
}

}  // namespace

int main() {
  xiaotu::ProductVersion version;
  check(xiaotu::parse_product_version("1.4.2", version), "解析 1.4.2");
  check(version.major == 1 && version.minor == 4 && version.patch == 2, "版本分量正确");
  check(!xiaotu::parse_product_version("1.4", version), "两位版本应拒绝");
  check(!xiaotu::parse_product_version("1.4.2.1", version), "四位版本应拒绝");
  check(!xiaotu::parse_product_version("v1.4.2", version), "带前缀应拒绝");

  xiaotu::ProductVersion lower{1, 3, 9};
  xiaotu::ProductVersion higher{1, 4, 0};
  check(xiaotu::compare_product_version(lower, higher) < 0, "版本大小比较");
  check(xiaotu::compare_product_version(higher, higher) == 0, "版本相等比较");

  xiaotu::ProductVersion zero_nine{0, 9, 0};
  xiaotu::ProductVersion zero_ten{0, 10, 0};
  check(xiaotu::compare_product_version(zero_nine, zero_ten) < 0,
        "0.9.0 应小于 0.10.0（不能按字符串比较）");

  const auto day = xiaotu::parse_iso8601_utc("2026-11-13T00:00:00Z");
  const auto previous = xiaotu::parse_iso8601_utc("2026-11-12T00:00:00Z");
  check(day.has_value() && previous.has_value(), "ISO 时间解析");
  check(day.value() - previous.value() == 86400, "ISO 时间相差一天");
  check(!xiaotu::parse_iso8601_utc("2026-11-13 00:00:00").has_value(), "非严格 ISO 应拒绝");

  check(xiaotu::compare_dotted_version("12.0", "13.1") < 0, "系统版本比较");
  check(xiaotu::compare_dotted_version("13.0", "13.0.0") == 0, "系统版本补零比较");

  auto body = base_manifest();
  xiaotu::UpdateManifest manifest;
  xiaotu::ManifestError error;
  check(xiaotu::parse_manifest(body, manifest, error), "合法清单应通过");
  check(manifest.build == 2026111301 && manifest.schema_max == 32, "清单字段解析正确");
  check(error.code.empty(), "合法清单无错误码");

  auto missing = base_manifest();
  missing.erase("sha256");
  check(!xiaotu::parse_manifest(missing, manifest, error) &&
            error.code == "MANIFEST_FIELD_MISSING",
        "缺少字段应拒绝");

  auto bad_sha = base_manifest();
  bad_sha["sha256"] = "xyz";
  check(!xiaotu::parse_manifest(bad_sha, manifest, error) &&
            error.code == "MANIFEST_SHA256_INVALID",
        "非法 sha256 应拒绝");

  auto insecure = base_manifest();
  insecure["url"] = "http://cdn.example.com/a.dmg";
  check(!xiaotu::parse_manifest(insecure, manifest, error) &&
            error.code == "MANIFEST_URL_INSECURE",
        "非 HTTPS 应拒绝");

  auto bad_reason = base_manifest();
  bad_reason["mandatory"] = true;
  bad_reason["mandatory_reason"] = "WHATEVER";
  check(!xiaotu::parse_manifest(bad_reason, manifest, error) &&
            error.code == "MANIFEST_MANDATORY_REASON_INVALID",
        "未知强制理由应拒绝");

  auto bad_range = base_manifest();
  bad_range["schema_min"] = 33;
  bad_range["schema_max"] = 32;
  check(!xiaotu::parse_manifest(bad_range, manifest, error) &&
            error.code == "MANIFEST_SCHEMA_RANGE_INVALID",
        "schema 范围倒置应拒绝");

  xiaotu::parse_manifest(base_manifest(), manifest, error);
  auto decision = xiaotu::evaluate_manifest(manifest, query(), 0);
  check(decision.update_available && !decision.mandatory, "新构建应可用");
  check(decision.code == "UPDATE_AVAILABLE", "普通更新状态码");

  auto same = query();
  same.build = manifest.build;
  check(xiaotu::evaluate_manifest(manifest, same, 0).code == "NO_UPDATE", "同构建无更新");

  auto wrong_platform = query();
  wrong_platform.platform = "windows";
  check(xiaotu::evaluate_manifest(manifest, wrong_platform, 0).code == "PLATFORM_MISMATCH",
        "平台不匹配");

  auto old_schema = query();
  old_schema.schema_version = 20;
  auto path_body = base_manifest();
  path_body["upgrade_path"] = nlohmann::json::array(
      {{{"schema", 28}, {"version", "1.3.8"}, {"build", 2026100101}},
       {{"schema", 25}, {"version", "1.3.5"}, {"build", 2026040101}}});
  xiaotu::UpdateManifest with_path;
  check(xiaotu::parse_manifest(path_body, with_path, error), "含升级路径的清单应通过");
  check(with_path.upgrade_path.size() == 2 && with_path.upgrade_path[0].schema == 25,
        "升级路径按 schema 排序");
  auto intermediate = xiaotu::evaluate_manifest(with_path, old_schema, 0);
  check(intermediate.code == "INTERMEDIATE_REQUIRED" && intermediate.intermediate_required,
        "旧 schema 应要求先装中间版本");
  check(intermediate.intermediate.version == "1.3.5" && intermediate.intermediate.schema == 25,
        "中间版本取最接近的更大 schema");

  auto bad_path = base_manifest();
  bad_path["upgrade_path"] = nlohmann::json::array({{{"schema", 0}, {"version", "1.3.5"},
                                                     {"build", 1}}});
  check(!xiaotu::parse_manifest(bad_path, with_path, error) &&
            error.code == "MANIFEST_UPGRADE_PATH_INVALID",
        "非法升级路径应拒绝");

  auto wrong_schema = query();
  wrong_schema.schema_version = 27;
  check(xiaotu::evaluate_manifest(manifest, wrong_schema, 0).code == "INCOMPATIBLE_SCHEMA",
        "schema 超出范围应拒绝更新");

  auto old_os = query();
  old_os.os_version = "11.6";
  check(xiaotu::evaluate_manifest(manifest, old_os, 0).code == "OS_UNSUPPORTED",
        "低于最低系统版本应拒绝");

  auto skipped = query();
  skipped.skipped_version = manifest.version;
  skipped.skip_until_epoch = 1000;
  check(xiaotu::evaluate_manifest(manifest, skipped, 500).code == "SKIPPED",
        "跳过期内应跳过");
  check(xiaotu::evaluate_manifest(manifest, skipped, 1000).update_available,
        "跳过到期后恢复提示");

  auto mandatory_body = base_manifest();
  mandatory_body["mandatory"] = true;
  mandatory_body["mandatory_reason"] = "SECURITY";
  xiaotu::UpdateManifest mandatory;
  check(xiaotu::parse_manifest(mandatory_body, mandatory, error), "强制清单应通过");
  auto mandatory_skip = query();
  mandatory_skip.skipped_version = mandatory.version;
  mandatory_skip.skip_until_epoch = 99999;
  auto mandatory_decision = xiaotu::evaluate_manifest(mandatory, mandatory_skip, 0);
  check(mandatory_decision.update_available && mandatory_decision.mandatory,
        "强制更新不受跳过限制");
  check(mandatory_decision.reason == "SECURITY", "强制原因保留");

  const int bucket_a = xiaotu::rollout_bucket("device-a", 2026111301);
  const int bucket_b = xiaotu::rollout_bucket("device-a", 2026111301);
  check(bucket_a == bucket_b, "分桶稳定");
  check(bucket_a >= 0 && bucket_a < 100, "分桶范围 0-99");
  check(xiaotu::rollout_allows(bucket_a + 1, bucket_a), "桶号小于比例时放行");
  check(!xiaotu::rollout_allows(bucket_a, bucket_a), "桶号等于比例时不放行");

  auto staged_body = base_manifest();
  // 找一个桶号 > 0 的 install_id，使 rollout_percent = bucket 时必然不放行。
  int staged_bucket = 0;
  std::string staged_id = "device-a";
  for (int i = 0; i < 200 && staged_bucket == 0; ++i) {
    staged_id = "device-" + std::to_string(i);
    staged_bucket = xiaotu::rollout_bucket(staged_id, 2026111301);
  }
  staged_body["rollout_percent"] = staged_bucket;
  xiaotu::UpdateManifest staged;
  check(xiaotu::parse_manifest(staged_body, staged, error), "灰度清单应通过");
  auto staged_query = query();
  staged_query.install_id = staged_id;
  check(!xiaotu::evaluate_manifest(staged, staged_query, 0).update_available,
        "未命中灰度桶不下发");

  // 手动检查冷却：10 分钟。
  check(xiaotu::manual_check_retry_after(0, 5000, 600) == 0, "从未手动检查时可立即检查");
  check(xiaotu::manual_check_retry_after(1000, 1000, 600) == 600, "刚检查完剩余完整冷却");
  check(xiaotu::manual_check_retry_after(1000, 1300, 600) == 300, "冷却中返回剩余秒数");
  check(xiaotu::manual_check_retry_after(1000, 1600, 600) == 0, "冷却到期后放行");
  check(xiaotu::manual_check_retry_after(2000, 1000, 600) == 0, "时钟回拨时不阻塞");

  // 跳过天数范围：1–30 的整数。
  check(xiaotu::valid_skip_days(1) && xiaotu::valid_skip_days(30), "1 与 30 天应合法");
  check(!xiaotu::valid_skip_days(0) && !xiaotu::valid_skip_days(31) &&
            !xiaotu::valid_skip_days(-7),
        "0、31、负数应拒绝");

  // 检查失败退避：1 分钟 / 10 分钟 / 1 小时，之后保持 1 小时。
  check(xiaotu::check_retry_delay(1) == 60, "首次失败退避 1 分钟");
  check(xiaotu::check_retry_delay(2) == 600, "第二次失败退避 10 分钟");
  check(xiaotu::check_retry_delay(3) == 3600, "第三次失败退避 1 小时");
  check(xiaotu::check_retry_delay(9) == 3600, "后续失败保持 1 小时");

  // install 三种返回。
  using xiaotu::InstallDecision;
  check(xiaotu::decide_install(false, 2026111301, 2026100101) == InstallDecision::kUnavailable,
        "无 Sparkle 时不可用");
  check(xiaotu::decide_install(false, 0, 2026100101) == InstallDecision::kUnavailable,
        "无 Sparkle 且无待更新仍不可用");
  check(xiaotu::decide_install(true, 0, 2026100101) == InstallDecision::kNoPendingUpdate,
        "有安装器但无待更新");
  check(xiaotu::decide_install(true, 2026100101, 2026100101) ==
            InstallDecision::kNoPendingUpdate,
        "待更新不大于当前构建时应拒绝");
  check(xiaotu::decide_install(true, 2026111301, 2026100101) == InstallDecision::kStarted,
        "有待更新且安装器可用时启动");

  // v2 清单的 macOS 段 build 固定为 0：待安装与恢复态改用版本号判定。
  check(xiaotu::is_newer_product_version("1.0.3", "1.0.2"), "更高版本视为有待更新");
  check(!xiaotu::is_newer_product_version("1.0.2", "1.0.2"), "同版本不提示更新");
  check(!xiaotu::is_newer_product_version("1.0.1", "1.0.2"), "更低版本不提示更新");
  check(!xiaotu::is_newer_product_version("", "1.0.2"), "缺失版本不提示更新");
  check(!xiaotu::is_newer_product_version("bad", "1.0.2"), "非法版本不提示更新");
  check(xiaotu::is_same_product_version("1.0.2", "1.0.2"), "版本号相等");
  check(!xiaotu::is_same_product_version("1.0.2", "1.0.3"), "版本号不等");
  check(!xiaotu::is_same_product_version("", ""), "空版本不算相等");
  check(xiaotu::decide_install_version(true, "1.0.3", "1.0.2") == InstallDecision::kStarted,
        "版本更高且安装器可用时启动");
  check(xiaotu::decide_install_version(true, "1.0.2", "1.0.2") ==
            InstallDecision::kNoPendingUpdate,
        "版本相同拒绝安装");
  check(xiaotu::decide_install_version(false, "1.0.3", "1.0.2") == InstallDecision::kUnavailable,
        "无安装器时优先返回不可用");

  // 中间版本选择：原生桥与服务端 evaluate_manifest 共用同一条规则。
  const std::vector<xiaotu::UpgradeStep> upgrade_path{
      {28, "1.4.0", 2026100101},
      {25, "1.3.5", 2026040101},
      {30, "1.4.2", 2026111301}};
  const auto pick_low = xiaotu::select_intermediate_step(upgrade_path, 20);
  check(pick_low.has_value() && pick_low->schema == 25, "中间版本取最接近的更大 schema");
  const auto pick_exact = xiaotu::select_intermediate_step(upgrade_path, 25);
  check(pick_exact.has_value() && pick_exact->schema == 28, "schema 相等时跳过该步");
  check(!xiaotu::select_intermediate_step(upgrade_path, 30).has_value(),
        "没有更大的 schema 时返回空");
  check(!xiaotu::select_intermediate_step({}, 20).has_value(), "空路径返回空");

  auto consistent_body = base_manifest();
  consistent_body["upgrade_path"] = nlohmann::json::array(
      {{{"schema", 28}, {"version", "1.4.0"}, {"build", 2026100101}},
       {{"schema", 25}, {"version", "1.3.5"}, {"build", 2026040101}},
       {{"schema", 30}, {"version", "1.4.2"}, {"build", 2026111301}}});
  xiaotu::UpdateManifest consistent;
  check(xiaotu::parse_manifest(consistent_body, consistent, error), "带 upgrade_path 的清单解析通过");
  for (const int local : {20, 25}) {
    auto q = query();
    q.schema_version = local;
    const auto decision = xiaotu::evaluate_manifest(consistent, q, 0);
    const auto expected = xiaotu::select_intermediate_step(consistent.upgrade_path, local);
    check(expected.has_value() && decision.code == "INTERMEDIATE_REQUIRED" &&
              decision.intermediate.schema == expected->schema &&
              decision.intermediate.version == expected->version &&
              decision.intermediate.build == expected->build,
          "evaluate_manifest 与 select_intermediate_step 选出同一中间版本");
  }
  auto above_min = query();
  above_min.schema_version = 30;
  check(xiaotu::evaluate_manifest(consistent, above_min, 0).code == "UPDATE_AVAILABLE",
        "schema 达到最低要求时不再要求中间版本");

  // resume：暂停时可恢复，未暂停时给出可识别状态而不是错误。
  using xiaotu::ResumeDecision;
  check(xiaotu::decide_resume(true) == ResumeDecision::kResumed, "暂停时恢复自动更新");
  check(xiaotu::decide_resume(false) == ResumeDecision::kNotPaused,
        "未暂停时返回 NOT_PAUSED 语义");

  // 恢复模式：验证期（validating == current）或更高待装版本，失败达阈值才触发。
  check(xiaotu::should_enter_recovery(3, 3, 2026111301, 2026111301, 0),
        "新版本验证期连续失败 3 次进入恢复模式");
  check(xiaotu::should_enter_recovery(3, 3, 0, 2026100101, 2026111301),
        "更高版本待装时连续失败 3 次进入恢复模式");
  check(!xiaotu::should_enter_recovery(2, 3, 2026111301, 2026111301, 0),
        "失败次数未达阈值不进入恢复模式");
  check(!xiaotu::should_enter_recovery(3, 3, 2026100101, 2026111301, 0),
        "验证期与当前版本不一致且无待装版本时不进入恢复模式");
  check(!xiaotu::should_enter_recovery(3, 3, 0, 2026111301, 2026111301),
        "待装版本等于当前版本时不进入恢复模式");

  // 同上，版本口径（macOS v2）。
  check(xiaotu::should_enter_recovery_version(3, 3, "1.0.2", "", "1.0.2"),
        "版本验证期连续失败 3 次进入恢复模式");
  check(xiaotu::should_enter_recovery_version(3, 3, "", "1.0.3", "1.0.2"),
        "待装版本更高时连续失败 3 次进入恢复模式");
  check(!xiaotu::should_enter_recovery_version(2, 3, "1.0.2", "", "1.0.2"),
        "失败次数未达阈值不进入恢复模式");
  check(!xiaotu::should_enter_recovery_version(3, 3, "1.0.1", "", "1.0.2"),
        "验证版本与当前版本不一致时不进入恢复模式");
  check(!xiaotu::should_enter_recovery_version(3, 3, "", "1.0.2", "1.0.2"),
        "待装版本等于当前版本时不进入恢复模式");

  // ---- v2 统一清单：按平台拍平 + 版本优先判定 ----
  xiaotu::UpdateManifest mac_manifest;
  check(xiaotu::parse_manifest(v2_base_manifest(), "macos", mac_manifest, error),
        "v2 macOS 段应通过");
  check(mac_manifest.manifest_version == 2 && mac_manifest.platform == "macos" &&
            mac_manifest.version == "1.4.2" && mac_manifest.build == 0 &&
            mac_manifest.rollout_percent == 100,
        "v2 macOS 段拍平正确");
  check(mac_manifest.url.find("xiaotu-1.4.2-macos-arm64.dmg") != std::string::npos,
        "v2 macOS 取 dmg 产物");
  check(mac_manifest.published_at == "2026-11-18T04:48:54Z",
        "v2 updated_at 映射为 published_at");

  xiaotu::UpdateManifest win_manifest;
  check(xiaotu::parse_manifest(v2_base_manifest(), "windows", win_manifest, error),
        "v2 Windows 段应通过");
  check(win_manifest.platform == "windows" && win_manifest.build == 2026111801 &&
            win_manifest.rollout_percent == 50,
        "v2 Windows 段字段正确");
  check(win_manifest.url.find("win64-setup.exe") != std::string::npos,
        "v2 Windows 优先取 setup");

  xiaotu::UpdateManifest first_manifest;
  check(xiaotu::parse_manifest(v2_base_manifest(), first_manifest, error) &&
            first_manifest.platform == "macos",
        "不带平台的 v2 解析校验全部平台并取第一个");

  xiaotu::UpdateManifest unknown_manifest;
  check(!xiaotu::parse_manifest(v2_base_manifest(), "linux", unknown_manifest, error) &&
            error.code == "MANIFEST_PLATFORM_MISSING",
        "v2 请求不存在的平台应拒绝");

  // v2 不再携带 schema/api 区间：旧 schema 客户端也不再被拦截。
  auto v2_query = query();
  v2_query.version = "1.4.1";
  v2_query.build = 2026100101;
  v2_query.schema_version = 3;
  check(xiaotu::evaluate_manifest(mac_manifest, v2_query, 0).code == "UPDATE_AVAILABLE",
        "v2 版本更高应下发更新（忽略 schema/api 区间）");

  auto v2_same = v2_query;
  v2_same.version = "1.4.2";
  check(xiaotu::evaluate_manifest(mac_manifest, v2_same, 0).code == "NO_UPDATE",
        "v2 macOS 版本相同时无更新（build 固定 0）");

  auto v2_win_query = v2_query;
  v2_win_query.platform = "windows";
  v2_win_query.version = "1.4.3";
  v2_win_query.build = 2026111800;
  check(xiaotu::evaluate_manifest(win_manifest, v2_win_query, 0).code == "UPDATE_AVAILABLE",
        "v2 Windows 同版本更高构建号应下发");
  v2_win_query.build = 2026111801;
  check(xiaotu::evaluate_manifest(win_manifest, v2_win_query, 0).code == "NO_UPDATE",
        "v2 Windows 同版本同构建号无更新");

  auto v2_newer_local = v2_query;
  v2_newer_local.version = "1.5.0";
  check(xiaotu::evaluate_manifest(mac_manifest, v2_newer_local, 0).code == "NO_UPDATE",
        "v2 本地版本更高时无更新");

  auto v2_missing_dmg = v2_base_manifest();
  v2_missing_dmg["platforms"]["macos"]["artifacts"].erase("dmg");
  check(!xiaotu::parse_manifest(v2_missing_dmg, "macos", mac_manifest, error) &&
            error.code == "MANIFEST_ARTIFACT_MISSING",
        "v2 macOS 缺少 dmg 应拒绝");

  auto v2_bad_rollout = v2_base_manifest();
  v2_bad_rollout["platforms"]["windows"]["rollout_percent"] = 0;
  check(!xiaotu::parse_manifest(v2_bad_rollout, "windows", win_manifest, error) &&
            error.code == "MANIFEST_ROLLOUT_INVALID",
        "v2 rollout 0 应拒绝");

  if (failures == 0) {
    std::cout << "update_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
