// core/update_engine 编排层回归（重构方案 P0）：
// 假依赖驱动状态机全路径 + 五条内置规则 + 1.0.15 三个线上 bug 的回归用例。
// 不触网、不依赖真实哈希/安装器，全平台可跑。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "xiaotu/update_engine.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

constexpr const char* kSetupUrl = "https://dl.example.com/xiaotu/xiaotu-setup.exe";
constexpr const char* kSha =
    "8888888888888888888888888888888888888888888888888888888888888888";
constexpr const char* kSha2 =
    "7777777777777777777777777777777777777777777777777777777777777777";
constexpr std::int64_t kPayloadSize = 4096;

// v2 统一清单：windows/setup 段是引擎要拍平的字段。
std::string make_manifest(std::string_view version, std::int64_t build, std::string_view sha,
                          std::int64_t size) {
  nlohmann::json body{
      {"manifest_version", 2}, {"product", "xiaotu"}, {"channel", "stable"},
      {"key_id", "2026-11"},   {"updated_at", "2026-11-20T00:00:00Z"},
      {"signature", "c2lnbmF0dXJl"}};
  body["platforms"]["windows"] = {
      {"version", std::string(version)},
      {"build", build},
      {"min_os", "10.0"},
      {"rollout_percent", 100},
      {"artifacts",
       {{"setup",
         {{"url", kSetupUrl}, {"size", size}, {"sha256", std::string(sha)}}}}}};
  return body.dump();
}

// ---- 假依赖 ----

struct FakeClock : xiaotu::IClock {
  std::int64_t epoch = 1700000000;
  std::int64_t now_epoch() const override { return epoch; }
};

struct FakeStore : xiaotu::IStore {
  std::string data;
  int saves = 0;
  bool load(std::string& out, std::string&) override {
    out = data;
    return !data.empty();
  }
  bool save(const std::string& json, std::string&) override {
    data = json;
    ++saves;
    return true;
  }
};

struct FakeHasher : xiaotu::IHasher {
  std::string hex;  // 对任意文件返回同一 hex，模拟「文件内容 = 该哈希」
  bool fail = false;
  bool sha256_file(const fs::path&, std::string& out, std::string& err) override {
    if (fail) {
      err = "HASH_FAIL";
      return false;
    }
    out = hex;
    return true;
  }
};

struct FakeTransport : xiaotu::ITransport {
  std::string manifest_body;
  bool fetch_ok = true;
  std::string fetch_err = "NETWORK";
  std::string payload;
  bool download_ok = true;
  std::string download_err = "CONN_RESET";
  bool leave_part_on_fail = false;  // 模拟失败时残留半截 .part
  int fetch_count = 0;
  int download_count = 0;
  std::string snapshot_at_half;     // 首个进度回调后的持久化快照（规则 4）
  std::function<void()> mid_download;  // 下载中途钩子（触发 cancel 用）
  FakeStore* store = nullptr;

  bool fetch(const std::string&, std::string& body, std::string& err) override {
    ++fetch_count;
    if (!fetch_ok) {
      err = fetch_err;
      return false;
    }
    body = manifest_body;
    return true;
  }

  bool download(const std::string&, const fs::path& dest,
                const xiaotu::ProgressFn& on_progress, const xiaotu::CancelFn& cancel,
                std::string& err) override {
    ++download_count;
    fs::path part = dest;
    part += ".part";
    if (!download_ok) {
      if (leave_part_on_fail) {
        std::ofstream f(part, std::ios::binary);
        f.write(payload.data(), static_cast<std::streamsize>(payload.size() / 2));
      }
      err = download_err;
      return false;
    }
    {
      std::ofstream f(part, std::ios::binary);
      f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    const auto half = static_cast<std::int64_t>(payload.size() / 2);
    const auto total = static_cast<std::int64_t>(payload.size());
    on_progress(half, total);
    if (store) snapshot_at_half = store->data;  // 进度必须已落盘
    if (mid_download) mid_download();
    if (cancel()) {
      err = "CANCELED";
      return false;
    }
    std::error_code ec;
    fs::rename(part, dest, ec);
    if (ec) {
      err = "RENAME_FAILED";
      return false;
    }
    on_progress(total, total);
    return true;
  }
};

struct FakeInstaller : xiaotu::IInstaller {
  int calls = 0;
  xiaotu::PendingPackage last;
  bool start_ok = true;
  std::string fail_error;
  xiaotu::InstallResult install(const xiaotu::PendingPackage& pkg) override {
    ++calls;
    last = pkg;
    if (!start_ok) return {false, "FAILED", fail_error};
    return {true, "STARTED", ""};
  }
};

// 一套假依赖 + 临时目录 + 引擎。current=1.4.2/100，清单=1.4.3/2026111801。
struct Harness {
  static inline int seq = 0;
  fs::path dir;
  FakeClock clock;
  FakeStore store;
  FakeTransport transport;
  FakeHasher hasher;
  FakeInstaller installer;
  xiaotu::EngineConfig cfg;
  std::unique_ptr<xiaotu::UpdateEngine> engine;

  Harness() {
    dir = fs::temp_directory_path() / ("xiaotu_engine_smoke_" + std::to_string(seq++));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    cfg.manifest_url = "https://dl.example.com/xiaotu/manifest.json";
    cfg.platform = "windows";
    cfg.channel = "stable";
    cfg.install_id = "install-id-fixed";
    cfg.current_version = "1.4.2";
    cfg.current_build = 100;
    cfg.manual_cooldown_seconds = 300;
    cfg.auto_check_interval_seconds = 3600;
    cfg.download_dir = dir / "dl";
    cfg.os_version = "11.0";
    transport.manifest_body = make_manifest("1.4.3", 2026111801, kSha, kPayloadSize);
    transport.payload = std::string(static_cast<std::size_t>(kPayloadSize), 'A');
    transport.store = &store;
    hasher.hex = kSha;
    engine = std::make_unique<xiaotu::UpdateEngine>(deps(), cfg);
    engine->load();
  }

  ~Harness() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  xiaotu::UpdateEngine::Deps deps() {
    return {&transport, &hasher, &installer, &clock, &store};
  }

  // 换一个引擎实例读同一份 store，模拟进程重启。
  std::unique_ptr<xiaotu::UpdateEngine> restart(xiaotu::IInstaller* inst = nullptr) {
    auto d = deps();
    if (inst) d.installer = inst;
    auto e = std::make_unique<xiaotu::UpdateEngine>(d, cfg);
    e->load();
    return e;
  }

  nlohmann::json status() const { return engine->status(); }

  fs::path pending_file() const {
    const auto p = status().value("pending_path", "");
    return p.empty() ? fs::path() : fs::path(p);
  }
};

// 走通「检查 → 下载完成」，返回 harness 供后续断言。
void drive_to_done(Harness& h) {
  check(h.engine->check(false), "drive_to_done: check ok");
  check(h.engine->download_pending(), "drive_to_done: download ok");
}

// ---- 用例 ----

// 全路径：IDLE → CHECKING → UPDATE_AVAILABLE → DOWNLOADING → DONE → INSTALLING。
void test_happy_path() {
  Harness h;
  check(h.status()["state"] == "IDLE", "happy: 初始 IDLE");

  check(h.engine->check(false), "happy: check 返回判定");
  auto s = h.status();
  check(s["state"] == "UPDATE_AVAILABLE", "happy: 检查后 UPDATE_AVAILABLE");
  check(s["version"] == "1.4.3", "happy: version 拍平");
  check(s["build"] == 2026111801, "happy: build 拍平");
  check(s["bytes_total"] == kPayloadSize, "happy: bytes_total 来自清单");
  check(s["last_result"] == "UPDATE_AVAILABLE", "happy: last_result");

  check(h.engine->download_pending(), "happy: 下载成功");
  s = h.status();
  check(s["state"] == "DONE", "happy: 下载后 DONE");
  check(s["pending_size"] == kPayloadSize, "happy: pending_size");
  check(s["bytes_done"] == kPayloadSize, "happy: bytes_done 收口");
  check(fs::exists(h.pending_file()), "happy: 包文件在盘上");
  check(!fs::exists(fs::path(h.pending_file().string() + ".part")), "happy: .part 已改名");
  check(s["last_result"] == "DOWNLOADED", "happy: last_result=DOWNLOADED");

  const auto res = h.engine->try_install();
  check(res.started, "happy: 安装启动");
  check(res.code == "STARTED", "happy: code=STARTED");
  check(h.installer.calls == 1, "happy: installer 调了一次");
  check(h.installer.last.build == 2026111801, "happy: pkg.build 透传");
  check(h.installer.last.sha256 == kSha, "happy: pkg.sha256 透传");
  check(h.status()["state"] == "INSTALLING", "happy: 安装后 INSTALLING");
}

// 规则 1（1.0.15 bug：清单换版后陈旧 pending 不清除 → 误装旧包）。
void test_rule1_manifest_sha_change() {
  Harness h;
  drive_to_done(h);
  const fs::path old_file = h.pending_file();
  check(fs::exists(old_file), "rule1: 旧包已下载");

  // 服务端换包：同版本号但 sha 变了。
  h.transport.manifest_body = make_manifest("1.4.3", 2026111801, kSha2, kPayloadSize);
  h.hasher.hex = kSha2;
  h.clock.epoch += 3601;  // 越过自动检查间隔
  check(h.engine->check(false), "rule1: 重查成功");

  auto s = h.status();
  check(s["state"] == "UPDATE_AVAILABLE", "rule1: pending 作废回 UPDATE_AVAILABLE");
  check(s["pending_path"] == "", "rule1: pending_path 已清");
  check(s["pending_size"] == 0, "rule1: pending_size 已清");
  check(!fs::exists(old_file), "rule1: 旧包文件已删除");

  // 重新下载新包后校验用的是新 sha。
  check(h.engine->download_pending(), "rule1: 新包下载成功");
  check(h.status()["state"] == "DONE", "rule1: 新包 DONE");
}

// 规则 2（1.0.15 bug：安装前不重校验 → 坏包照跑、程序先自杀）。
void test_rule2_verify_before_install() {
  Harness h;
  drive_to_done(h);
  check(h.status()["state"] == "DONE", "rule2: 先到 DONE");

  // 模拟包在盘上被破坏（哈希对不上）。
  h.hasher.hex = std::string(64, 'f');
  const auto res = h.engine->try_install();
  check(!res.started, "rule2: 拒绝安装");
  check(res.code == "VERIFY_FAILED", "rule2: code=VERIFY_FAILED");
  check(h.installer.calls == 0, "rule2: installer 没被调用");
  check(h.status()["state"] == "UPDATE_AVAILABLE", "rule2: 回到可重下状态");
  check(!fs::exists(h.pending_file()) || h.status()["pending_path"] == "",
        "rule2: 坏包已清");
}

// 规则 3：下载失败/取消必须删掉半截文件。
void test_rule3_partial_cleanup() {
  {
    Harness h;
    check(h.engine->check(false), "rule3: check ok");
    h.transport.download_ok = false;
    h.transport.leave_part_on_fail = true;
    check(!h.engine->download_pending(), "rule3: 下载失败");
    const fs::path dest = h.cfg.download_dir / "xiaotu-setup.exe";
    check(!fs::exists(dest), "rule3: dest 不存在");
    check(!fs::exists(fs::path(dest.string() + ".part")), "rule3: 半截 .part 已删");
    auto s = h.status();
    check(s["state"] == "FAILED", "rule3: 状态 FAILED");
    check(s["last_result"] == "DOWNLOAD_FAILED", "rule3: last_result");
    check(s["last_error"] == "CONN_RESET", "rule3: last_error 透传");
  }
  {
    // 取消分支：回到 UPDATE_AVAILABLE，同样不留半截文件。
    Harness h;
    check(h.engine->check(false), "rule3-cancel: check ok");
    h.transport.mid_download = [&h] { h.engine->cancel_download(); };
    check(!h.engine->download_pending(), "rule3-cancel: 下载被取消");
    const fs::path dest = h.cfg.download_dir / "xiaotu-setup.exe";
    check(!fs::exists(dest) && !fs::exists(fs::path(dest.string() + ".part")),
          "rule3-cancel: 文件已清理");
    auto s = h.status();
    check(s["state"] == "UPDATE_AVAILABLE", "rule3-cancel: 回 UPDATE_AVAILABLE");
    check(s["last_result"] == "CANCELED", "rule3-cancel: last_result=CANCELED");
  }
}

// 规则 4（1.0.15 bug：download_progress 不落盘 → 重启后显示「0 B」）。
void test_rule4_progress_persisted() {
  Harness h;
  drive_to_done(h);

  // 下载中途（首个进度回调后）持久化文件里就必须有非零 bytes_done。
  check(!h.transport.snapshot_at_half.empty(), "rule4: 进度回调时已落盘");
  const auto mid = nlohmann::json::parse(h.transport.snapshot_at_half);
  check(mid["bytes_done"] == kPayloadSize / 2, "rule4: 中途进度已落盘");
  check(mid["state"] == "DOWNLOADING", "rule4: 中途状态已落盘");

  // 重启后数字不丢。
  auto h2 = h.restart();
  auto s = h2->status();
  check(s["state"] == "DONE", "rule4: 重启后仍 DONE");
  check(s["bytes_done"] == kPayloadSize, "rule4: 重启后 bytes_done 不是 0");
  check(s["pending_size"] == kPayloadSize, "rule4: 重启后 pending_size 还在");
}

// 手动冷却 + 失败退避（规则 5：全部走注入时钟）。
void test_cooldown_and_backoff() {
  Harness h;
  check(h.engine->check(true), "cooldown: 首次手动检查放行");
  check(h.transport.fetch_count == 1, "cooldown: 拉了一次清单");

  h.clock.epoch += 10;
  check(!h.engine->check(true), "cooldown: 冷却期内手动检查被挡");
  check(h.transport.fetch_count == 1, "cooldown: 被挡时不拉网络");
  check(h.status()["retry_after"] > 0, "cooldown: retry_after 非零");
  check(h.status()["last_result"] == "COOLDOWN", "cooldown: last_result=COOLDOWN");

  h.clock.epoch += 300;
  check(h.engine->check(true), "cooldown: 冷却结束放行");
  check(h.transport.fetch_count == 2, "cooldown: 第二次真正拉清单");

  // 失败退避：1→60s，2→600s，3→3600s（check_retry_delay 口径）。
  h.transport.fetch_ok = false;
  h.clock.epoch += 3601;
  check(!h.engine->check(false), "backoff: 检查失败");
  auto s = h.status();
  check(s["consecutive_failures"] == 1, "backoff: 失败计数 1");
  check(s["next_check_epoch"] == h.clock.epoch + 60, "backoff: 首次退避 60s");

  h.clock.epoch += 61;
  check(!h.engine->check(false), "backoff: 第二次失败");
  check(h.status()["next_check_epoch"] == h.clock.epoch + 600, "backoff: 第二次退避 600s");

  h.clock.epoch += 601;
  check(!h.engine->check(false), "backoff: 第三次失败");
  check(h.status()["next_check_epoch"] == h.clock.epoch + 3600, "backoff: 第三次退避 3600s");

  // 恢复后计数清零。
  h.transport.fetch_ok = true;
  h.clock.epoch += 3601;
  check(h.engine->check(false), "backoff: 恢复成功");
  check(h.status()["consecutive_failures"] == 0, "backoff: 计数清零");
  check(h.status()["state"] == "UPDATE_AVAILABLE", "backoff: 回到 UPDATE_AVAILABLE");

  // 已下载好的包不能因一次检查失败而丢：状态保持 DONE。
  check(h.engine->download_pending(), "backoff: 下载完成");
  h.transport.fetch_ok = false;
  h.clock.epoch += 3601;
  check(!h.engine->check(false), "backoff: DONE 后检查失败");
  check(h.status()["state"] == "DONE", "backoff: DONE 保持不被检查失败打回");
}

// 1.0.15 同类 bug 回归：装完后残留 pending 显示「待安装 X」。
void test_stale_pending_cleared_on_load() {
  Harness h;
  drive_to_done(h);
  const std::string saved = h.store.data;

  // 新版本已装上：本机升到 1.4.3/2026111801，重启加载旧状态。
  h.cfg.current_version = "1.4.3";
  h.cfg.current_build = 2026111801;
  auto e = h.restart();
  auto s = e->status();
  check(s["state"] == "IDLE", "stale: 装完后回 IDLE");
  check(s["version"] == "", "stale: 残留 manifest 已清");
  check(s["pending_path"] == "", "stale: 残留 pending 已清");
  (void)saved;
}

// 崩溃恢复：下载/安装中途被杀，重启后状态可重试。
void test_crash_recovery() {
  {
    Harness h;
    check(h.engine->check(false), "crash: check ok");
    // 手工构造「下载中被杀」的持久化（进度已落盘一半）。
    nlohmann::json j = nlohmann::json::parse(h.store.data);
    j["state"] = "DOWNLOADING";
    j["bytes_done"] = kPayloadSize / 2;
    h.store.data = j.dump();
    auto e = h.restart();
    auto s = e->status();
    check(s["state"] == "UPDATE_AVAILABLE", "crash: DOWNLOADING → UPDATE_AVAILABLE");
    check(s["bytes_done"] == 0, "crash: 中断进度清零");
  }
  {
    Harness h;
    drive_to_done(h);
    nlohmann::json j = nlohmann::json::parse(h.store.data);
    j["state"] = "INSTALLING";
    h.store.data = j.dump();
    auto e = h.restart();
    check(e->status()["state"] == "DONE", "crash: INSTALLING → DONE");
  }
  {
    // 包文件丢了 → 回到待下载。
    Harness h;
    drive_to_done(h);
    std::error_code ec;
    fs::remove(h.pending_file(), ec);
    auto e = h.restart();
    auto s = e->status();
    check(s["state"] == "UPDATE_AVAILABLE", "crash: 包丢失回 UPDATE_AVAILABLE");
    check(s["pending_path"] == "", "crash: pending 已清");
  }
}

// 跳过版本：1–30 天合法；跳过期内不再提示更新。
void test_skip_flow() {
  Harness h;
  check(h.engine->check(false), "skip: check ok");
  check(!h.engine->skip_current(0), "skip: 0 天非法");
  check(!h.engine->skip_current(31), "skip: 31 天非法");
  check(h.engine->skip_current(7), "skip: 7 天生效");

  auto s = h.status();
  check(s["state"] == "IDLE", "skip: 回 IDLE");
  check(s["skipped_version"] == "1.4.3", "skip: 记录被跳版本");
  check(s["pending_path"] == "", "skip: pending 已清");

  h.clock.epoch += 3601;
  check(h.engine->check(false), "skip: 重查得到判定");
  s = h.status();
  check(s["last_result"] == "SKIPPED", "skip: 判定 SKIPPED");
  check(s["state"] == "IDLE", "skip: 保持 IDLE");
  check(s["version"] == "", "skip: 不保留待更新信息");
}

// 安装入口守卫：无 pending / 无安装器 / 安装启动失败。
void test_install_guards() {
  {
    Harness h;
    const auto res = h.engine->try_install();
    check(!res.started && res.code == "NO_PENDING_UPDATE", "guard: 无 pending");
    check(h.installer.calls == 0, "guard: 无 pending 不调安装器");
  }
  {
    Harness h;
    drive_to_done(h);
    // 无安装器端口：UNAVAILABLE（decide_install 优先返回不可用）。
    xiaotu::UpdateEngine::Deps d = h.deps();
    d.installer = nullptr;
    xiaotu::UpdateEngine no_installer(d, h.cfg);
    no_installer.load();
    const auto res = no_installer.try_install();
    check(!res.started && res.code == "UNAVAILABLE", "guard: 无安装器端口");
  }
  {
    Harness h;
    drive_to_done(h);
    h.installer.start_ok = false;
    h.installer.fail_error = "SHELL_FAILED";
    const auto res = h.engine->try_install();
    check(!res.started, "guard: 启动失败");
    check(h.status()["state"] == "DONE", "guard: 失败回 DONE 可重试");
    check(h.status()["last_error"] == "SHELL_FAILED", "guard: 错误透传");
  }
}

// 统一 status_json 字段集合快照（前端契约，方案 2.4）。
void test_status_json_field_snapshot() {
  Harness h;
  const auto s = h.status();
  const std::set<std::string> expected = {
      "auto_check",              "bytes_done",            "bytes_total",
      "build",                   "channel",               "consecutive_failures",
      "enabled",                 "install_id",            "last_check_epoch",
      "last_error",              "last_manual_check_epoch", "last_result",
      "manifest_setup_url",      "manifest_sha256",       "manifest_build",
      "manifest_version",        "mandatory",
      "mandatory_deadline",      "mandatory_reason",      "next_check_epoch",
      "notes_url",               "pending_path",          "pending_size",
      "prefetch",                "retry_after",           "skipped_version",
      "skip_until_epoch",        "state",                 "version",
  };
  std::set<std::string> actual;
  for (auto it = s.begin(); it != s.end(); ++it) actual.insert(it.key());
  check(actual == expected, "snapshot: status_json 字段集合与契约一致");
}

// 状态名 ↔ 枚举往返（持久化字符串的稳定性）。
void test_state_names() {
  for (auto st : {xiaotu::UpdateState::kIdle, xiaotu::UpdateState::kChecking,
                  xiaotu::UpdateState::kUpdateAvailable, xiaotu::UpdateState::kDownloading,
                  xiaotu::UpdateState::kDone, xiaotu::UpdateState::kFailed,
                  xiaotu::UpdateState::kInstalling}) {
    xiaotu::UpdateState back{};
    check(xiaotu::update_state_from_name(xiaotu::update_state_name(st), back) && back == st,
          "names: 状态名往返一致");
  }
  xiaotu::UpdateState junk{};
  check(!xiaotu::update_state_from_name("WHATEVER", junk), "names: 非法状态名被拒");
}

}  // namespace

int main() {
  test_happy_path();
  test_rule1_manifest_sha_change();
  test_rule2_verify_before_install();
  test_rule3_partial_cleanup();
  test_rule4_progress_persisted();
  test_cooldown_and_backoff();
  test_stale_pending_cleared_on_load();
  test_crash_recovery();
  test_skip_flow();
  test_install_guards();
  test_status_json_field_snapshot();
  test_state_names();

  if (failures == 0) {
    std::cout << "update_engine_smoke: all checks passed" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
