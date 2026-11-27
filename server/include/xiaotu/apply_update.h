#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace xiaotu {

// Windows 自动更新协助器：壳启动自身（server）副本跑 --apply-update，
// 等壳退出后同步完成安装/解压替换，无论成败都按原参数重启壳。
// 设计参照 devcard 已验证模式（server/src/apply_update.cpp）。
struct ApplyUpdateOptions {
  std::filesystem::path package;     // 已双重校验的安装包（.exe 或 .zip）
  unsigned long wait_pid = 0;        // 等这个进程退出后再开始安装（避免文件占用）
  std::filesystem::path result;      // 安装结果 JSON（原子写）
  std::filesystem::path relaunch;    // 装完/装失败后各拉起一次（同一路径）
  std::vector<std::string> relaunch_args;  // 原进程参数（不含 argv[0]）
};

// 读 PE 版本资源里的 ProductVersion；读不到返回空串。
std::string read_product_version(const std::filesystem::path& exe);

// 以分离进程启动协助器（SW_HIDE，默认 CreateProcessW 标志）。
// 失败返回 false 并填 err，不抛异常（壳侧统一处理）。
bool spawn_apply_update(const std::filesystem::path& helper,
                        const ApplyUpdateOptions& options,
                        std::string* err);

// 协助器主体：等父进程退出 → 记录升级前版本 → 跑安装器/解压替换
// → 记录升级后版本 → 原子写结果 JSON → 拉起目标程序。
// 返回进程退出码（安装成败写在 options.result 里，不靠本进程退出码表达）。
int run_apply_update(const ApplyUpdateOptions& options);

// 四态：apply-state.json 只记这四态；interrupted 由 installing 超阈值推导。
enum class ApplyState { Installing, Verifying, Done, Failed };
constexpr int kApplyInterruptSeconds = 600;  // 比 devcard 的 300s 宽松，覆盖大 zip 解压

const char* apply_state_name(ApplyState state);

bool write_apply_state(const std::filesystem::path& data_dir, ApplyState state,
                       const std::string& detail, std::string* error);

struct ApplyStateSnapshot {
  bool present = false;
  std::string state;
  std::string detail;
  std::string updated_at;
  long long age_seconds = 0;
};

ApplyStateSnapshot read_apply_state(const std::filesystem::path& data_dir);
bool apply_state_interrupted(const ApplyStateSnapshot& snapshot,
                             int threshold_seconds = kApplyInterruptSeconds);

}  // namespace xiaotu
