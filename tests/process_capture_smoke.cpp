#include "xiaotu/process_capture.h"

#include <iostream>
#include <string>

// 用不存在的程序名启动子进程，验证失败原因可用。
// Windows：CreateProcessW 直接失败，必须 started=false 且带系统错误码；
// POSIX：fork 成功后由子进程 execvp 失败，按既有约定以 127 退出。
int main() {
  int failures = 0;
  const xiaotu::ProcessResult result =
      xiaotu::run_process_capture("xiaotu-no-such-program-20261113", {"--version"});

#if defined(_WIN32)
  if (result.started) {
    std::cerr << "不存在的程序不应启动成功" << std::endl;
    ++failures;
  }
  if (result.error.empty()) {
    std::cerr << "启动失败必须给出原因" << std::endl;
    ++failures;
  }
  // 纯文件名找不到时 CreateProcessW 稳定返回 ERROR_FILE_NOT_FOUND(2)，
  // 断言到具体的系统错误码，防止以后又把错误码取晚或拼错文案。
  if (result.error.find("系统错误 2") == std::string::npos) {
    std::cerr << "错误码应为 ERROR_FILE_NOT_FOUND(2)，实际：" << result.error << std::endl;
    ++failures;
  }
#else
  if (!result.started || result.exit_code != 127) {
    std::cerr << "exec 失败应以 127 退出，实际 started=" << result.started
              << " exit_code=" << result.exit_code << std::endl;
    ++failures;
  }
#endif

  if (failures == 0) {
    std::cout << "process_capture_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
