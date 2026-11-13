#pragma once

#include <string>
#include <vector>

namespace xiaotu {

// 子进程执行结果。output 合并 stdout 与 stderr，按行保留原始文本；
// 失败原因（如程序不存在）放在 error，便于拼进用户可见的错误信息。
struct ProcessResult {
  // 进程是否真的创建成功（CreateProcess / fork 成功）。
  bool started = false;
  // 是否成功取到退出状态；started 为真时才可能为真。二者都为真时
  // exit_code 才有意义，started 为真而 collected 为假表示收集失败。
  bool collected = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

// 不经 shell 执行程序：程序与参数分开传入，路径含空格、引号时按平台规则转义。
// 标准输出与标准错误通过管道读回，避免只拿到退出码却说不清失败原因。
// Windows 走 CreateProcess + 管道；POSIX 走 fork/execvp + 管道。
ProcessResult run_process_capture(const std::string& program,
                                  const std::vector<std::string>& args);

// 统一描述失败原因：未启动时给系统错误描述；已启动但收集失败时明确说明
// 进程确实起来了，只是拿不到退出状态，避免被写成「无法启动」。
std::string process_failure_text(const ProcessResult& result);

// 清理 Windows FormatMessageW 在 IGNORE_INSERTS 下留下的占位符与转义：
// %1–%9（含多位编号）、%n/%r/%t、%%；随后压缩空白、去掉空括号与句首/句尾
// 孤立标点。返回空串表示清理后没有可用描述，调用方应只显示系统错误码。
// 该函数不依赖 Windows 头文件，便于在任意平台做单测。
std::string clean_windows_error_message(const std::string& text);

// 取输出里最后一个非空行，用于错误信息；没有输出时返回空串。
std::string last_output_line(const std::string& output);

// 把多行输出压成单行并限制长度，避免把整段堆栈写进接口响应或日志。
std::string compact_output(const std::string& output, std::size_t max_bytes = 600);

}  // namespace xiaotu
