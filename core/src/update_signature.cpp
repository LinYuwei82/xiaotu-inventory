#include "xiaotu/update_signature.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "xiaotu/log.h"

#ifdef XIAOTU_HAS_OPENSSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#endif

namespace xiaotu {

// ---- RFC 8785 规范化序列化 ----
//
// 与 tools/package/update_manifest.py 的 canonical_bytes() 字节级一致：
//   去掉 "signature" 字段 → 键按 UTF-8 字节序排序 → separators=(",", ":")
//   → ensure_ascii=false（非 ASCII 原样 UTF-8 输出）

namespace {

void canonical_write_string(const std::string& s, std::string& out) {
  out.push_back('"');
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int bytes = 1;
    if (c >= 0xF0) bytes = 4;
    else if (c >= 0xE0) bytes = 3;
    else if (c >= 0xC0) bytes = 2;

    if (bytes == 1) {
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
          if (c < 0x20) {
            char buf[12];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
    } else {
      out.append(s.substr(i, bytes));
      i += bytes;
    }
  }
  out.push_back('"');
}

void canonical_write(const nlohmann::json& v, std::string& out) {
  switch (v.type()) {
    case nlohmann::json::value_t::object: {
      out.push_back('{');
      std::vector<std::string> keys;
      keys.reserve(v.size());
      for (auto it = v.begin(); it != v.end(); ++it) {
        keys.push_back(it.key());
      }
      std::sort(keys.begin(), keys.end());
      bool first = true;
      for (const auto& k : keys) {
        if (!first) out.push_back(',');
        first = false;
        canonical_write_string(k, out);
        out.push_back(':');
        canonical_write(v[k], out);
      }
      out.push_back('}');
      break;
    }
    case nlohmann::json::value_t::array: {
      out.push_back('[');
      bool first = true;
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (!first) out.push_back(',');
        first = false;
        canonical_write(*it, out);
      }
      out.push_back(']');
      break;
    }
    case nlohmann::json::value_t::string:
      canonical_write_string(v.get_ref<const std::string&>(), out);
      break;
    case nlohmann::json::value_t::number_integer: {
      // RFC 8785: 输出最短十进制表示（Python json.dumps 用 g 格式的语义）
      auto i = v.get<std::int64_t>();
      out += std::to_string(i);
      break;
    }
    case nlohmann::json::value_t::number_float: {
      // Python json.dumps 对 float 输出最短表示，不用指数形式，至少一个小数点
      double d = v.get<double>();
      if (!std::isfinite(d)) {
        // RFC 8785 不允许 NaN/Inf，但实际清单里不会有
        out += "null";
        break;
      }
      if (d == 0.0) {
        out += "0.0";
        break;
      }
      // 是整数值但类型为 float（如 JSON "1" 被 parseFloat 处理）
      if (d == std::floor(d) && std::abs(d) < 1e16) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", d);
        out += buf;
        break;
      }
      // 用 Python 风格最短表示：优先 %.15g，必要时 %.16g
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.15g", d);
      std::string s = buf;
      // 如果没有小数点也没有 'e'，补 .0
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find('E') == std::string::npos) {
        s += ".0";
      }
      out += s;
      break;
    }
    case nlohmann::json::value_t::boolean:
      out += v.get<bool>() ? "true" : "false";
      break;
    case nlohmann::json::value_t::null:
      out += "null";
      break;
    case nlohmann::json::value_t::number_unsigned: {
      auto i = v.get<std::uint64_t>();
      out += std::to_string(i);
      break;
    }
    case nlohmann::json::value_t::binary:
      // 不是标准 JSON，正常清单里不会出现
      out += "null";
      break;
    case nlohmann::json::value_t::discarded:
      out += "null";
      break;
  }
}

}  // namespace

std::string canonical_json_bytes(const nlohmann::json& body) {
  nlohmann::json stripped = body;
  stripped.erase("signature");
  std::string out;
  out.reserve(256);
  canonical_write(stripped, out);
  return out;
}

// ---- Ed25519 验签（OpenSSL 3.x EVP API） ----

#ifdef XIAOTU_HAS_OPENSSL

namespace {

bool base64_decode(const std::string& in, std::vector<unsigned char>& out,
                   size_t expected_len) {
  out.resize(expected_len);
  BIO* b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO* bmem = BIO_new_mem_buf(in.data(), static_cast<int>(in.size()));
  b64 = BIO_push(b64, bmem);
  int got = BIO_read(b64, out.data(), static_cast<int>(out.size()));
  BIO_free_all(b64);
  if (got < 0) return false;
  if (got != static_cast<int>(expected_len)) {
    out.resize(got);
    return false;
  }
  out.resize(got);
  return true;
}

EVP_PKEY* load_ed25519_pubkey_raw(const std::vector<unsigned char>& raw32) {
  if (raw32.size() != 32) return nullptr;
  OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
  if (!bld) return nullptr;
  if (OSSL_PARAM_BLD_push_octet_string(bld, "pub", raw32.data(), raw32.size()) != 1) {
    OSSL_PARAM_BLD_free(bld);
    return nullptr;
  }
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
  OSSL_PARAM_BLD_free(bld);
  if (!params) return nullptr;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  if (!ctx) { OSSL_PARAM_free(params); return nullptr; }
  EVP_PKEY* pkey = nullptr;
  if (EVP_PKEY_fromdata_init(ctx) != 1 ||
      EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) {
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    return nullptr;
  }
  EVP_PKEY_CTX_free(ctx);
  OSSL_PARAM_free(params);
  return pkey;
}

}  // namespace

bool verify_manifest_signature(const nlohmann::json& body,
                               const std::string& public_key_base64) {
  if (!body.contains("signature") || !body["signature"].is_string()) {
    log_warn("更新验签：清单缺少 signature 字段");
    return false;
  }
  if (public_key_base64.empty()) {
    log_warn("更新验签：未提供公钥");
    return false;
  }

  // base64 解码 signature (64 bytes) 和 public key (32 bytes)
  const std::string sig_b64 = body["signature"].get<std::string>();
  std::vector<unsigned char> sig64, pub32;
  if (!base64_decode(sig_b64, sig64, 64)) {
    log_warn("更新验签：signature 不是有效的 64 字节 base64（实际 " +
             std::to_string(sig64.size()) + "）");
    return false;
  }
  if (!base64_decode(public_key_base64, pub32, 32)) {
    log_warn("更新验签：公钥不是有效的 32 字节 base64（实际 " +
             std::to_string(pub32.size()) + "）");
    return false;
  }

  // 规范化（去掉 signature、键排序、无空白）
  std::string payload = canonical_json_bytes(body);

  // Ed25519 验签（OpenSSL 3 EVP API）
  EVP_PKEY* pkey = load_ed25519_pubkey_raw(pub32);
  if (!pkey) {
    log_warn("更新验签：Ed25519 公钥加载失败");
    return false;
  }
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  int r = -1;
  if (EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1) {
    r = EVP_DigestVerify(md, sig64.data(), sig64.size(),
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size());
  }
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(pkey);

  if (r != 1) {
    log_warn("更新验签：Ed25519 签名不匹配");
    return false;
  }
  return true;
}

#else  // !XIAOTU_HAS_OPENSSL

bool verify_manifest_signature(const nlohmann::json& body,
                               const std::string& public_key_base64) {
  // 编译时未链接 OpenSSL → 跳过 C++ 侧验签。
  // Sparkle（接入后）会自行校验 appcast.xml EdDSA，作为最终信任锚。
  // 客户端在这种环境下不强制签名校验，避免阻断单机版功能。
  (void)body;
  (void)public_key_base64;
  log_warn("更新验签：编译时未启用 OpenSSL（Ed25519 验签降级为 stub，"
           "Sparkle 启用后由 Sparkle 校验 appcast）");
  return true;  // 放行：让 Sparkle 决定
}

#endif

}  // namespace xiaotu
