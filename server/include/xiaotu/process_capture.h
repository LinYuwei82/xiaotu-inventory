#pragma once

#include <string>
#include <vector>

namespace xiaotu {

// 子进程执行结果。output 合并 stdout 与 stderr，按行保留原始文本；
// 失败原因（如程序不存在）放在 error，便于拼进用户可见的错误信息。
struct ProcessResult {
  bool started = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

// 不经 shell 执行程序：程序与参数分开传入，路径含空格、引号时按平台规则转义。
// 标准输出与标准错误通过管道读回，避免只拿到退出码却说不清失败原因。
// Windows 走 CreateProcess + 管道；POSIX 走 fork/execvp + 管道。
ProcessResult run_process_capture(const std::string& program,
                                  const std::vector<std::string>& args);

// 取输出里最后一个非空行，用于错误信息；没有输出时返回空串。
std::string last_output_line(const std::string& output);

// 把多行输出压成单行并限制长度，避免把整段堆栈写进接口响应或日志。
std::string compact_output(const std::string& output, std::size_t max_bytes = 600);

}  // namespace xiaotu
