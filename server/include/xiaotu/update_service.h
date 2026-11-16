#pragma once

#include <string>

#include "xiaotu/update.h"

namespace xiaotu {

// 网络版/私有化部署的更新源设置。
// 两种清单来源：
//   - manifest_url 非空：优先从厂商更新源拉取，验签通过后写入 manifest_cache_file，
//     之后每次检查读缓存；拉取失败或缓存损坏时回退到上一次验签通过的缓存。
//   - manifest_file 非空：部署侧同步好的本地清单（内网/离线部署）。
// 两者都为空表示未启用更新检查。
struct UpdateSettings {
  std::string manifest_file;
  std::string manifest_url;
  std::string manifest_cache_file;
  // 远端清单的缓存有效期：有效期内直接读缓存，不重复访问外网。
  int manifest_ttl_seconds = 600;
  std::string channel = "stable";
  std::string min_supported_version;
  int rollout_percent = 100;
  // 清单 Ed25519 验签：公钥缺失时拒绝信任清单；验签由发布侧 Python 工具执行，
  // 避免在 C++ 内引入新的密码学依赖。
  std::string public_key_file;
  std::string verifier_python = "python3";
  std::string verifier_script;
};

struct UpdateCheckResult {
  bool configured = false;
  bool manifest_ok = false;
  std::string error_code;
  std::string error_message;
  UpdateManifest manifest;
  UpdateDecision decision;
  // 本次判定使用的清单来源：file（本地清单）/ network（刚拉取）/ cache（缓存或回退）。
  std::string manifest_source;
  // 实际读取并验签通过的清单文件；下载/安装复验时必须用它，不能再用配置里的本地路径。
  std::string manifest_path;
  // 远端拉取失败、改用旧缓存时为 true；仍会按旧清单给出更新结果。
  bool manifest_stale = false;
  std::string manifest_warning;
};

struct ManifestSyncResult {
  bool ok = false;
  std::string path;
  bool from_network = false;
  bool using_cache = false;
  std::string error_code;
  std::string error_message;
};

// 远端清单同步：manifest_url 非空时按 manifest_ttl_seconds 刷新到 manifest_cache_file。
// 下载先写同目录临时文件，验签通过后才原子替换缓存；失败时保留旧缓存并回退使用。
// manifest_url 为空时直接返回 manifest_file（不做任何 IO）。
ManifestSyncResult sync_update_manifest(const UpdateSettings& settings,
                                        std::int64_t now_epoch);

// now_epoch 为 UTC 秒。读取（必要时先刷新）清单后返回判定结果。
UpdateCheckResult check_for_update(const UpdateSettings& settings, const UpdateQuery& query,
                                   std::int64_t now_epoch);

// 安装包校验结果。verify_package 让验签工具同时校验清单签名与安装包 sha256。
struct PackageVerifyResult {
  bool ok = false;
  std::string error_code;
  std::string error_message;
};

// manifest_file 为已验签的清单文件，package_file 为下载到本地的安装包。
// 失败时 error_message 带验签工具的原始输出，便于定位是签名不符还是哈希不符。
PackageVerifyResult verify_package(const UpdateSettings& settings,
                                   const std::string& manifest_file,
                                   const std::string& package_file);

}  // namespace xiaotu
