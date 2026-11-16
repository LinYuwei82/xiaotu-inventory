#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xiaotu {

// 验证更新清单的 Ed25519 签名。
//
// 输入：
//   body             已解析的清单 JSON（必须包含 "signature" 字段）
//   public_key_base64 公钥的 base64 原始 32 字节（与 update_manifest.py 的 --pub 语义一致）
//
// 规范化规则与 tools/package/update_manifest.py 的 canonical_bytes 字节级一致：
//   去掉 "signature" 字段 → JSON 键排序 → 无空白 separators → UTF-8 编码
//
// 返回：
//   true  验签通过
//   false 验签失败（签名无效 / 字段缺失 / 算法不可用 / 公钥格式错误）
//
// 失败原因写进 log_warn；返回值只表达"可信 / 不可信"，不暴露具体原因给调用方。
bool verify_manifest_signature(const nlohmann::json& body,
                               const std::string& public_key_base64);

// 与上面同一个规范化函数，单独暴露供单测使用（C++ 侧与 Python 侧对齐）。
// 去掉 "signature" 字段后按键排序、无多余空白、UTF-8 编码的 JSON 字节。
std::string canonical_json_bytes(const nlohmann::json& body);

}  // namespace xiaotu
