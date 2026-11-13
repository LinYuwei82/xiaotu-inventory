#pragma once

#include <filesystem>
#include <string>

#include "xiaotu/update.h"
#include "xiaotu/update_service.h"

namespace xiaotu {

struct UpdatePackageResult {
  bool ok = false;
  std::string error_code;
  std::string error_message;
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
  std::string sha256;
  // launch 时返回：manual 表示只打开安装包，由用户完成安装。
  std::string mode;
};

// 安装包在本地下载目录中的固定文件名：<version>-<build>-<URL 末段>。
// 路由与安装动作都用它定位文件，避免接受任意路径。
std::filesystem::path update_package_path(const std::filesystem::path& directory,
                                          const UpdateManifest& manifest);

// 用系统 curl 下载清单 url 指向的安装包到 directory：
//   1. 下载前删除同名残留文件，下载失败删除半截文件；
//   2. 清单给出 size 时先比对字节数；
//   3. 再调用验签工具带 --file 同时校验清单签名与安装包 sha256。
// 任一校验失败都不保留文件，并写日志（含验签工具输出）。
UpdatePackageResult download_update_package(const UpdateSettings& settings,
                                            const std::string& manifest_file,
                                            const UpdateManifest& manifest,
                                            const std::filesystem::path& directory);

// 安装前再次校验安装包，然后交给系统打开：
//   macOS 用 open（挂载 dmg 或打开安装器），Windows 用 cmd /c start。
// 只打开安装包，不关闭本进程：替换正在运行的二进制与占用中的数据库不安全，
// 是否退出由用户确认后自行执行（见 docs/更新模块技术方案.md 第 6 节）。
UpdatePackageResult launch_update_package(const UpdateSettings& settings,
                                          const std::string& manifest_file,
                                          const std::filesystem::path& package);

}  // namespace xiaotu
