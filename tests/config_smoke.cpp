// config.toml 子集解析的冒烟测试：正常读取、注释、类型、错误提示。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "xiaotu/config.h"
#include "xiaotu/error.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::filesystem::path write_temp(const std::string& name, const std::string& content) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("xiaotu_config_" + name + ".toml");
  std::ofstream out(path);
  out << content;
  return path;
}

std::string error_code_of(const std::filesystem::path& path) {
  try {
    xiaotu::load_config_file(path);
  } catch (const xiaotu::AppError& error) {
    return error.code();
  }
  return "";
}

}  // namespace

int main() {
  const std::filesystem::path valid = write_temp(
      "valid",
      "# 顶部注释\n"
      "[server]\n"
      "listen = \"127.0.0.1\"   # 行尾注释\n"
      "port = 18099\n"
      "[storage]\n"
      "data_dir = \"/mnt/data#1\"\n"
      "backup_dir = \"/mnt/backups\"\n"
      "[log]\n"
      "level = \"DEBUG\"\n"
      "dir = \"/var/log/xiaotu\"\n"
      "[update]\n"
      "auto_check = false\n"
      "prefetch = true\n"
      "manifest_url = \"https://dl.xiaotu.online/xiaotu/updates/stable/latest.json\"\n");
  const xiaotu::ConfigValues values = xiaotu::load_config_file(valid);
  check(values.listen.has_value() && *values.listen == "127.0.0.1", "读取 server.listen");
  check(values.port.has_value() && *values.port == 18099, "读取 server.port");
  check(values.data_dir.has_value() && *values.data_dir == "/mnt/data#1",
        "字符串内的 # 不被当作注释");
  check(values.backup_dir.has_value() && *values.backup_dir == "/mnt/backups",
        "读取 storage.backup_dir");
  check(values.update_auto_check.has_value() && *values.update_auto_check == false,
        "读取 update.auto_check");
  check(values.update_prefetch.has_value() && *values.update_prefetch == true,
        "读取 update.prefetch");
  check(values.update_manifest_url.has_value() &&
            *values.update_manifest_url ==
                "https://dl.xiaotu.online/xiaotu/updates/stable/latest.json",
        "读取 update.manifest_url");
  {
    const xiaotu::ConfigValues empty =
        xiaotu::load_config_file(write_temp("backup_empty", "[storage]\nbackup_dir = \"\"\n"));
    check(empty.backup_dir.has_value() && empty.backup_dir->empty(),
          "storage.backup_dir 空串合法（表示默认目录）");
    const xiaotu::ConfigValues relative = xiaotu::load_config_file(
        write_temp("backup_relative", "[storage]\nbackup_dir = \"bak/copies\"\n"));
    check(relative.backup_dir.has_value() && *relative.backup_dir == "bak/copies",
          "storage.backup_dir 相对路径按原值保存");
  }
  check(values.log_level.has_value() && *values.log_level == "debug", "日志级别转小写");
  check(values.log_dir.has_value() && *values.log_dir == "/var/log/xiaotu", "读取 log.dir");

  check(error_code_of(write_temp("unknown_key", "[server]\nportx = 1\n")) == "INVALID_CONFIG",
        "未知键报错");
  check(error_code_of(write_temp("unknown_section", "[cache]\ndir = \"/tmp\"\n")) ==
            "INVALID_CONFIG",
        "未知段报错");
  check(error_code_of(write_temp("duplicate", "[server]\nport = 1\nport = 2\n")) ==
            "INVALID_CONFIG",
        "重复键报错");
  check(error_code_of(write_temp("range", "[server]\nport = 70000\n")) == "INVALID_CONFIG",
        "端口越界报错");
  check(error_code_of(write_temp("no_section", "port = 1\n")) == "INVALID_CONFIG",
        "缺段名报错");
  check(error_code_of(write_temp("bad_string", "[server]\nlisten = 127.0.0.1\n")) ==
            "INVALID_CONFIG",
        "字符串缺少引号报错");
  check(error_code_of(write_temp("bad_bool", "[update]\nauto_check = 1\n")) ==
            "INVALID_CONFIG",
        "布尔值只接受 true / false");
  check(error_code_of(write_temp("insecure_url", "[update]\nmanifest_url = \"http://x\"\n")) ==
            "INVALID_CONFIG",
        "update.manifest_url 必须使用 https");
  check(error_code_of(std::filesystem::temp_directory_path() / "xiaotu_config_missing.toml") ==
            "NOT_FOUND",
        "文件不存在报错");

  for (const auto& name : {"valid", "unknown_key", "unknown_section", "duplicate", "range",
                           "no_section", "bad_string", "bad_bool", "backup_empty",
                           "backup_relative", "insecure_url"}) {
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::temp_directory_path() /
                                ("xiaotu_config_" + std::string(name) + ".toml"),
                            ignored);
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "config_smoke 通过" << std::endl;
  return 0;
}
