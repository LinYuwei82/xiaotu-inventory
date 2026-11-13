#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "xiaotu/update_install.h"
#include "xiaotu/update_service.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

// 验签替身：marker 存在时向 stderr 输出一句「哈希不符」并退出 1，
// 否则静默退出 0。真实 sha256 由 tools/package 的 Python 单测覆盖。
std::filesystem::path write_verifier_stub(const std::filesystem::path& directory,
                                          const std::filesystem::path& marker) {
#if defined(_WIN32)
  const auto path = directory / "verify-stub.bat";
  std::ofstream out(path);
  out << "@echo off\r\n"
      << "if exist \"" << marker.string() << "\" (\r\n"
      << "  echo sha256 不符：测试桩 1>&2\r\n"
      << "  exit /b 1\r\n"
      << ")\r\n"
      << "exit /b 0\r\n";
  return path;
#else
  const auto path = directory / "verify-stub.sh";
  {
    std::ofstream out(path);
    out << "#!/bin/sh\n"
        << "if [ -f '" << marker.string() << "' ]; then\n"
        << "  echo 'sha256 不符：测试桩' >&2\n"
        << "  exit 1\n"
        << "fi\n"
        << "exit 0\n";
  }
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add);
  return path;
#endif
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "xiaotu-update-download-test";
  fs::remove_all(root);
  fs::create_directories(root);

  const std::string payload(1024, 'x');
  httplib::Server server;
  server.Get("/pkg.dmg",
             [&payload](const httplib::Request&, httplib::Response& res) {
               res.set_content(payload, "application/octet-stream");
             });
  server.Get("/missing.dmg",
             [](const httplib::Request&, httplib::Response& res) { res.status = 404; });
  const int port = server.bind_to_any_port("127.0.0.1");
  if (port <= 0) {
    std::cerr << "FAIL: 无法监听本地测试端口" << std::endl;
    return 1;
  }
  std::thread serving([&server]() { server.listen_after_bind(); });

  const fs::path marker = root / "reject.marker";
  const fs::path public_key = root / "update.pub.pem";
  const fs::path script = root / "update_manifest.py";
  std::ofstream(public_key) << "test-public-key";
  std::ofstream(script) << "# test verifier";
  const fs::path stub = write_verifier_stub(root, marker);

  xiaotu::UpdateSettings settings;
  settings.manifest_file = (root / "latest.json").string();
  settings.public_key_file = public_key.string();
  settings.verifier_script = script.string();
  settings.verifier_python = stub.string();

  xiaotu::UpdateManifest manifest;
  manifest.version = "9.9.9";
  manifest.build = 2026111301;
  manifest.url = "http://127.0.0.1:" + std::to_string(port) + "/pkg.dmg";
  manifest.size = static_cast<std::int64_t>(payload.size());
  manifest.sha256 = "0123456789abcdef";

  const fs::path download_dir = root / "updates";
  xiaotu::UpdatePackageResult result =
      xiaotu::download_update_package(settings, settings.manifest_file, manifest, download_dir);
  check(result.ok, "下载并通过校验后返回成功");
  check(fs::exists(result.path), "成功时保留安装包");
  check(result.bytes == payload.size(), "成功时字节数与下载内容一致");
  check(result.path.filename().string().find("9.9.9-2026111301-") == 0,
        "安装包文件名带版本与构建号");

  // 验签失败：删除文件、错误码稳定、错误信息带工具输出。
  std::ofstream(marker) << "reject";
  manifest.build = 2026111302;
  result = xiaotu::download_update_package(settings, settings.manifest_file, manifest, download_dir);
  check(!result.ok && result.error_code == "UPDATE_SIGNATURE_INVALID",
        "校验失败返回 UPDATE_SIGNATURE_INVALID");
  check(result.error_message.find("sha256 不符") != std::string::npos,
        "校验失败信息包含验签工具输出");
  check(!fs::exists(result.path), "校验失败不保留下载文件");
  fs::remove(marker);

  // 大小不符：下载后先拒绝，不进入验签。
  manifest.build = 2026111303;
  manifest.size = 10;
  result = xiaotu::download_update_package(settings, settings.manifest_file, manifest, download_dir);
  check(!result.ok && result.error_code == "UPDATE_SIZE_MISMATCH", "大小不符时拒绝");
  check(!fs::exists(result.path), "大小不符时不保留文件");

  // 下载失败：HTTP 404 不留下半截文件。
  manifest.build = 2026111304;
  manifest.size = static_cast<std::int64_t>(payload.size());
  manifest.url = "http://127.0.0.1:" + std::to_string(port) + "/missing.dmg";
  result = xiaotu::download_update_package(settings, settings.manifest_file, manifest, download_dir);
  check(!result.ok && result.error_code == "UPDATE_DOWNLOAD_FAILED", "HTTP 404 返回下载失败");
  check(!fs::exists(result.path), "下载失败不保留文件");

  server.stop();
  serving.join();
  fs::remove_all(root);
  if (failures == 0) {
    std::cout << "update_download_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
