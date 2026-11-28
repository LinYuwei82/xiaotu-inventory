#pragma once

#include <httplib.h>

#include <string>

namespace xiaotu {

// HTTP 请求相关的通用小工具：Cookie 读写、写方法判定与本机来源校验。
// 由认证中间件与登录/关机等路由共用，避免这些判断散落在多个文件里。

std::string cookie_value(const httplib::Request& req, const std::string& name);

// 会话续期与登录时下发 Cookie，属性必须保持一致。
std::string cookie_attributes(const httplib::Request& req, int max_age_seconds);

bool state_changing(const std::string& method);

// 登录前就放行的本机端点只应由本机页面调用：Origin/Referer 若存在必须指向当前服务。
bool local_page_request(const httplib::Request& req, int port, const std::string& mode);

// 关闭端点的回环门禁：带 X-Forwarded-For 时同时校验代理追加的最后一跳。
bool shutdown_from_local_origin(const httplib::Request& req);

}  // namespace xiaotu
