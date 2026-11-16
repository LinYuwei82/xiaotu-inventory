#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// 清单声明的升级路径：旧 schema 需要先升级到某个中间版本。
struct UpgradeStep {
  int schema = 0;
  std::string version;
  std::int64_t build = 0;
};

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
  // 可选的升级路径，按 schema 升序；本地 schema 低于 schema_min 时用于指引中间版本。
  std::vector<UpgradeStep> upgrade_path;
};

struct ManifestError {
  std::string code;
  std::string message;
};

// 解析并校验清单；失败时返回 false，error 带稳定错误码。
bool parse_manifest(const nlohmann::json& body, UpdateManifest& out, ManifestError& error);

// 解析清单中的可选字段 upgrade_path；非法时返回 false 并设 MANIFEST_UPGRADE_PATH_INVALID。
bool parse_upgrade_path(const nlohmann::json& path, std::vector<UpgradeStep>& out,
                        ManifestError& error);

// 选出中间版本：schema 严格大于 local_schema 且最接近的一步；没有则返回 nullopt。
// 服务端 evaluate_manifest 与 macOS 原生桥共用这一条规则，避免两套判定。
std::optional<UpgradeStep> select_intermediate_step(const std::vector<UpgradeStep>& path,
                                                    int local_schema);

struct UpdateQuery {
  std::string version;
  std::int64_t build = 0;
  std::string channel = "stable";
  std::string platform = "macos";
  int schema_version = 0;
  int api_version = 0;
  std::string os_version;  // 可空；用于 min_os 判断
  // 服务端自查更新时忽略 min_os：该字段描述桌面客户端的最低系统版本，
  // 与服务端所在机器无关，不能因为拿不到系统版本就判成 OS_UNSUPPORTED。
  bool ignore_min_os = false;
  std::string install_id;  // 随机 UUID，仅用于灰度分桶
  std::string skipped_version;
  std::int64_t skip_until_epoch = 0;  // 跳过期截止（UTC 秒）
};

struct UpdateDecision {
  bool update_available = false;
  bool mandatory = false;
  std::string code;    // NO_UPDATE / UPDATE_AVAILABLE / SKIPPED / INCOMPATIBLE_SCHEMA 等
  std::string reason;  // 强制原因；非强制为空
  // code == INTERMEDIATE_REQUIRED 时给出应先升级到的版本；其余情况为空。
  bool intermediate_required = false;
  UpgradeStep intermediate;
};

// now_epoch 为 UTC 秒。纯函数，便于测试注入时钟。
UpdateDecision evaluate_manifest(const UpdateManifest& manifest, const UpdateQuery& query,
                                 std::int64_t now_epoch);

// 灰度分桶：SHA-256(install_id + ":" + build) 前 4 字节 mod 100。
int rollout_bucket(std::string_view install_id, std::int64_t build);
bool rollout_allows(int percent, int bucket);

// ---- 原生桥接层复用的小判定（纯函数，便于 ctest 覆盖） ----

// 手动检查冷却：返回距下次可检查的秒数；<= 0 表示当前可检查。
// last_manual_epoch 为 0（从未手动检查）时直接放行。
std::int64_t manual_check_retry_after(std::int64_t last_manual_epoch, std::int64_t now_epoch,
                                      std::int64_t cooldown_seconds);

// 跳过天数合法范围：1–30 的整数。
bool valid_skip_days(int days);

// install 桥方法的三种结果，对应 STARTED / UNAVAILABLE / NO_PENDING_UPDATE。
enum class InstallDecision { kStarted, kUnavailable, kNoPendingUpdate };

// 无安装器时优先返回不可用；有待安装版本（pending_build > current_build）才可启动安装。
InstallDecision decide_install(bool installer_available, std::int64_t pending_build,
                               std::int64_t current_build);

// resume 桥方法的结果：RESUMED / NOT_PAUSED。
enum class ResumeDecision { kResumed, kNotPaused };
ResumeDecision decide_resume(bool paused);

// 连续启动失败后的恢复判定：
//   validating_build == current_build 表示新版本仍在验证期（安装后首次运行）；
//   pending_build != 0 && pending_build != current_build 表示更高版本已下载但还没装上。
// 任一情况成立且失败次数达到 threshold 时进入恢复模式，提示回退或继续使用当前版本。
bool should_enter_recovery(int launch_attempts, int threshold, std::int64_t validating_build,
                           std::int64_t current_build, std::int64_t pending_build);

// ISO 8601 UTC（YYYY-MM-DDTHH:MM:SSZ）与 epoch 互转；解析失败返回 nullopt。
std::optional<std::int64_t> parse_iso8601_utc(std::string_view text);

// 点分版本比较（如 "12.0" < "13.1"）；无法解析时按 0 处理。
int compare_dotted_version(std::string_view a, std::string_view b);

}  // namespace xiaotu
