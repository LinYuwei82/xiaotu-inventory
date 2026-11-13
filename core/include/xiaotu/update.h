#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace xiaotu {

// 四条版本线中的产品版本（SemVer）。构建号、schema、API 版本均为整数，
// 分别由 XIAOTU_BUILD、schema_migration、XIAOTU_API_VERSION 提供。
struct ProductVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

bool parse_product_version(std::string_view text, ProductVersion& out);
int compare_product_version(const ProductVersion& a, const ProductVersion& b);
std::string format_product_version(const ProductVersion& version);

// 当前二进制携带的版本信息。
const char* current_product_version();
std::int64_t current_build_number();
int current_api_version();

// 更新清单。字段语义见 docs/更新模块技术方案.md 第 3 节。
struct UpdateManifest {
  int manifest_version = 1;
  std::string key_id;
  std::string platform;  // macos / windows
  std::string channel;   // stable / beta
  std::string version;
  std::int64_t build = 0;
  std::string min_os;
  std::string url;
  std::string sha256;
  std::int64_t size = 0;
  std::string published_at;
  std::string release_notes_url;
  bool mandatory = false;
  std::string mandatory_reason;    // SECURITY / PROTOCOL / REGULATORY / MIN_SUPPORTED
  std::string mandatory_deadline;  // ISO 8601 UTC，可空
  int schema_min = 0;
  int schema_max = 0;
  int api_min = 0;
  int api_max = 0;
  int rollout_percent = 100;
  std::string signature;
};

struct ManifestError {
  std::string code;
  std::string message;
};

// 解析并校验清单；失败时返回 false，error 带稳定错误码。
bool parse_manifest(const nlohmann::json& body, UpdateManifest& out, ManifestError& error);

struct UpdateQuery {
  std::string version;
  std::int64_t build = 0;
  std::string channel = "stable";
  std::string platform = "macos";
  int schema_version = 0;
  int api_version = 0;
  std::string os_version;  // 可空；用于 min_os 判断
  std::string install_id;  // 随机 UUID，仅用于灰度分桶
  std::string skipped_version;
  std::int64_t skip_until_epoch = 0;  // 跳过期截止（UTC 秒）
};

struct UpdateDecision {
  bool update_available = false;
  bool mandatory = false;
  std::string code;    // NO_UPDATE / UPDATE_AVAILABLE / SKIPPED / INCOMPATIBLE_SCHEMA 等
  std::string reason;  // 强制原因；非强制为空
};

// now_epoch 为 UTC 秒。纯函数，便于测试注入时钟。
UpdateDecision evaluate_manifest(const UpdateManifest& manifest, const UpdateQuery& query,
                                 std::int64_t now_epoch);

// 灰度分桶：SHA-256(install_id + ":" + build) 前 4 字节 mod 100。
int rollout_bucket(std::string_view install_id, std::int64_t build);
bool rollout_allows(int percent, int bucket);

// ISO 8601 UTC（YYYY-MM-DDTHH:MM:SSZ）与 epoch 互转；解析失败返回 nullopt。
std::optional<std::int64_t> parse_iso8601_utc(std::string_view text);

// 点分版本比较（如 "12.0" < "13.1"）；无法解析时按 0 处理。
int compare_dotted_version(std::string_view a, std::string_view b);

}  // namespace xiaotu
