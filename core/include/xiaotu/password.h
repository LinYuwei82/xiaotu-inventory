#pragma once

#include <string>
#include <string_view>

namespace xiaotu {

// Argon2id 口令散列（官方参考实现内嵌）。散列串自带参数与盐，可直接存库。
std::string hash_password(std::string_view password);

// 散列为空、格式非法或口令不匹配都返回 false，不抛异常。
bool verify_password(std::string_view encoded, std::string_view password);

// 口令长度约束：8–128 个字符。
bool password_acceptable(std::string_view password);

}  // namespace xiaotu
