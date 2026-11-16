#include "xiaotu/update_signature.h"

#include <openssl/evp.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// 测试用的 Ed25519 密钥对：进程内用 OpenSSL EVP 生成并签名，不调用 openssl 命令行、
// 不写临时文件，因此 POSIX 与 Windows（MSVC）跑同一份用例。
struct KeyPair {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
  std::string pub_base64;  // 公钥 32 字节 raw 的 base64
};

namespace {

void fail_and_exit(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  std::exit(1);
}

std::string base64_encode(const unsigned char* data, std::size_t len) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) v |= static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? kTable[(v >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? kTable[v & 0x3F] : '=');
  }
  return out;
}

}  // namespace

KeyPair gen_test_keypair() {
  KeyPair kp;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  if (!ctx || EVP_PKEY_keygen_init(ctx) != 1) {
    EVP_PKEY_CTX_free(ctx);
    fail_and_exit("Ed25519 密钥生成初始化失败");
  }
  EVP_PKEY* generated = nullptr;
  const int rc = EVP_PKEY_keygen(ctx, &generated);
  EVP_PKEY_CTX_free(ctx);
  if (rc != 1) fail_and_exit("Ed25519 密钥生成失败");
  kp.key.reset(generated);

  std::size_t pub_len = 0;
  if (EVP_PKEY_get_raw_public_key(kp.key.get(), nullptr, &pub_len) != 1 ||
      pub_len != 32) {
    fail_and_exit("读取公钥长度失败");
  }
  std::vector<unsigned char> pub(pub_len);
  if (EVP_PKEY_get_raw_public_key(kp.key.get(), pub.data(), &pub_len) != 1) {
    fail_and_exit("导出公钥失败");
  }
  kp.pub_base64 = base64_encode(pub.data(), pub.size());
  if (kp.pub_base64.size() != 44) {
    fail_and_exit("公钥 base64 长度不是 44（实际 " +
                  std::to_string(kp.pub_base64.size()) + "）");
  }
  return kp;
}

// 用 Ed25519 对任意字节串签名，返回 base64（64 字节签名 → 88 字符）。
std::string sign_bytes(EVP_PKEY* key, const std::vector<unsigned char>& msg) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) fail_and_exit("EVP_MD_CTX_new 失败");

  std::size_t sig_len = 0;
  bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestSign(ctx, nullptr, &sig_len, msg.data(), msg.size()) == 1;
  if (!ok) {
    EVP_MD_CTX_free(ctx);
    fail_and_exit("Ed25519 签名长度查询失败");
  }
  std::vector<unsigned char> sig(sig_len);
  ok = EVP_DigestSign(ctx, sig.data(), &sig_len, msg.data(), msg.size()) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok) fail_and_exit("Ed25519 签名失败");
  sig.resize(sig_len);
  return base64_encode(sig.data(), sig.size());
}

int main() {
  int fails = 0;

  // === 准备：生成测试密钥对 ===
  KeyPair kp = gen_test_keypair();
  std::cout << "Test pubkey b64: " << kp.pub_base64 << "\n";

  // === Case 1: 正确签名通过验签 ===
  // 构造一份带 signature 占位字段的 manifest（验签前该字段会被剔除）。
  nlohmann::json manifest = {
      {"manifest_version", 1},
      {"key_id", "2026-11"},
      {"platform", "macos"},
      {"channel", "stable"},
      {"version", "0.9.0"},
      {"build", 2026111601},
      {"url", "https://oss.example.com/macos/0.9.0/xiaotu-inventory-0.9.0.dmg"},
      {"sha256",
       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
      {"size", 3400000},
      {"published_at", "2026-11-16T17:00:00Z"},
      {"schema_min", 1},
      {"schema_max", 39},
      {"api_min", 1},
      {"api_max", 1},
      {"rollout_percent", 100},
      {"signature", ""}  // 占位，签名由本用例在进程内完成
  };

  std::string canonical = xiaotu::canonical_json_bytes(manifest);
  std::vector<unsigned char> msg_bytes(canonical.begin(), canonical.end());
  std::string sig_b64 = sign_bytes(kp.key.get(), msg_bytes);
  manifest["signature"] = sig_b64;

  bool ok = xiaotu::verify_manifest_signature(manifest, kp.pub_base64);
  if (!ok) {
    std::cerr << "FAIL Case 1: 正确签名未通过 C++ 验签\n";
    std::cerr << "Canonical payload: " << canonical << "\n";
    fails++;
  } else {
    std::cout << "PASS Case 1: 正确签名\n";
  }

  // === Case 2: 篡改后验签失败 ===
  manifest["version"] = "0.9.1";      // 改一个字段
  manifest["signature"] = "";         // 清空
  manifest["signature"] = "AAAABBBBCCCCDDDD";  // 假签名
  ok = xiaotu::verify_manifest_signature(manifest, kp.pub_base64);
  if (ok) {
    std::cerr << "FAIL Case 2: 篡改签名不应通过\n";
    fails++;
  } else {
    std::cout << "PASS Case 2: 篡改签名正确拒绝\n";
  }

  // === Case 3: 不同公钥验签失败 ===
  KeyPair kp2 = gen_test_keypair();
  // 还原正确签名
  manifest["version"] = "0.9.0";
  manifest["signature"] = sig_b64;
  ok = xiaotu::verify_manifest_signature(manifest, kp2.pub_base64);
  if (ok) {
    std::cerr << "FAIL Case 3: 错误公钥不应通过\n";
    fails++;
  } else {
    std::cout << "PASS Case 3: 错误公钥正确拒绝\n";
  }

  // === Case 4: 缺 signature 字段 ===
  nlohmann::json manifest_nosig = manifest;
  manifest_nosig.erase("signature");
  ok = xiaotu::verify_manifest_signature(manifest_nosig, kp.pub_base64);
  if (ok) {
    std::cerr << "FAIL Case 4: 缺 signature 不应通过\n";
    fails++;
  } else {
    std::cout << "PASS Case 4: 缺 signature 正确拒绝\n";
  }

  // === Case 5: 规范化与 Python 字节级一致 ===
  // 期望值由 Python 生成（与 tools/package/update_manifest.py 同规则）：
  //   json.dumps(body_without_signature, separators=(',', ':'), sort_keys=True,
  //              ensure_ascii=False)
  // 固定成常量后无需在测试机上装 Python，Windows 也能跑同一条断言。
  static const char* kExpectedCanonical =
      R"({"api_max":1,"api_min":1,"build":2026111601,"channel":"stable",)"
      R"("key_id":"2026-11","manifest_version":1,"platform":"macos",)"
      R"("published_at":"2026-11-16T17:00:00Z","rollout_percent":100,)"
      R"("schema_max":39,"schema_min":1,)"
      R"("sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
      R"("size":3400000,)"
      R"("url":"https://oss.example.com/macos/0.9.0/xiaotu-inventory-0.9.0.dmg",)"
      R"("version":"0.9.0"})";
  if (canonical != kExpectedCanonical) {
    std::cerr << "FAIL Case 5: C++ 规范化与 Python 不一致\n";
    std::cerr << "CPP: " << canonical << "\n";
    std::cerr << "PY : " << kExpectedCanonical << "\n";
    fails++;
  } else {
    std::cout << "PASS Case 5: C++ 规范化与 Python 字节级一致\n";
  }

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES: " + std::to_string(fails))
            << "\n";
  return fails == 0 ? 0 : 1;
}
