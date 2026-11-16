#include "xiaotu/update_signature.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// 用 OpenSSL 临时生成 Ed25519 密钥对（测试专用）。
// 返回私钥 PEM 路径和公钥 base64（32 字节 raw）。
struct KeyPair {
  std::string priv_pem;   // 临时私钥 PEM 文件路径
  std::string pub_base64; // 公钥 32 字节 raw 的 base64
};

KeyPair gen_test_keypair(const std::string& tmp_dir) {
  KeyPair kp;
  kp.priv_pem = tmp_dir + "/test_ed25519.pem";
  // 生成私钥
  std::string cmd = "openssl genpkey -algorithm ED25519 -out " + kp.priv_pem +
                    " 2>/dev/null";
  int r = std::system(cmd.c_str());
  if (r != 0) {
    std::cerr << "FAIL: openssl genpkey rc=" << r << "\n";
    exit(1);
  }
  // 提取 raw 公钥 32 字节并 base64
  cmd = "openssl pkey -in " + kp.priv_pem +
        " -pubout -outform DER 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null"
        " | tail -c 32 | base64";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) { std::cerr << "FAIL: popen\n"; exit(1); }
  char buf[128];
  std::string b64;
  while (fgets(buf, sizeof(buf), f)) b64 += buf;
  pclose(f);
  // trim 换行
  while (!b64.empty() && (b64.back() == '\n' || b64.back() == ' ')) b64.pop_back();
  kp.pub_base64 = b64;
  if (kp.pub_base64.size() != 44) {
    std::cerr << "FAIL: pub key b64 wrong size " << kp.pub_base64.size()
              << " (expected 44)\n";
    exit(1);
  }
  return kp;
}

// 用 openssl 对任意消息（字节串）做 Ed25519 签名，返回 base64
std::string sign_bytes(const std::string& priv_pem, const std::vector<unsigned char>& msg) {
  // 把 msg 写到临时文件，用 openssl pkeyutl sign，再 base64
  std::string tmp_in = "/tmp/xiaotu_test_sign_in.bin";
  std::string tmp_sig = "/tmp/xiaotu_test_sign_out.bin";
  FILE* f = fopen(tmp_in.c_str(), "wb");
  fwrite(msg.data(), 1, msg.size(), f);
  fclose(f);
  std::string cmd = "openssl pkeyutl -sign -inkey " + priv_pem + " -rawin -in " +
                    tmp_in + " -out " + tmp_sig +
                    " 2>/dev/null && base64 < " + tmp_sig + " | tr -d '\\n'";
  FILE* p = popen(cmd.c_str(), "r");
  char buf[256];
  std::string b64;
  while (fgets(buf, sizeof(buf), p)) b64 += buf;
  pclose(p);
  while (!b64.empty() && b64.back() == '\n') b64.pop_back();
  return b64;
}

int main() {
  int fails = 0;

  // === 准备：生成测试密钥对 ===
  KeyPair kp = gen_test_keypair("/tmp");
  std::cout << "Test pubkey b64: " << kp.pub_base64 << "\n";

  // === Case 1: 规范化与 Python 对齐 ===
  // 构造一份带 signature 字段的 manifest（Python 会去掉它再签名）
  nlohmann::json manifest = {
    {"manifest_version", 1},
    {"key_id", "2026-11"},
    {"platform", "macos"},
    {"channel", "stable"},
    {"version", "0.9.0"},
    {"build", 2026111601},
    {"url", "https://oss.example.com/macos/0.9.0/xiaotu-inventory-0.9.0.dmg"},
    {"sha256", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
    {"size", 3400000},
    {"published_at", "2026-11-16T17:00:00Z"},
    {"schema_min", 1},
    {"schema_max", 39},
    {"api_min", 1},
    {"api_max", 1},
    {"rollout_percent", 100},
    {"signature", ""}  // 占位，签名由 Python 或 openssl 签
  };

  std::string canonical = xiaotu::canonical_json_bytes(manifest);
  // 用 openssl 对 canonical 字节签名
  std::vector<unsigned char> msg_bytes(canonical.begin(), canonical.end());
  std::string sig_b64 = sign_bytes(kp.priv_pem, msg_bytes);
  manifest["signature"] = sig_b64;

  // 调 C++ 验签
  bool ok = xiaotu::verify_manifest_signature(manifest, kp.pub_base64);
  if (!ok) {
    std::cerr << "FAIL Case 1: 正确签名未通过 C++ 验签\n";
    std::cerr << "Canonical payload: " << canonical << "\n";
    fails++;
  } else {
    std::cout << "PASS Case 1: 正确签名\n";
  }

  // === Case 2: 篡改后验签失败 ===
  manifest["version"] = "0.9.1";  // 改一个字段
  manifest["signature"] = "";
  std::string canonical2 = xiaotu::canonical_json_bytes(manifest);
  // 用 OLD canonical 签名 + NEW canonical 验签 → 应该失败
  // 或者：用篡改后的 canonical 签，但保留旧签名 → 应该失败
  // 更简单：直接篡改签名字段
  manifest["signature"] = "AAAABBBBCCCCDDDD";  // 假签名
  ok = xiaotu::verify_manifest_signature(manifest, kp.pub_base64);
  if (ok) {
    std::cerr << "FAIL Case 2: 篡改签名不应通过\n";
    fails++;
  } else {
    std::cout << "PASS Case 2: 篡改签名正确拒绝\n";
  }

  // === Case 3: 不同公钥验签失败 ===
  KeyPair kp2 = gen_test_keypair("/tmp");
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

  // === Case 5: 规范化与 Python 交叉验证 ===
  // 用 Python 直接对 C++ canonical 输出做比对，避免 shell 引号嵌套
  {
    std::ofstream ofs("/tmp/xiaotu_cpp_canonical.bin", std::ios::binary);
    ofs << canonical;
    ofs.close();
    std::ofstream json_ofs("/tmp/xiaotu_manifest.json");
    json_ofs << manifest.dump();
    json_ofs.close();
    std::string cmd =
        "python3 -c \"import json,sys; from pathlib import Path; "
        "body = json.loads(open('/tmp/xiaotu_manifest.json').read()); "
        "py = json.dumps({k:v for k,v in body.items() if k!='signature'}, "
        "separators=(',',':'), sort_keys=True, ensure_ascii=False); "
        "cpp = Path('/tmp/xiaotu_cpp_canonical.bin').read_text(); "
        "sys.exit(0 if py == cpp else 1)\" 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::cerr << "FAIL Case 5: C++ 规范化与 Python 不一致\n";
      std::string debug_cmd =
          "python3 -c \"import json; from pathlib import Path; "
          "body = json.loads(open('/tmp/xiaotu_manifest.json').read()); "
          "py = json.dumps({k:v for k,v in body.items() if k!='signature'}, "
          "separators=(',',':'), sort_keys=True, ensure_ascii=False); "
          "cpp = Path('/tmp/xiaotu_cpp_canonical.bin').read_text(); "
          "print('PY:', repr(py)); print('CPP:', repr(cpp)); print('same:', py==cpp)\"";
      std::system(debug_cmd.c_str());
      fails++;
    } else {
      std::cout << "PASS Case 5: C++ 规范化与 Python 字节级一致\n";
    }
  }

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES: " + std::to_string(fails)) << "\n";
  return fails == 0 ? 0 : 1;
}
