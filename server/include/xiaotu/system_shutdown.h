#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace xiaotu {

// 客户端地址是否为回环地址：只接受 127.0.0.1、::1 与 IPv4 映射形式
// ::ffff:127.0.0.1；域名（如 localhost）、私网地址与空串一律拒绝。
bool is_loopback_address(std::string_view address);

// 优雅关闭用的一次性令牌：进程启动时生成 32 字节随机数的 hex 写入文件（权限 0600），
// 校验成功后删除文件并失效，之后的校验一律失败；令牌只在本进程生命周期内有效。
class ShutdownToken {
 public:
  // 生成新令牌并覆盖写入 file，已存在的旧文件随之失效；返回是否成功写出。
  bool issue(const std::filesystem::path& file);

  // 常量时间比较；成功时删除令牌文件并标记已用。
  bool consume(std::string_view candidate);

  bool issued() const;
  std::string file_path() const;

 private:
  mutable std::mutex mutex_;
  std::string token_;
  std::filesystem::path file_;
  bool used_ = false;
};

}  // namespace xiaotu
