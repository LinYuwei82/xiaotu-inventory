#include "xiaotu/update.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef XIAOTU_VERSION
#define XIAOTU_VERSION "0.9.0"
#endif
#ifndef XIAOTU_BUILD
#define XIAOTU_BUILD 1
#endif
#ifndef XIAOTU_API_VERSION
#define XIAOTU_API_VERSION 1
#endif

namespace xiaotu {
namespace {

bool is_hex_sha256(std::string_view text) {
  if (text.size() != 64) {
    return false;
  }
  for (const char ch : text) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

void set_error(ManifestError& error, std::string code, std::string message) {
  error.code = std::move(code);
  error.message = std::move(message);
}

bool read_string(const nlohmann::json& body, const char* key, std::string& out, bool required,
                 ManifestError& error) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) {
    if (required) {
      set_error(error, "MANIFEST_FIELD_MISSING", std::string("缺少字段：") + key);
      return false;
    }
    return true;
  }
  if (!it->is_string()) {
    set_error(error, "MANIFEST_FIELD_TYPE", std::string("字段类型错误：") + key);
    return false;
  }
  out = it->get<std::string>();
  return true;
}

bool read_int(const nlohmann::json& body, const char* key, std::int64_t& out, bool required,
              ManifestError& error) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) {
    if (required) {
      set_error(error, "MANIFEST_FIELD_MISSING", std::string("缺少字段：") + key);
      return false;
    }
    return true;
  }
  if (!it->is_number_integer()) {
    set_error(error, "MANIFEST_FIELD_TYPE", std::string("字段类型错误：") + key);
    return false;
  }
  out = it->get<std::int64_t>();
  return true;
}

bool read_bool(const nlohmann::json& body, const char* key, bool& out, ManifestError& error) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) {
    return true;
  }
  if (!it->is_boolean()) {
    set_error(error, "MANIFEST_FIELD_TYPE", std::string("字段类型错误：") + key);
    return false;
  }
  out = it->get<bool>();
  return true;
}

constexpr std::uint32_t kSha256Init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

std::uint32_t rotr(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

std::array<std::uint8_t, 32> sha256(std::string_view input) {
  std::string data(input);
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8u;
  data.push_back(static_cast<char>(0x80));
  while (data.size() % 64 != 56) {
    data.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    data.push_back(static_cast<char>((bit_length >> shift) & 0xFFu));
  }

  std::uint32_t h[8];
  std::memcpy(h, kSha256Init, sizeof(h));

  for (std::size_t offset = 0; offset < data.size(); offset += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(data.data() + offset + i * 4);
      w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
             (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];
    std::uint32_t f = h[5];
    std::uint32_t g = h[6];
    std::uint32_t hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t temp1 = hh + s1 + ch + kSha256K[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::array<std::uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFFu);
    digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFFu);
    digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFFu);
    digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFFu);
  }
  return digest;
}

// Howard Hinnant 的 days_from_civil，用于不依赖时区与系统 timegm。
std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool parse_int_field(std::string_view text, int& out) {
  if (text.empty()) {
    return false;
  }
  int value = 0;
  for (const char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    value = value * 10 + (ch - '0');
    if (value > 100000) {
      return false;
    }
  }
  out = value;
  return true;
}

}  // namespace

bool parse_product_version(std::string_view text, ProductVersion& out) {
  ProductVersion parsed;
  std::string_view rest = text;
  int parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    const std::size_t dot = rest.find('.');
    const std::string_view piece = dot == std::string_view::npos ? rest : rest.substr(0, dot);
    if (piece.empty()) {
      return false;
    }
    int value = 0;
    for (const char ch : piece) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
      value = value * 10 + (ch - '0');
      if (value > 100000) {
        return false;
      }
    }
    parts[i] = value;
    if (i < 2) {
      if (dot == std::string_view::npos) {
        return false;
      }
      rest = rest.substr(dot + 1);
    } else if (dot != std::string_view::npos) {
      return false;
    }
  }
  out = ProductVersion{parts[0], parts[1], parts[2]};
  return true;
}

int compare_product_version(const ProductVersion& a, const ProductVersion& b) {
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  return 0;
}

std::string format_product_version(const ProductVersion& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
         std::to_string(version.patch);
}

const char* current_product_version() { return XIAOTU_VERSION; }

std::int64_t current_build_number() { return static_cast<std::int64_t>(XIAOTU_BUILD); }

int current_api_version() { return XIAOTU_API_VERSION; }

namespace {

// v1 / v2 共用的字段校验。v2 没有 schema/api 区间，平台与构建号规则也不同：
// 构建号允许为 0（本机未注入时按 0 处理），但不得为负。
bool validate_manifest_fields(UpdateManifest& parsed, bool v2, ManifestError& error) {
  if (parsed.key_id.empty()) {
    set_error(error, "MANIFEST_KEY_ID_EMPTY", "key_id 不能为空");
    return false;
  }
  if (!v2 && parsed.platform != "macos" && parsed.platform != "windows") {
    set_error(error, "MANIFEST_PLATFORM_INVALID", "platform 只能是 macos 或 windows");
    return false;
  }
  if (parsed.channel != "stable" && parsed.channel != "beta") {
    set_error(error, "MANIFEST_CHANNEL_INVALID", "channel 只能是 stable 或 beta");
    return false;
  }
  ProductVersion version;
  if (!parse_product_version(parsed.version, version)) {
    set_error(error, "MANIFEST_VERSION_INVALID", "version 不符合 MAJOR.MINOR.PATCH");
    return false;
  }
  if (v2 ? parsed.build < 0 : parsed.build <= 0) {
    set_error(error, "MANIFEST_BUILD_INVALID", "build 非法");
    return false;
  }
  if (!starts_with(parsed.url, "https://")) {
    set_error(error, "MANIFEST_URL_INSECURE", "url 必须是 https");
    return false;
  }
  if (!is_hex_sha256(parsed.sha256)) {
    set_error(error, "MANIFEST_SHA256_INVALID", "sha256 必须是 64 位十六进制");
    return false;
  }
  if (parsed.size <= 0) {
    set_error(error, "MANIFEST_SIZE_INVALID", "size 必须为正数");
    return false;
  }
  if (!parse_iso8601_utc(parsed.published_at).has_value()) {
    set_error(error, "MANIFEST_TIME_INVALID", "published_at 必须是 UTC ISO 8601");
    return false;
  }
  if (!parsed.mandatory_deadline.empty() &&
      !parse_iso8601_utc(parsed.mandatory_deadline).has_value()) {
    set_error(error, "MANIFEST_TIME_INVALID", "mandatory_deadline 格式错误");
    return false;
  }
  if (!v2) {
    if (parsed.schema_min <= 0 || parsed.schema_max < parsed.schema_min) {
      set_error(error, "MANIFEST_SCHEMA_RANGE_INVALID", "schema 范围非法");
      return false;
    }
    if (parsed.api_min <= 0 || parsed.api_max < parsed.api_min) {
      set_error(error, "MANIFEST_API_RANGE_INVALID", "API 范围非法");
      return false;
    }
  }
  if (parsed.rollout_percent < 1 || parsed.rollout_percent > 100) {
    set_error(error, "MANIFEST_ROLLOUT_INVALID", "rollout_percent 必须在 1–100");
    return false;
  }
  if (parsed.mandatory) {
    const bool known = parsed.mandatory_reason == "SECURITY" ||
                       parsed.mandatory_reason == "PROTOCOL" ||
                       parsed.mandatory_reason == "REGULATORY" ||
                       parsed.mandatory_reason == "MIN_SUPPORTED";
    if (!known) {
      set_error(error, "MANIFEST_MANDATORY_REASON_INVALID",
                "mandatory_reason 必须是四类之一");
      return false;
    }
  } else {
    parsed.mandatory_reason.clear();
    parsed.mandatory_deadline.clear();
  }
  if (parsed.signature.empty()) {
    set_error(error, "MANIFEST_SIGNATURE_MISSING", "signature 不能为空");
    return false;
  }
  return true;
}

// 解析 v2 的单个平台段并拍平：macOS 取 artifacts.dmg，Windows 优先 setup，
// 缺省时退回 portable（绿色包）。未知 artifacts key 与未知字段一律忽略。
bool parse_manifest_v2_section(const nlohmann::json& section, const std::string& platform,
                               UpdateManifest& parsed, ManifestError& error) {
  if (!section.is_object()) {
    set_error(error, "MANIFEST_PLATFORM_INVALID",
              "platforms." + platform + " 必须是对象");
    return false;
  }
  parsed.platform = platform;
  std::int64_t build = 0;
  std::int64_t rollout = 100;
  if (!read_string(section, "version", parsed.version, true, error) ||
      !read_int(section, "build", build, true, error) ||
      !read_string(section, "min_os", parsed.min_os, false, error) ||
      !read_string(section, "notes_url", parsed.release_notes_url, false, error) ||
      !read_int(section, "rollout_percent", rollout, false, error) ||
      !read_bool(section, "mandatory", parsed.mandatory, error) ||
      !read_string(section, "mandatory_reason", parsed.mandatory_reason, false, error) ||
      !read_string(section, "mandatory_deadline", parsed.mandatory_deadline, false, error)) {
    return false;
  }
  parsed.build = build;
  parsed.rollout_percent = static_cast<int>(rollout);

  const auto artifacts = section.find("artifacts");
  if (artifacts == section.end() || !artifacts->is_object()) {
    set_error(error, "MANIFEST_ARTIFACT_MISSING",
              "platforms." + platform + " 缺少 artifacts");
    return false;
  }
  const char* role = nullptr;
  if (platform == "macos") {
    role = "dmg";
  } else {
    const auto setup = artifacts->find("setup");
    role = (setup != artifacts->end() && setup->is_object()) ? "setup" : "portable";
  }
  const auto entry = artifacts->find(role);
  if (entry == artifacts->end() || !entry->is_object()) {
    set_error(error, "MANIFEST_ARTIFACT_MISSING",
              "platforms." + platform + ".artifacts 缺少 " + role);
    return false;
  }
  if (!read_string(*entry, "url", parsed.url, true, error) ||
      !read_string(*entry, "sha256", parsed.sha256, true, error) ||
      !read_int(*entry, "size", parsed.size, true, error)) {
    error.message = std::string("artifacts.") + role + "：" + error.message;
    return false;
  }
  return true;
}

bool parse_manifest_v2(const nlohmann::json& body, std::string_view requested_platform,
                       UpdateManifest& out, ManifestError& error) {
  UpdateManifest base;
  base.manifest_version = 2;
  std::string product;
  if (!read_string(body, "product", product, true, error) ||
      !read_string(body, "channel", base.channel, true, error) ||
      !read_string(body, "key_id", base.key_id, true, error) ||
      !read_string(body, "updated_at", base.published_at, true, error) ||
      !read_string(body, "signature", base.signature, true, error)) {
    return false;
  }
  const auto platforms = body.find("platforms");
  if (platforms == body.end() || !platforms->is_object()) {
    set_error(error, "MANIFEST_PLATFORM_MISSING", "清单缺少 platforms 对象");
    return false;
  }

  std::vector<std::string> candidates;
  if (!requested_platform.empty()) {
    candidates.emplace_back(requested_platform);
  } else {
    for (const char* name : {"macos", "windows"}) {
      if (platforms->find(name) != platforms->end()) {
        candidates.emplace_back(name);
      }
    }
  }
  if (candidates.empty()) {
    set_error(error, "MANIFEST_PLATFORM_MISSING", "platforms 中没有 macos / windows");
    return false;
  }

  UpdateManifest chosen;
  bool have = false;
  for (const auto& name : candidates) {
    const auto section = platforms->find(name);
    if (section == platforms->end()) {
      set_error(error, "MANIFEST_PLATFORM_MISSING", "清单缺少 platforms." + name);
      return false;
    }
    UpdateManifest part = base;
    if (!parse_manifest_v2_section(*section, name, part, error) ||
        !validate_manifest_fields(part, true, error)) {
      return false;
    }
    if (!have) {
      chosen = std::move(part);
      have = true;
    }
  }
  out = std::move(chosen);
  return true;
}

}  // namespace

bool parse_manifest(const nlohmann::json& body, UpdateManifest& out, ManifestError& error) {
  return parse_manifest(body, std::string_view{}, out, error);
}

bool parse_manifest(const nlohmann::json& body, std::string_view platform, UpdateManifest& out,
                    ManifestError& error) {
  error = {};
  if (!body.is_object()) {
    set_error(error, "MANIFEST_NOT_OBJECT", "清单必须是 JSON 对象");
    return false;
  }

  UpdateManifest parsed;
  std::int64_t number = 0;
  if (!read_int(body, "manifest_version", number, true, error)) {
    return false;
  }
  parsed.manifest_version = static_cast<int>(number);
  if (parsed.manifest_version == 2) {
    return parse_manifest_v2(body, platform, out, error);
  }
  if (parsed.manifest_version != 1) {
    set_error(error, "MANIFEST_VERSION_UNSUPPORTED", "不支持的清单版本");
    return false;
  }

  if (!read_string(body, "key_id", parsed.key_id, true, error) ||
      !read_string(body, "platform", parsed.platform, true, error) ||
      !read_string(body, "channel", parsed.channel, true, error) ||
      !read_string(body, "version", parsed.version, true, error) ||
      !read_int(body, "build", parsed.build, true, error) ||
      !read_string(body, "url", parsed.url, true, error) ||
      !read_string(body, "sha256", parsed.sha256, true, error) ||
      !read_int(body, "size", parsed.size, true, error) ||
      !read_string(body, "published_at", parsed.published_at, true, error) ||
      !read_bool(body, "mandatory", parsed.mandatory, error)) {
    return false;
  }
  read_string(body, "min_os", parsed.min_os, false, error);
  read_string(body, "release_notes_url", parsed.release_notes_url, false, error);
  read_string(body, "mandatory_reason", parsed.mandatory_reason, false, error);
  read_string(body, "mandatory_deadline", parsed.mandatory_deadline, false, error);
  read_string(body, "signature", parsed.signature, true, error);
  if (!error.code.empty()) {
    return false;
  }

  std::int64_t schema_min = 0;
  std::int64_t schema_max = 0;
  std::int64_t api_min = 0;
  std::int64_t api_max = 0;
  std::int64_t rollout = 100;
  if (!read_int(body, "schema_min", schema_min, true, error) ||
      !read_int(body, "schema_max", schema_max, true, error) ||
      !read_int(body, "api_min", api_min, true, error) ||
      !read_int(body, "api_max", api_max, true, error) ||
      !read_int(body, "rollout_percent", rollout, false, error)) {
    return false;
  }
  parsed.schema_min = static_cast<int>(schema_min);
  parsed.schema_max = static_cast<int>(schema_max);
  parsed.api_min = static_cast<int>(api_min);
  parsed.api_max = static_cast<int>(api_max);
  parsed.rollout_percent = static_cast<int>(rollout);

  if (const auto it = body.find("upgrade_path"); it != body.end() && !it->is_null()) {
    if (!parse_upgrade_path(*it, parsed.upgrade_path, error)) {
      return false;
    }
  }

  if (!validate_manifest_fields(parsed, false, error)) {
    return false;
  }

  out = std::move(parsed);
  return true;
}

bool parse_upgrade_path(const nlohmann::json& path, std::vector<UpgradeStep>& out,
                        ManifestError& error) {
  if (!path.is_array()) {
    set_error(error, "MANIFEST_UPGRADE_PATH_INVALID", "upgrade_path 必须是数组");
    return false;
  }
  for (const auto& item : path) {
    if (!item.is_object()) {
      set_error(error, "MANIFEST_UPGRADE_PATH_INVALID", "upgrade_path 元素必须是对象");
      return false;
    }
    UpgradeStep step;
    std::int64_t step_schema = 0;
    std::int64_t step_build = 0;
    if (!read_int(item, "schema", step_schema, true, error) ||
        !read_string(item, "version", step.version, true, error) ||
        !read_int(item, "build", step_build, true, error)) {
      return false;
    }
    ProductVersion step_version;
    if (step_schema <= 0 || step_build <= 0 ||
        !parse_product_version(step.version, step_version)) {
      set_error(error, "MANIFEST_UPGRADE_PATH_INVALID", "upgrade_path 字段值非法");
      return false;
    }
    step.schema = static_cast<int>(step_schema);
    step.build = step_build;
    out.push_back(std::move(step));
  }
  std::sort(out.begin(), out.end(),
            [](const UpgradeStep& a, const UpgradeStep& b) { return a.schema < b.schema; });
  return true;
}

std::optional<UpgradeStep> select_intermediate_step(const std::vector<UpgradeStep>& path,
                                                    int local_schema) {
  const UpgradeStep* chosen = nullptr;
  for (const auto& step : path) {
    if (step.schema > local_schema && (chosen == nullptr || step.schema < chosen->schema)) {
      chosen = &step;
    }
  }
  if (chosen == nullptr) {
    return std::nullopt;
  }
  return *chosen;
}

int rollout_bucket(std::string_view install_id, std::int64_t build) {
  const std::string material = std::string(install_id) + ":" + std::to_string(build);
  const auto digest = sha256(material);
  const std::uint32_t value = (static_cast<std::uint32_t>(digest[0]) << 24) |
                              (static_cast<std::uint32_t>(digest[1]) << 16) |
                              (static_cast<std::uint32_t>(digest[2]) << 8) |
                              static_cast<std::uint32_t>(digest[3]);
  return static_cast<int>(value % 100u);
}

bool rollout_allows(int percent, int bucket) {
  return bucket >= 0 && bucket < std::clamp(percent, 0, 100);
}

std::int64_t manual_check_retry_after(std::int64_t last_manual_epoch, std::int64_t now_epoch,
                                      std::int64_t cooldown_seconds) {
  if (last_manual_epoch <= 0 || cooldown_seconds <= 0 || now_epoch < last_manual_epoch) {
    return 0;
  }
  const std::int64_t remaining = last_manual_epoch + cooldown_seconds - now_epoch;
  return remaining > 0 ? remaining : 0;
}

bool valid_skip_days(int days) {
  return days >= 1 && days <= 30;
}

std::int64_t check_retry_delay(int failures) {
  if (failures <= 1) {
    return 60;
  }
  if (failures == 2) {
    return 600;
  }
  return 3600;
}

InstallDecision decide_install(bool installer_available, std::int64_t pending_build,
                               std::int64_t current_build) {
  if (!installer_available) {
    return InstallDecision::kUnavailable;
  }
  if (pending_build <= current_build) {
    return InstallDecision::kNoPendingUpdate;
  }
  return InstallDecision::kStarted;
}

bool is_newer_product_version(std::string_view target, std::string_view current) {
  ProductVersion target_version{};
  ProductVersion current_version{};
  if (!parse_product_version(target, target_version) ||
      !parse_product_version(current, current_version)) {
    return false;
  }
  return compare_product_version(target_version, current_version) > 0;
}

bool is_update_newer(std::string_view remote_version, std::int64_t remote_build,
                     std::string_view local_version, std::int64_t local_build) {
  ProductVersion remote{};
  ProductVersion local{};
  if (!parse_product_version(remote_version, remote) ||
      !parse_product_version(local_version, local)) {
    return false;
  }
  const int order = compare_product_version(remote, local);
  if (order != 0) {
    return order > 0;
  }
  if (local_build <= 0) {
    return false;
  }
  return remote_build > local_build;
}

bool is_same_product_version(std::string_view a, std::string_view b) {
  ProductVersion left{};
  ProductVersion right{};
  if (!parse_product_version(a, left) || !parse_product_version(b, right)) {
    return false;
  }
  return compare_product_version(left, right) == 0;
}

ResumeDecision decide_resume(bool paused) {
  return paused ? ResumeDecision::kResumed : ResumeDecision::kNotPaused;
}

bool should_enter_recovery(int launch_attempts, int threshold, std::int64_t validating_build,
                           std::int64_t current_build, std::int64_t pending_build) {
  if (threshold <= 0 || launch_attempts < threshold) {
    return false;
  }
  const bool validating = validating_build != 0 && validating_build == current_build;
  const bool pending_update = pending_build != 0 && pending_build != current_build;
  return validating || pending_update;
}

bool should_enter_recovery_version(int launch_attempts, int threshold,
                                   std::string_view validating_version,
                                   std::string_view pending_version,
                                   std::string_view current_version) {
  if (threshold <= 0 || launch_attempts < threshold) {
    return false;
  }
  return is_same_product_version(validating_version, current_version) ||
         is_newer_product_version(pending_version, current_version);
}

UpdateDecision evaluate_manifest(const UpdateManifest& manifest, const UpdateQuery& query,
                                 std::int64_t now_epoch) {
  UpdateDecision decision;
  if (manifest.platform != query.platform) {
    decision.code = "PLATFORM_MISMATCH";
    return decision;
  }
  if (manifest.channel != query.channel) {
    decision.code = "CHANNEL_MISMATCH";
    return decision;
  }
  if (!manifest.min_os.empty() && !query.ignore_min_os) {
    if (compare_dotted_version(query.os_version, manifest.min_os) < 0) {
      decision.code = "OS_UNSUPPORTED";
      return decision;
    }
  }
  if (manifest.manifest_version < 2) {
    if (query.schema_version < manifest.schema_min) {
      // 旧库无法直接迁移到最新版：按 upgrade_path 给出应先升级到的中间版本。
      // 与服务端共用 select_intermediate_step，保证两处选出的版本一致。
      if (const auto chosen = select_intermediate_step(manifest.upgrade_path,
                                                       query.schema_version)) {
        decision.code = "INTERMEDIATE_REQUIRED";
        decision.intermediate_required = true;
        decision.intermediate = *chosen;
        return decision;
      }
      decision.code = "INCOMPATIBLE_SCHEMA";
      return decision;
    }
    if (query.schema_version > manifest.schema_max) {
      decision.code = "INCOMPATIBLE_SCHEMA";
      return decision;
    }
    if (query.api_version > 0 &&
        (query.api_version < manifest.api_min || query.api_version > manifest.api_max)) {
      decision.code = "INCOMPATIBLE_API";
      return decision;
    }
    if (manifest.build <= query.build) {
      decision.code = "NO_UPDATE";
      return decision;
    }
  } else {
    // v2 不携带 schema/api 区间与 upgrade_path，数据兼容性由客户端与服务端握手把关。
    // 更新判定（macOS / Windows 同一口径）：先比 version，同版本再比 build；
    // 本机 build 未知（<= 0）时同版本不比 build。
    ProductVersion remote{};
    ProductVersion local{};
    if (!parse_product_version(manifest.version, remote) ||
        !parse_product_version(query.version, local)) {
      decision.code = "VERSION_INVALID";
      return decision;
    }
    const int order = compare_product_version(remote, local);
    if (order < 0) {
      decision.code = "NO_UPDATE";
      return decision;
    }
    if (order == 0 && manifest.build <= query.build) {
      decision.code = "NO_UPDATE";
      return decision;
    }
  }
  const int bucket = rollout_bucket(query.install_id, manifest.build);
  if (!manifest.mandatory && !rollout_allows(manifest.rollout_percent, bucket)) {
    decision.code = "NOT_IN_ROLLOUT";
    return decision;
  }
  if (!manifest.mandatory && !query.skipped_version.empty() &&
      query.skipped_version == manifest.version && now_epoch < query.skip_until_epoch) {
    decision.code = "SKIPPED";
    return decision;
  }

  decision.update_available = true;
  decision.mandatory = manifest.mandatory;
  decision.reason = manifest.mandatory_reason;
  decision.code = manifest.mandatory ? "UPDATE_REQUIRED" : "UPDATE_AVAILABLE";
  return decision;
}

std::optional<std::int64_t> parse_iso8601_utc(std::string_view text) {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
    return std::nullopt;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_int_field(text.substr(0, 4), year) || !parse_int_field(text.substr(5, 2), month) ||
      !parse_int_field(text.substr(8, 2), day) || !parse_int_field(text.substr(11, 2), hour) ||
      !parse_int_field(text.substr(14, 2), minute) ||
      !parse_int_field(text.substr(17, 2), second)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return std::nullopt;
  }
  return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
         static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 + second;
}

int compare_dotted_version(std::string_view a, std::string_view b) {
  auto next = [](std::string_view& text) -> int {
    if (text.empty()) {
      return 0;
    }
    const std::size_t dot = text.find('.');
    const std::string_view piece = dot == std::string_view::npos ? text : text.substr(0, dot);
    int value = 0;
    for (const char ch : piece) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        break;
      }
      value = value * 10 + (ch - '0');
    }
    text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    return value;
  };
  for (int i = 0; i < 4; ++i) {
    const int left = next(a);
    const int right = next(b);
    if (left != right) {
      return left < right ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace xiaotu
