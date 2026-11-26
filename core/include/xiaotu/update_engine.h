#pragma once

// core/update_engine：平台无关的更新编排层（见 docs/更新模块拆分重构方案.md 2.2）。
//
// 职责：状态机 + 持久化 schema + 进度聚合 + 统一 status_json。
// 判定逻辑全部复用 core/update 的纯函数（evaluate_manifest / 冷却 / 退避 / 安装判定），
// 平台 I/O（下载、哈希、安装）与时间、存储通过下面的接口注入，两壳只提供薄端口实现。
//
// 引擎内置五条规则（1.0.15 三连修的回归点）：
//   1. 清单 sha256 变化 → 作废旧 pending，状态回退，防误装旧包；
//   2. 任何进入 INSTALLING 前 → 重校验大小 + SHA256，防坏包自杀式安装；
//   3. 下载失败/取消 → 引擎删除 dest 与 dest.part，不留半截文件；
//   4. bytes_done 与 pending_size 同步落盘，重启后不再显示「0 B」；
//   5. 冷却/退避计时全部走注入的 IClock，可测。
//
// 线程约定：check()/download_pending()/try_install() 由壳的后台线程串行调用；
// status_json() 任意线程可调（内部加锁，慢 I/O 期间不持锁）。

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "xiaotu/update.h"

namespace xiaotu {

// ---- 依赖注入接口（壳端口实现） ----

// 下载进度回调：已写字节 / 总字节（total 未知时为 0）。
using ProgressFn = std::function<void(std::int64_t bytes_done, std::int64_t bytes_total)>;
// 取消查询回调：返回 true 表示调用方要求中断下载。
using CancelFn = std::function<bool()>;

// 待安装包（引擎校验通过后交给 IInstaller 的描述）。
struct PendingPackage {
  std::filesystem::path path;
  std::int64_t size = 0;
  std::string sha256;
  std::string version;
  std::int64_t build = 0;
};

// install 结果。code：STARTED / UNAVAILABLE / NO_PENDING_UPDATE / VERIFY_FAILED / FAILED。
struct InstallResult {
  bool started = false;
  std::string code;
  std::string error;
};

// 拉清单 / 下载产物。download 的实现契约：写 dest.part，成功后原子改名为 dest；
// 失败时允许留下 .part，由引擎统一删除（规则 3）。
class ITransport {
 public:
  virtual ~ITransport() = default;
  virtual bool fetch(const std::string& url, std::string& body, std::string& err) = 0;
  virtual bool download(const std::string& url, const std::filesystem::path& dest,
                        const ProgressFn& on_progress, const CancelFn& cancel,
                        std::string& err) = 0;
};

// 文件 SHA256（hex 小写）。
class IHasher {
 public:
  virtual ~IHasher() = default;
  virtual bool sha256_file(const std::filesystem::path& p, std::string& hex,
                           std::string& err) = 0;
};

// 平台安装语义全部关在实现里：mac 挂载替换自重启、win ShellExecute /VERYSILENT。
// 「何时允许安装」的判定与安装前重校验由引擎负责，这里只管「怎么装」。
class IInstaller {
 public:
  virtual ~IInstaller() = default;
  virtual InstallResult install(const PendingPackage& pkg) = 0;
};

// UTC 秒。
class IClock {
 public:
  virtual ~IClock() = default;
  virtual std::int64_t now_epoch() const = 0;
};

// 状态持久化：引擎只关心一个 JSON 字符串（schema 与 status_json 相同）。
class IStore {
 public:
  virtual ~IStore() = default;
  virtual bool load(std::string& json, std::string& err) = 0;
  virtual bool save(const std::string& json, std::string& err) = 0;
};

// ---- 状态机 ----

enum class UpdateState {
  kIdle,             // 空闲（无待更新）
  kChecking,         // 正在拉取/判定清单
  kUpdateAvailable,  // 有新版本，待下载
  kDownloading,      // 下载中
  kDone,             // 包已下载并通过校验，待安装
  kFailed,           // 检查/下载/校验失败，带 retry_after
  kInstalling,       // 安装已启动（进程即将退出）
};

const char* update_state_name(UpdateState state);
bool update_state_from_name(std::string_view name, UpdateState& out);

// ---- 配置 ----

struct EngineConfig {
  std::string manifest_url;
  std::string platform = "windows";  // windows / macos
  std::string channel = "stable";
  std::string install_id;            // 灰度分桶用，壳负责生成并持久化
  // 本机版本；留空/非正时回落到二进制编译值（current_product_version / current_build_number）。
  std::string current_version;
  std::int64_t current_build = 0;
  // 手动检查冷却（秒），走 manual_check_retry_after。
  std::int64_t manual_cooldown_seconds = 300;
  // 自动检查间隔（秒）；检查成功后 next_check_epoch = now + 该值。
  std::int64_t auto_check_interval_seconds = 6 * 3600;
  // 检查成功后 next_check_epoch 的 ±抖动（秒）；0 表示不抖动。
  std::int64_t auto_check_jitter_seconds = 0;
  // 从未检查过时的首轮延迟（秒）；<=0 表示立即可查。
  std::int64_t first_check_delay_seconds = 0;
  std::filesystem::path download_dir;
  // 清单验签公钥（base64）；为空表示跳过验签（仅测试场景）。
  std::string public_key_base64;
  // 判定透传字段（UpdateQuery 用）。
  std::string os_version;
  int schema_version = 0;
  int api_version = 0;
  // 忽略清单 min_os 拦截：旧 win 壳 v2 路径不查系统版本，端口置 true 保持行为。
  bool ignore_min_os = false;
  // 开关位（仅随状态落盘透传给前端，引擎自身不消费）。
  bool enabled = true;
  bool auto_check = true;
  bool prefetch = true;
};

// ---- 引擎 ----

class UpdateEngine {
 public:
  struct Deps {
    ITransport* transport = nullptr;
    IHasher* hasher = nullptr;
    IInstaller* installer = nullptr;
    IClock* clock = nullptr;
    IStore* store = nullptr;
  };

  UpdateEngine(Deps deps, EngineConfig cfg);
  ~UpdateEngine();  // Persisted 是不完整类型，析构必须落在 cpp

  // 启动后注入清单地址与频道（壳 start() 调用）；空值不覆盖现值。线程安全。
  void configure(const std::string& manifest_url, const std::string& channel);
  // 运行期开关（setOptions 桥调用）：写内存并立即落盘。
  void set_options(bool enabled, bool auto_check, bool prefetch);

  // 从 IStore 恢复状态；顺带做崩溃恢复（DOWNLOADING→UPDATE_AVAILABLE、
  // INSTALLING→DONE）与「已安装覆盖的残留 pending」清理。
  void load();

  // 检查清单。manual=true 受手动冷却约束；失败按 check_retry_delay 退避。
  // 返回是否得到一次有效的版本判定（false=被冷却/退避挡住或检查失败）。
  bool check(bool manual);

  // 下载并校验待更新包（UPDATE_AVAILABLE / FAILED 状态可发起；DONE 直接返回 true）。
  bool download_pending();

  // 请求安装。进入 INSTALLING 前强制重校验大小 + SHA256（规则 2）。
  InstallResult try_install();

  // 请求中断进行中的下载（transport 的 cancel 回调会读到 true）。
  void cancel_download();

  // 跳过当前待更新版本 N 天（1–30）。返回是否生效。
  bool skip_current(int days);
  // 跳过指定版本 N 天（1–30）：作废当前 pending 并记 SKIPPED。
  // mac 壳的 skipVersion 桥带显式版本串，用它保持旧口径。
  bool skip_version(const std::string& version, int days);
  // 清空下次检查时间，让下一次自动检查立即可跑（resume 桥用）。
  void invalidate_schedule();

  // 唯一对外状态口径（字段集合见 update_engine_smoke 的快照断言）。
  std::string status_json() const;
  nlohmann::json status() const;

  UpdateState state() const;

 private:
  struct Persisted;  // 落盘字段集合，藏在 cpp 里

  std::int64_t now() const;
  std::string cur_version() const;
  std::int64_t cur_build() const;
  UpdateQuery build_query_locked() const;
  std::string build_status_json_locked() const;
  void persist_locked();
  void clear_pending_locked();
  void clear_manifest_locked();
  // pending_path（UTF-8 窄串）对应的文件是否还在盘上。调用方需持锁。
  bool pending_on_disk_locked() const;
  // 校验包文件：存在 + 大小 + SHA256。失败时删除文件并返回 false。
  // 不持锁调用（大文件哈希期间不能挡住 status_json）。
  bool verify_pending(const std::filesystem::path& path, std::int64_t expect_size,
                      const std::string& expect_sha, std::string& reason) const;
  void fail_check_locked(std::int64_t now_epoch, const std::string& error);

  Deps deps_;
  EngineConfig cfg_;
  mutable std::mutex mutex_;
  std::atomic<bool> cancel_flag_{false};
  std::shared_ptr<Persisted> st_;  // shared_ptr：const 方法里也要加锁读
};

}  // namespace xiaotu
