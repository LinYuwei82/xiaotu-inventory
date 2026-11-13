#pragma once

#include <string>

#include "xiaotu/update.h"

namespace xiaotu {

// 网络版/私有化部署的更新源设置。清单由部署侧同步到本地文件，
// 服务端只做解析、兼容判断与灰度判定，不在此处发起 HTTPS 请求。
struct UpdateSettings {
  std::string manifest_file;
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
};

// now_epoch 为 UTC 秒；纯函数，读取 manifest_file 后返回判定结果。
UpdateCheckResult check_for_update(const UpdateSettings& settings, const UpdateQuery& query,
                                   std::int64_t now_epoch);

}  // namespace xiaotu
