#pragma once

#include <cstdint>
#include <string>

namespace xiaotu {

class Conn;
class Db;

// 单机版本机口令与「本机免输」共用的设置读写与凭据签发逻辑。
// 路由在 local_auth_routes.cpp，状态读写集中在这里，避免多处各写一份 SQL。

// app_setting 里「本机免输」开关的键。
inline constexpr const char* kLocalRememberSettingKey = "local_remember_enabled";

std::string read_setting_value(Conn& conn, const char* key);

void write_setting_value(Conn& conn, const char* key, const std::string& value);

// 默认 30 天免输；只有显式写 '0' 才表示「每次都要输」。
bool local_remember_enabled(Db& db);

bool has_local_remember_token(Db& db);

// 钥匙串里放服务端签发的随机 token，这里只存哈希与时间。
struct IssuedRememberToken {
  std::string token;
  std::string expires_at;
};

// 同一时刻只保留一枚有效凭据：签发新凭据即让旧凭据失效。
IssuedRememberToken issue_local_remember_token(Db& db, std::int64_t user_id,
                                               const std::string& username);

void clear_local_remember_tokens(Db& db, std::int64_t user_id, const std::string& username,
                                 const char* detail);

// 到期与时钟回拨都在服务端判断，失败原因写入 failure。
bool validate_local_remember_token(Db& db, const std::string& token, std::string* failure);

}  // namespace xiaotu
