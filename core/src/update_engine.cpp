#include "xiaotu/platform.h"
#include "xiaotu/update_engine.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <system_error>
#include <utility>

#include "xiaotu/update_signature.h"

namespace xiaotu {
namespace {

// 从 URL 取文件名（去掉 query/fragment）；取不到时给一个兜底名。
std::string basename_from_url(std::string_view url) {
  const auto frag = url.find_first_of("?#");
  const std::string_view path = frag == std::string_view::npos ? url : url.substr(0, frag);
  const auto slash = path.find_last_of('/');
  std::string name = slash == std::string_view::npos ? std::string(path)
                                                     : std::string(path.substr(slash + 1));
  if (name.empty()) name = "update.pkg";
  return name;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool hex_iequal(std::string_view a, std::string_view b) { return to_lower(a) == to_lower(b); }

void remove_quiet(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

std::filesystem::path part_path_of(const std::filesystem::path& p) {
  std::filesystem::path out = p;
  out += ".part";
  return out;
}

// ---- 持久化读取小工具：字段缺失或类型不符时保持原值 ----

void get_str(const nlohmann::json& j, const char* key, std::string& out) {
  if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
}

void get_i64(const nlohmann::json& j, const char* key, std::int64_t& out) {
  if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<std::int64_t>();
}

void get_int(const nlohmann::json& j, const char* key, int& out) {
  if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
}

void get_bool(const nlohmann::json& j, const char* key, bool& out) {
  if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
}

}  // namespace

// 落盘字段集合：status_json 与持久化共用同一 schema（统一口径，杜绝两套字段漂移）。
struct UpdateEngine::Persisted {
  UpdateState state = UpdateState::kIdle;
  std::string last_result;
  std::string last_error;
  std::int64_t last_check_epoch = 0;
  std::int64_t next_check_epoch = 0;
  int consecutive_failures = 0;
  std::string install_id;
  std::string channel;
  // 清单（待更新版本）
  std::string manifest_version;
  std::int64_t manifest_build = 0;
  std::string manifest_sha256;
  std::int64_t manifest_size = 0;
  std::string manifest_setup_url;
  std::string notes_url;
  bool mandatory = false;
  std::string mandatory_reason;
  std::string mandatory_deadline;
  // 下载
  std::int64_t bytes_done = 0;
  std::int64_t bytes_total = 0;
  std::string pending_path;
  std::int64_t pending_size = 0;
  // 跳过
  std::string skipped_version;
  std::int64_t skip_until_epoch = 0;
};

const char* update_state_name(UpdateState state) {
  switch (state) {
    case UpdateState::kIdle:
      return "IDLE";
    case UpdateState::kChecking:
      return "CHECKING";
    case UpdateState::kUpdateAvailable:
      return "UPDATE_AVAILABLE";
    case UpdateState::kDownloading:
      return "DOWNLOADING";
    case UpdateState::kDone:
      return "DONE";
    case UpdateState::kFailed:
      return "FAILED";
    case UpdateState::kInstalling:
      return "INSTALLING";
  }
  return "IDLE";
}

bool update_state_from_name(std::string_view name, UpdateState& out) {
  if (name == "IDLE") out = UpdateState::kIdle;
  else if (name == "CHECKING") out = UpdateState::kChecking;
  else if (name == "UPDATE_AVAILABLE") out = UpdateState::kUpdateAvailable;
  else if (name == "DOWNLOADING") out = UpdateState::kDownloading;
  else if (name == "DONE") out = UpdateState::kDone;
  else if (name == "FAILED") out = UpdateState::kFailed;
  else if (name == "INSTALLING") out = UpdateState::kInstalling;
  else return false;
  return true;
}

UpdateEngine::UpdateEngine(Deps deps, EngineConfig cfg)
    : deps_(deps), cfg_(std::move(cfg)), st_(std::make_shared<Persisted>()) {
  st_->install_id = cfg_.install_id;
  st_->channel = cfg_.channel;
}

UpdateEngine::~UpdateEngine() = default;

void UpdateEngine::configure(const std::string& manifest_url, const std::string& channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!manifest_url.empty()) cfg_.manifest_url = manifest_url;
  if (!channel.empty()) {
    cfg_.channel = channel;
    st_->channel = channel;
  }
}

void UpdateEngine::set_options(bool enabled, bool auto_check, bool prefetch) {
  std::lock_guard<std::mutex> lock(mutex_);
  cfg_.enabled = enabled;
  cfg_.auto_check = auto_check;
  cfg_.prefetch = prefetch;
  persist_locked();
}

std::int64_t UpdateEngine::now() const {
  return deps_.clock ? deps_.clock->now_epoch() : 0;
}

std::string UpdateEngine::cur_version() const {
  return cfg_.current_version.empty() ? std::string(current_product_version())
                                      : cfg_.current_version;
}

std::int64_t UpdateEngine::cur_build() const {
  return cfg_.current_build > 0 ? cfg_.current_build : current_build_number();
}

UpdateQuery UpdateEngine::build_query_locked() const {
  UpdateQuery q;
  q.version = cur_version();
  q.build = cur_build();
  q.channel = st_->channel;
  q.platform = cfg_.platform;
  q.schema_version = cfg_.schema_version;
  q.api_version = cfg_.api_version;
  q.os_version = cfg_.os_version;
  q.ignore_min_os = cfg_.ignore_min_os;
  q.install_id = st_->install_id;
  q.skipped_version = st_->skipped_version;
  q.skip_until_epoch = st_->skip_until_epoch;
  return q;
}

std::string UpdateEngine::build_status_json_locked() const {
  const std::int64_t t = now();
  nlohmann::json j;
  j["state"] = update_state_name(st_->state);
  j["version"] = st_->manifest_version;
  j["build"] = st_->manifest_build;
  // 兼容旧 updater_win.json 的键名：落盘用 manifest_*，前端契约用 version/build 别名。
  j["manifest_version"] = st_->manifest_version;
  j["manifest_build"] = st_->manifest_build;
  // 旧 win 壳前端契约（normalizeWinUpdateStatus）用 manifest_size 做总进度与待装大小。
  j["manifest_size"] = st_->manifest_size;
  j["bytes_total"] = st_->bytes_total;
  j["bytes_done"] = st_->bytes_done;
  j["pending_path"] = st_->pending_path;
  j["pending_size"] = st_->pending_size;
  // retry_after：仅失败退避（手动冷却已移除），前端据此提示稍后重试。
  std::int64_t retry = 0;
  if (st_->consecutive_failures > 0) {
    retry = st_->next_check_epoch - t;
  }
  j["retry_after"] = retry > 0 ? retry : 0;
  j["last_error"] = st_->last_error;
  j["last_result"] = st_->last_result;
  j["consecutive_failures"] = st_->consecutive_failures;
  j["channel"] = st_->channel;
  j["install_id"] = st_->install_id;
  j["mandatory"] = st_->mandatory;
  j["mandatory_reason"] = st_->mandatory_reason;
  j["mandatory_deadline"] = st_->mandatory_deadline;
  j["notes_url"] = st_->notes_url;
  j["last_check_epoch"] = st_->last_check_epoch;
  j["next_check_epoch"] = st_->next_check_epoch;
  j["skipped_version"] = st_->skipped_version;
  j["skip_until_epoch"] = st_->skip_until_epoch;
  j["manifest_sha256"] = st_->manifest_sha256;
  j["manifest_setup_url"] = st_->manifest_setup_url;
  j["enabled"] = cfg_.enabled;
  j["auto_check"] = cfg_.auto_check;
  j["prefetch"] = cfg_.prefetch;
  return j.dump(2);
}

void UpdateEngine::persist_locked() {
  if (!deps_.store) return;
  std::string err;
  deps_.store->save(build_status_json_locked(), err);  // 落盘失败不影响内存状态
}

void UpdateEngine::clear_pending_locked() {
  st_->pending_path.clear();
  st_->pending_size = 0;
  st_->bytes_done = 0;
}

void UpdateEngine::clear_manifest_locked() {
  st_->manifest_version.clear();
  st_->manifest_build = 0;
  st_->manifest_sha256.clear();
  st_->manifest_size = 0;
  st_->manifest_setup_url.clear();
  st_->notes_url.clear();
  st_->mandatory = false;
  st_->mandatory_reason.clear();
  st_->mandatory_deadline.clear();
  clear_pending_locked();
  st_->bytes_total = 0;
}

bool UpdateEngine::verify_pending(const std::filesystem::path& path, std::int64_t expect_size,
                                  const std::string& expect_sha, std::string& reason) const {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    reason = "PENDING_MISSING";
    return false;
  }
  const auto sz = std::filesystem::file_size(path, ec);
  if (ec) {
    reason = "FILE_SIZE_FAILED";
    remove_quiet(path);
    return false;
  }
  // 规则 2 的大小校验：清单声明 size 才比对，0 表示未知。
  if (expect_size > 0 && static_cast<std::int64_t>(sz) != expect_size) {
    reason = "SIZE_MISMATCH";
    remove_quiet(path);
    return false;
  }
  if (!deps_.hasher) {
    reason = "NO_HASHER";
    return false;
  }
  std::string hex;
  std::string herr;
  if (!deps_.hasher->sha256_file(path, hex, herr)) {
    reason = herr.empty() ? "SHA256_COMPUTE_FAILED" : herr;
    remove_quiet(path);
    return false;
  }
  if (!expect_sha.empty() && !hex_iequal(hex, expect_sha)) {
    reason = "SHA256_MISMATCH";
    remove_quiet(path);
    return false;
  }
  return true;
}

void UpdateEngine::fail_check_locked(std::int64_t now_epoch, const std::string& error) {
  st_->consecutive_failures++;
  st_->next_check_epoch = now_epoch + check_retry_delay(st_->consecutive_failures);
  st_->last_result = "CHECK_FAILED";
  st_->last_error = error;
  // 检查失败不能弄丢已下载好的待安装包：包还在就保持 DONE，否则记 FAILED。
  st_->state = pending_on_disk_locked() ? UpdateState::kDone : UpdateState::kFailed;
  persist_locked();
}

// pending_path 存的是 UTF-8 窄串；文件操作前统一经 path_from_utf8 转回。
bool UpdateEngine::pending_on_disk_locked() const {
  if (st_->pending_path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(path_from_utf8(st_->pending_path), ec);
}

void UpdateEngine::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string text;
  std::string err;
  if (deps_.store && deps_.store->load(text, err) && !text.empty()) {
    try {
      const auto j = nlohmann::json::parse(text);
      std::string state_name;
      get_str(j, "state", state_name);
      if (!state_name.empty()) update_state_from_name(state_name, st_->state);
      get_str(j, "last_result", st_->last_result);
      get_str(j, "last_error", st_->last_error);
      get_i64(j, "last_check_epoch", st_->last_check_epoch);
      get_i64(j, "next_check_epoch", st_->next_check_epoch);
      get_int(j, "consecutive_failures", st_->consecutive_failures);
      // 落盘的 install_id 优先（灰度分桶必须跨重启稳定）。
      std::string persisted_id;
      get_str(j, "install_id", persisted_id);
      if (!persisted_id.empty()) st_->install_id = persisted_id;
      get_str(j, "channel", st_->channel);
      get_str(j, "manifest_version", st_->manifest_version);
      get_i64(j, "manifest_build", st_->manifest_build);
      get_str(j, "manifest_sha256", st_->manifest_sha256);
      get_i64(j, "manifest_size", st_->manifest_size);
      get_str(j, "manifest_setup_url", st_->manifest_setup_url);
      get_str(j, "notes_url", st_->notes_url);
      get_bool(j, "mandatory", st_->mandatory);
      get_str(j, "mandatory_reason", st_->mandatory_reason);
      get_str(j, "mandatory_deadline", st_->mandatory_deadline);
      get_i64(j, "bytes_done", st_->bytes_done);
      get_i64(j, "bytes_total", st_->bytes_total);
      get_str(j, "pending_path", st_->pending_path);
      get_i64(j, "pending_size", st_->pending_size);
      get_str(j, "skipped_version", st_->skipped_version);
      get_i64(j, "skip_until_epoch", st_->skip_until_epoch);
      // 开关位随状态落盘，重启后必须恢复（旧壳 auto_check/prefetch 跨重启保持）。
      get_bool(j, "enabled", cfg_.enabled);
      get_bool(j, "auto_check", cfg_.auto_check);
      get_bool(j, "prefetch", cfg_.prefetch);
    } catch (...) {
      // 文件损坏：按空状态起步，下次 persist 会覆盖。
    }
  }
  // 崩溃恢复：下载/安装中途被杀，重启后回到可重试的状态。
  if (st_->state == UpdateState::kDownloading) {
    st_->state = st_->manifest_version.empty() ? UpdateState::kIdle
                                               : UpdateState::kUpdateAvailable;
    st_->bytes_done = 0;
  }
  if (st_->state == UpdateState::kInstalling) {
    st_->state = UpdateState::kDone;
  }
  // 残留清理：清单版本已被本机覆盖（装完没清）→ 清掉，前端不再误报「待安装」。
  if (!st_->manifest_version.empty() &&
      !is_update_newer(st_->manifest_version, st_->manifest_build, cur_version(), cur_build())) {
    clear_manifest_locked();
    st_->state = UpdateState::kIdle;
  }
  // 包文件已丢失 → 回到待下载。
  if (st_->state == UpdateState::kDone && !pending_on_disk_locked()) {
    clear_pending_locked();
    st_->state = st_->manifest_version.empty() ? UpdateState::kIdle
                                               : UpdateState::kUpdateAvailable;
  }
  // 从未检查过（新装/迁移）：first_check_delay 秒后做首轮检查，
  // 避免新装用户等满一个完整周期；<=0 表示立即可查（win 壳口径）。
  if (st_->next_check_epoch <= 0) {
    st_->next_check_epoch = now() + cfg_.first_check_delay_seconds;
  }
  persist_locked();
}

bool UpdateEngine::check(bool manual) {
  std::int64_t t = now();
  std::string url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (st_->state == UpdateState::kDownloading || st_->state == UpdateState::kInstalling) {
      return false;  // 忙：下载/安装期间不重入
    }
    if (!manual && t < st_->next_check_epoch) {
      // 自动检查：间隔与失败退避都收敛到 next_check_epoch 一个门槛。
      return false;
    }
    if (!deps_.transport || cfg_.manifest_url.empty()) {
      fail_check_locked(t, deps_.transport ? "MANIFEST_URL_EMPTY" : "NO_TRANSPORT");
      return false;
    }
    url = cfg_.manifest_url;
    st_->state = UpdateState::kChecking;
    st_->last_check_epoch = t;
    st_->last_error.clear();
    persist_locked();
  }

  // 慢 I/O 与纯判定都在锁外/锁内分两段：fetch 不持锁，判定持锁。
  std::string body;
  std::string error;
  bool ok = deps_.transport->fetch(url, body, error);
  nlohmann::json body_json;
  if (ok) {
    try {
      body_json = nlohmann::json::parse(body);
    } catch (...) {
      ok = false;
      error = "MANIFEST_JSON_INVALID";
    }
  }
  if (ok && !cfg_.public_key_base64.empty() &&
      !verify_manifest_signature(body_json, cfg_.public_key_base64)) {
    ok = false;
    error = "MANIFEST_SIGNATURE_INVALID";
  }
  UpdateManifest manifest;
  if (ok) {
    ManifestError merr;
    if (!parse_manifest(body_json, cfg_.platform, manifest, merr,
                        cfg_.prefer_portable_artifact)) {
      ok = false;
      error = merr.code.empty() ? "MANIFEST_INVALID" : merr.code;
    }
  }

  t = now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ok) {
    fail_check_locked(t, error);
    return false;
  }
  st_->consecutive_failures = 0;
  st_->next_check_epoch = t + cfg_.auto_check_interval_seconds;
  if (cfg_.auto_check_jitter_seconds > 0) {
    // 24h±Nh 抖动（mac 壳口径）：避免全网同一时刻打 CDN。
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::int64_t> dist(-cfg_.auto_check_jitter_seconds,
                                                     cfg_.auto_check_jitter_seconds);
    st_->next_check_epoch += dist(rng);
  }

  // 规则 1：清单 sha256 变化 → 作废旧 pending（防误装旧包）。
  const std::string old_sha = st_->manifest_sha256;
  const bool had_pending = !st_->pending_path.empty();
  st_->manifest_version = manifest.version;
  st_->manifest_build = manifest.build;
  st_->manifest_sha256 = manifest.sha256;
  st_->manifest_size = manifest.size;
  st_->manifest_setup_url = manifest.url;
  st_->notes_url = manifest.release_notes_url;
  st_->mandatory = manifest.mandatory;
  st_->mandatory_reason = manifest.mandatory_reason;
  st_->mandatory_deadline = manifest.mandatory_deadline;
  if (had_pending && old_sha != manifest.sha256) {
    remove_quiet(path_from_utf8(st_->pending_path));
    clear_pending_locked();
    st_->state = UpdateState::kIdle;
  }

  const UpdateQuery q = build_query_locked();
  const UpdateDecision dec = evaluate_manifest(manifest, q, t);
  // 两壳旧口径：可用更新一律记 UPDATE_AVAILABLE（前端只认这个码），
  // 强制与否由 mandatory 字段表达；evaluate_manifest 的 UPDATE_REQUIRED
  // 仅服务端自查使用，透传会让壳把强制更新误判成「无更新」。
  st_->last_result = dec.update_available ? "UPDATE_AVAILABLE" : dec.code;
  if (!dec.update_available) {
    // 无可用更新（含已装覆盖/跳过/不在灰度）：清掉待更新信息，保留 skipped_*。
    clear_manifest_locked();
    st_->state = UpdateState::kIdle;
    st_->last_error.clear();
    persist_locked();
    return true;
  }
  if (pending_on_disk_locked()) {
    // 清单未变且包还在：直接恢复「已下载待安装」。
    st_->state = UpdateState::kDone;
    st_->bytes_done = st_->pending_size;
    st_->bytes_total = st_->manifest_size;
  } else {
    clear_pending_locked();
    st_->bytes_total = st_->manifest_size;
    st_->state = UpdateState::kUpdateAvailable;
  }
  st_->last_error.clear();
  persist_locked();
  return true;
}

bool UpdateEngine::download_pending() {
  std::string url;
  std::string sha;
  std::int64_t size = 0;
  std::filesystem::path dest;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (st_->state == UpdateState::kDone) return true;
    if (st_->state != UpdateState::kUpdateAvailable && st_->state != UpdateState::kFailed) {
      return false;
    }
    if (!deps_.transport || st_->manifest_setup_url.empty()) return false;
    url = st_->manifest_setup_url;
    sha = st_->manifest_sha256;
    size = st_->manifest_size;
    if (!cfg_.download_dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(cfg_.download_dir, ec);
      dest = cfg_.download_dir / basename_from_url(url);
    } else {
      dest = std::filesystem::path(basename_from_url(url));
    }
    st_->state = UpdateState::kDownloading;
    st_->bytes_done = 0;
    st_->bytes_total = size;
    st_->pending_path.clear();
    st_->pending_size = 0;
    st_->last_result = "DOWNLOAD_STARTED";
    st_->last_error.clear();
    persist_locked();  // 规则 4：进入下载即落盘一次
  }
  cancel_flag_ = false;
  // 规则 3：先清掉上次崩溃留下的 dest / dest.part。
  remove_quiet(dest);
  remove_quiet(part_path_of(dest));

  std::string terr;
  const ProgressFn on_progress = [this](std::int64_t done, std::int64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    st_->bytes_done = done;
    if (total > 0) st_->bytes_total = total;
    persist_locked();  // 规则 4：进度与 pending_size 同一次落盘
  };
  const CancelFn on_cancel = [this]() { return cancel_flag_.load(); };
  const bool ok = deps_.transport->download(url, dest, on_progress, on_cancel, terr);

  if (!ok) {
    // 规则 3：失败/取消必须删掉半截文件。
    remove_quiet(dest);
    remove_quiet(part_path_of(dest));
    std::lock_guard<std::mutex> lock(mutex_);
    st_->bytes_done = 0;
    // 与两壳旧行为一致：取消也记 FAILED 态（last_result=CANCELED），可立即重下。
    st_->state = UpdateState::kFailed;
    st_->last_result = cancel_flag_.load() ? "CANCELED" : "DOWNLOAD_FAILED";
    if (!cancel_flag_.load()) st_->last_error = terr;
    persist_locked();
    return false;
  }

  // 下载完成后先校验（锁外做哈希），再落状态。
  std::string reason;
  const bool verified = verify_pending(dest, size, sha, reason);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!verified) {
    st_->state = UpdateState::kFailed;
    st_->last_result = "VERIFY_FAILED";
    st_->last_error = reason;
    clear_pending_locked();
    persist_locked();
    return false;
  }
  st_->pending_path = path_to_utf8(dest);
  st_->pending_size = size > 0 ? size : st_->bytes_total;
  st_->bytes_done = st_->pending_size;
  st_->bytes_total = st_->pending_size;
  st_->state = UpdateState::kDone;
  st_->last_result = "DOWNLOADED";
  st_->last_error.clear();
  persist_locked();
  return true;
}

InstallResult UpdateEngine::try_install() {
  std::filesystem::path path;
  std::string sha;
  std::string version;
  std::int64_t size = 0;
  std::int64_t build = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (st_->state == UpdateState::kInstalling) {
      return {false, "FAILED", "install already in progress"};
    }
    const std::int64_t pending_build = st_->pending_path.empty() ? 0 : st_->manifest_build;
    const InstallDecision d =
        decide_install(deps_.installer != nullptr, pending_build, cur_build());
    if (d == InstallDecision::kUnavailable) {
      return {false, "UNAVAILABLE", "installer not available"};
    }
    if (d == InstallDecision::kNoPendingUpdate) {
      return {false, "NO_PENDING_UPDATE", "no pending update"};
    }
    path = path_from_utf8(st_->pending_path);
    sha = st_->manifest_sha256;
    version = st_->manifest_version;
    size = st_->manifest_size;
    build = st_->manifest_build;
  }

  // 规则 2：进入 INSTALLING 前重校验大小 + SHA256（锁外哈希）。
  std::string reason;
  if (!verify_pending(path, size, sha, reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_pending_locked();
    // 坏包记 FAILED（与旧壳一致）；download_pending 接受 FAILED，可立即重下。
    st_->state = UpdateState::kFailed;
    st_->last_result = "VERIFY_FAILED";
    st_->last_error = reason;
    persist_locked();
    return {false, "VERIFY_FAILED", reason};
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    st_->state = UpdateState::kInstalling;
    st_->last_error.clear();
    persist_locked();
  }

  PendingPackage pkg;
  pkg.path = path;
  pkg.size = size;
  pkg.sha256 = sha;
  pkg.version = version;
  pkg.build = build;
  InstallResult res = deps_.installer->install(pkg);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!res.started) {
    // 没启动起来：回到 DONE 允许重试，包还在。
    st_->state = UpdateState::kDone;
    st_->last_result = "INSTALL_FAILED";
    st_->last_error = res.error;
    persist_locked();
  }
  // started：保持 INSTALLING，进程即将由安装器接管。
  return res;
}

void UpdateEngine::cancel_download() { cancel_flag_ = true; }

bool UpdateEngine::skip_version(const std::string& version, int days) {
  if (!valid_skip_days(days) || version.empty()) return false;
  std::string pending_file;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    st_->skipped_version = version;
    st_->skip_until_epoch = now() + static_cast<std::int64_t>(days) * 86400;
    pending_file = st_->pending_path;
    clear_manifest_locked();
    st_->state = UpdateState::kIdle;
    st_->last_result = "SKIPPED";
    st_->last_error.clear();
    persist_locked();
  }
  if (!pending_file.empty()) remove_quiet(path_from_utf8(pending_file));
  return true;
}

bool UpdateEngine::skip_current(int days) {
  std::string version;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (st_->manifest_version.empty()) return false;
    version = st_->manifest_version;
  }
  return skip_version(version, days);
}

void UpdateEngine::invalidate_schedule() {
  std::lock_guard<std::mutex> lock(mutex_);
  st_->next_check_epoch = 0;
  persist_locked();
}

std::string UpdateEngine::status_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return build_status_json_locked();
}

nlohmann::json UpdateEngine::status() const {
  return nlohmann::json::parse(status_json(), nullptr, false);
}

UpdateState UpdateEngine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return st_->state;
}

}  // namespace xiaotu
