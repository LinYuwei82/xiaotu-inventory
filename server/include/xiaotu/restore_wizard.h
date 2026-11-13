#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace xiaotu {

// 网页向导发起的恢复请求。落盘到 <data_dir>/.restore-request.json，
// 进程退出后由下次启动在打开数据库之前执行。
struct RestoreRequestInfo {
  std::string file;    // 备份文件名（不含目录）
  std::string source;  // 绝对路径；只写盘，不返回给前端
  std::string actor;   // 发起恢复的登录用户
  std::string origin;  // 审计来源，固定为「网页向导」
  std::string requested_at;
};

// 恢复执行结果。写在 <data_dir>/.restore-result.json，供界面在重启后展示。
struct RestoreOutcome {
  bool ran = false;
  bool ok = false;
  std::string file;
  std::string at;
  int schema_version = 0;
  std::string safety_backup;  // 恢复前安全备份文件名
  std::string error_code;
  std::string error_message;
};

bool write_restore_request(const std::filesystem::path& data_dir,
                           const RestoreRequestInfo& request, std::string& error);
std::optional<RestoreRequestInfo> read_restore_request(const std::filesystem::path& data_dir);
void clear_restore_request(const std::filesystem::path& data_dir);

void write_restore_result(const std::filesystem::path& data_dir, const RestoreOutcome& outcome);
std::optional<RestoreOutcome> read_restore_result(const std::filesystem::path& data_dir);
void clear_restore_result(const std::filesystem::path& data_dir);

// 启动时调用（必须在打开数据库之前）：有请求就复用 restore_database 执行。
// 成功与失败都会写结果文件并删除请求文件；失败时 restore_database 已负责回滚。
std::optional<RestoreOutcome> apply_pending_restore(const std::filesystem::path& data_dir,
                                                    const std::filesystem::path& database_path,
                                                    const std::filesystem::path& backup_dir);

}  // namespace xiaotu
