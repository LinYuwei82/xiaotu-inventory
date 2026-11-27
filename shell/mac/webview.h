#pragma once

#include <string>

namespace xiaotu::platform {

// 窗口先行（方案：启动速度优化 P1-3）：在 server 重初始化之前先建窗口并
// 显示 splash（短暂泵事件循环让首帧落地），随后 run_webview 复用同一窗口。
// 非 webview 构建或维护命令不需要调用；未调用时 run_webview 行为与旧版一致。
void show_splash(const std::string& title);

// 用系统 WebView 打开本地页面，阻塞直到窗口关闭。
int run_webview(const std::string& url, const std::string& title);

}  // namespace xiaotu::platform
