#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

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
  // replace 表示已定位可替换的新程序（stage）或已启动替换脚本（schedule）。
  std::string mode;
};

// 安装包在本地下载目录中的固定文件名：<version>-<build>-<URL 末段>。
// 路由与安装动作都用它定位文件，避免接受任意路径。
std::filesystem::path update_package_path(const std::filesystem::path& directory,
                                          const UpdateManifest& manifest);

// 删除下载目录内的安装包：keep 为空时清空全部，否则只保留同名文件。
// 用于「已是最新」时回收磁盘，以及下载新版本前删除旧待装包（单包策略）。
void remove_update_packages(const std::filesystem::path& directory,
                            const std::filesystem::path& keep = {});

// 用系统 curl 下载清单 url 指向的安装包到 directory：
//   1. 下载前删除目录内其它旧包与同名残留文件；
//   2. 清单给出 size 时先比对字节数；
//   3. 再调用验签工具带 --file 同时校验清单签名与安装包 sha256。
// cancel 置位时终止 curl、删除半成品并返回 UPDATE_DOWNLOAD_CANCELED。
// 任一校验失败都不保留文件，并写日志（含验签工具输出）。
UpdatePackageResult download_update_package(const UpdateSettings& settings,
                                            const std::string& manifest_file,
                                            const UpdateManifest& manifest,
                                            const std::filesystem::path& directory,
                                            const std::atomic<bool>* cancel = nullptr);

// 当前进程可执行文件的绝对路径：Windows 用 GetModuleFileNameW，macOS 用
// _NSGetExecutablePath，Linux 读 /proc/self/exe。取不到时返回空路径。
std::filesystem::path current_executable_path();

// 自动替换所需的运行信息。args 不含 argv[0]；重启时按原参数拉起。
struct UpdateApplyOptions {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::filesystem::path working_dir;
  std::filesystem::path log_file;
  // 0 表示替换脚本等待当前进程；测试可传固定值。
  long long pid = 0;
};

// 安装前复验，并定位真正要替换的可执行文件：
//   - 包本身是可执行文件（Windows .exe / POSIX 无扩展名或 .bin）时直接使用；
//   - .zip 包先解压到 staging_dir，再按「与当前可执行文件同名」查找。
// 找不到载荷返回 UPDATE_PACKAGE_PAYLOAD_MISSING，不猜测其它文件。
UpdatePackageResult stage_update_package(const UpdateSettings& settings,
                                         const std::string& manifest_file,
                                         const std::filesystem::path& package,
                                         const std::filesystem::path& staging_dir,
                                         const std::filesystem::path& executable);

// 生成「等待旧进程退出 → 备份 → 覆盖 → 用原参数重启」的脚本文本。
// 拆成纯函数便于测试转义与失败回滚分支。
std::string update_replace_script(const UpdateApplyOptions& options,
                                  const std::filesystem::path& new_executable);

// 写入替换脚本并在后台启动：立即返回，脚本等本进程退出后完成替换与重启。
// 成功返回后调用方应尽快优雅退出；失败时不会改动当前程序。
UpdatePackageResult schedule_update_replace(const std::filesystem::path& new_executable,
                                            const UpdateApplyOptions& options,
                                            const std::filesystem::path& staging_dir);

}  // namespace xiaotu
