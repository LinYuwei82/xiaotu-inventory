#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace xiaotu {

// 业务错误：code 供前端提示与日志检索，message 面向用户。
class AppError : public std::runtime_error {
 public:
  AppError(std::string code, std::string message)
      : std::runtime_error(message), code_(std::move(code)) {}

  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

}  // namespace xiaotu
