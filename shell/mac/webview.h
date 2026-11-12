#pragma once

#include <string>

namespace xiaotu::platform {

// 用系统 WebView 打开本地页面，阻塞直到窗口关闭。
int run_webview(const std::string& url, const std::string& title);

}  // namespace xiaotu::platform
