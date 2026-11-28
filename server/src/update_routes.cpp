#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/log.h"
#include "xiaotu/services.h"
#include "xiaotu/update.h"
#include "xiaotu/update_install.h"
#include "xiaotu/update_service.h"

namespace xiaotu {

void HttpServer::register_update_routes() {
  // 更新检查：免登录，只读；不采集匿名统计。清单由部署侧同步到本地文件，
  // 服务端不在此处发起 HTTPS 请求（见 docs/更新模块技术方案.md 第 5 节）。
  server_->Get("/api/update/check", [this](const httplib::Request& req, httplib::Response& res) {
    auto int_param = [&req](const char* name, std::int64_t fallback) -> std::int64_t {
      const std::string raw = req.get_param_value(name);
      if (raw.empty()) {
        return fallback;
      }
      try {
        return std::stoll(raw);
      } catch (const std::exception&) {
        return -1;
      }
    };
    UpdateQuery query;
    query.version = req.get_param_value("current_version");
    query.build = int_param("build", -1);
    query.channel = req.get_param_value("channel");
    query.platform = req.get_param_value("platform");
    query.schema_version = static_cast<int>(int_param("schema_version", -1));
    query.api_version = static_cast<int>(int_param("api_version", 0));
    query.os_version = req.get_param_value("os_version");
    query.install_id = req.get_param_value("install_id");
    if (query.version.empty() || query.build <= 0 || query.schema_version <= 0 ||
        query.channel.empty() || query.platform.empty()) {
      json_error(res, 400, "UPDATE_QUERY_INVALID",
                 "需要 current_version、build、channel、platform、schema_version");
      return;
    }

    const UpdateSettings settings = update_settings();
    const UpdateCheckResult result =
        check_for_update(settings, query, static_cast<std::int64_t>(std::time(nullptr)));
    if (!result.manifest_ok) {
      json_error(res, result.configured ? 503 : 200, result.error_code, result.error_message);
      if (!result.configured) {
        // 未配置更新源不是错误：返回“无更新”，客户端按正常节奏重试。
        res.body = nlohmann::json{{"ok", true},
                                  {"update_available", false},
                                  {"code", "UPDATE_NOT_CONFIGURED"},
                                  {"configured", false},
                                  {"message", result.error_message},
                                  {"latest", nullptr}}
                       .dump();
        res.status = 200;
      }
      return;
    }

    nlohmann::json body;
    body["update_available"] = result.decision.update_available;
    body["mandatory"] = result.decision.mandatory;
    body["code"] = result.decision.code;
    body["mandatory_reason"] =
        result.decision.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.decision.reason);
    body["min_supported_version"] = info_.update_min_supported_version.empty()
                                        ? nlohmann::json(nullptr)
                                        : nlohmann::json(info_.update_min_supported_version);
    body["check_after_seconds"] = 86400;
    body["server_version"] = info_.version;
    body["api_version"] = current_api_version();
    if (result.decision.intermediate_required) {
      body["intermediate"] = {{"version", result.decision.intermediate.version},
                              {"build", result.decision.intermediate.build},
                              {"schema", result.decision.intermediate.schema}};
    } else {
      body["intermediate"] = nullptr;
    }
    if (result.decision.update_available) {
      body["latest"] = {{"version", result.manifest.version},
                        {"build", result.manifest.build},
                        {"url", result.manifest.url},
                        {"sha256", result.manifest.sha256},
                        {"size", result.manifest.size},
                        {"published_at", result.manifest.published_at},
                        {"release_notes_url", result.manifest.release_notes_url},
                        {"mandatory", result.manifest.mandatory},
                        {"mandatory_deadline", result.manifest.mandatory_deadline.empty()
                                                   ? nlohmann::json(nullptr)
                                                   : nlohmann::json(result.manifest.mandatory_deadline)}};
    } else {
      body["latest"] = nullptr;
    }
    json_ok(res, body);
  });

  // 更新状态：设置页用。版本、schema 与平台全部由服务端自身给出，
  // 前端只展示「当前版本 / 最新版本 / 发布日期 / 是否已是最新 / 失败原因」。
  server_->Get("/api/update/status",
               guard("setting.read", [this](const httplib::Request&, httplib::Response& res) {
                 const UpdateQuery query = self_update_query();
                 const UpdateCheckResult result = check_for_update(
                     update_settings(), query, static_cast<std::int64_t>(std::time(nullptr)));
                 nlohmann::json body;
                 body["ok"] = true;
                 body["configured"] = result.configured;
                 // 清单来源与回退提示：network=本次刚拉取，cache=本地缓存（可能是网络失败后的回退），
                 // file=部署侧同步的本地清单。
                 body["manifest_source"] = result.manifest_source.empty()
                                               ? nlohmann::json(nullptr)
                                               : nlohmann::json(result.manifest_source);
                 body["manifest_stale"] = result.manifest_stale;
                 body["manifest_warning"] = result.manifest_warning.empty()
                                                ? nlohmann::json(nullptr)
                                                : nlohmann::json(result.manifest_warning);
                 // 后台策略只读展示：由 config.toml 的 [update] 段决定，运行中不提供切换。
                 body["auto_check"] = info_.update_auto_check;
                 body["prefetch"] = info_.update_prefetch;
                 body["current"] = {{"version", info_.version},
                                    {"build", current_build_number()},
                                    {"schema_version", services_.db.schema_version()},
                                    {"platform", query.platform}};
                 body["update_available"] = result.manifest_ok && result.decision.update_available;
                 // 强制标记可能来自清单 mandatory，也可能来自服务端 min_supported_version；
                 // 与 latest.mandatory（仅清单字段）分开，界面以这个为准。
                 body["mandatory"] = result.manifest_ok && result.decision.mandatory;
                 body["mandatory_reason"] = result.manifest_ok && !result.decision.reason.empty()
                                                ? nlohmann::json(result.decision.reason)
                                                : nlohmann::json(nullptr);
                 body["min_supported_version"] = info_.update_min_supported_version.empty()
                                                     ? nlohmann::json(nullptr)
                                                     : nlohmann::json(info_.update_min_supported_version);
                 if (result.manifest_ok) {
                   body["up_to_date"] = result.decision.code == "NO_UPDATE";
                   body["code"] = result.decision.code;
                   body["error"] = nullptr;
                 } else {
                   body["up_to_date"] = nullptr;
                   body["code"] = result.error_code;
                   body["error"] = {{"code", result.error_code},
                                     {"message", result.error_message}};
                 }
                 if (result.manifest_ok && result.decision.update_available) {
                   body["latest"] = {{"version", result.manifest.version},
                                     {"build", result.manifest.build},
                                     {"published_at", result.manifest.published_at},
                                     {"release_notes_url", result.manifest.release_notes_url},
                                     {"size", result.manifest.size},
                                     {"sha256", result.manifest.sha256},
                                     {"mandatory", result.manifest.mandatory}};
                 } else {
                   body["latest"] = nullptr;
                 }
                 {
                   std::lock_guard<std::mutex> lock(update_job_mutex_);
                   body["download"] = {
                       {"state", update_job_state_},
                       {"file", update_job_file_.empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(update_job_file_)},
                       {"bytes", update_job_bytes_},
                       {"code", update_job_code_.empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(update_job_code_)},
                       {"message", update_job_message_.empty()
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(update_job_message_)},
                       {"at", update_job_at_.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(update_job_at_)}};
                 }
                 json_ok(res, body);
               }));

  // 下载安装包：后台执行（大包会超过 15 秒写超时），下载完成后校验 sha256 与签名。
  // 校验失败不保留文件，原因写入日志与任务状态，前端轮询 /api/update/status 展示。
  server_->Post("/api/update/download",
                guard("setting.write", [this](const httplib::Request&, httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可下载更新");
                    return;
                  }
                  const UpdateSettings settings = update_settings();
                  const UpdateCheckResult result = check_for_update(
                      settings, self_update_query(), static_cast<std::int64_t>(std::time(nullptr)));
                  if (!result.configured) {
                    json_error(res, 400, "UPDATE_NOT_CONFIGURED", result.error_message);
                    return;
                  }
                  if (!result.manifest_ok) {
                    json_error(res, 503, result.error_code, result.error_message);
                    return;
                  }
                  if (!result.decision.update_available) {
                    json_error(res, 409, "NO_UPDATE_AVAILABLE", "当前已是最新版本，无需下载");
                    return;
                  }
                  const std::string manifest_file = result.manifest_path;
                  const UpdateManifest manifest = result.manifest;
                  if (!start_update_download(settings, manifest_file, manifest, false)) {
                    json_error(res, 409, "UPDATE_BUSY", "已有更新包正在下载");
                    return;
                  }
                  json_ok(res, nlohmann::json{{"ok", true},
                                              {"state", "running"},
                                              {"version", manifest.version},
                                              {"build", manifest.build}});
                }));

  // 取消后台下载：置位取消标志并唤醒退避等待；正在跑的 curl 会被终止，
  // 任务状态随后由下载线程改为 canceled，前端轮询 /api/update/status 获取结果。
  server_->Post("/api/update/download/cancel",
                guard("setting.write", [this](const httplib::Request&, httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可取消下载更新");
                    return;
                  }
                  bool running = false;
                  {
                    std::lock_guard<std::mutex> lock(update_job_mutex_);
                    running = update_job_state_ == "running";
                    if (running) {
                      update_job_cancel_ = true;
                    }
                  }
                  if (!running) {
                    json_error(res, 409, "UPDATE_NOT_DOWNLOADING", "当前没有正在进行的下载");
                    return;
                  }
                  update_job_cv_.notify_all();
                  json_ok(res, nlohmann::json{{"ok", true}, {"state", "canceling"}});
                }));

  // 安装更新：必须显式 confirm。服务端复验安装包哈希与签名后，准备替换脚本、
  // 返回响应，然后优雅退出；脚本等进程退出后备份旧程序、覆盖新程序并按原参数重启。
  // 失败时脚本回滚旧程序，不留下无法启动的状态。
  server_->Post("/api/update/install",
                guard("setting.write", [this](const httplib::Request& req,
                                              httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可安装更新");
                    return;
                  }
                  const nlohmann::json body = json_body(req);
                  if (!body.value("confirm", false)) {
                    json_error(res, 400, "UPDATE_CONFIRM_REQUIRED", "安装更新需要二次确认");
                    return;
                  }
                  const UpdateSettings settings = update_settings();
                  const UpdateCheckResult result = check_for_update(
                      settings, self_update_query(), static_cast<std::int64_t>(std::time(nullptr)));
                  if (!result.manifest_ok) {
                    json_error(res, result.configured ? 503 : 400, result.error_code,
                               result.error_message);
                    return;
                  }
                  if (!result.decision.update_available) {
                    json_error(res, 409, "NO_UPDATE_AVAILABLE", "当前已是最新版本");
                    return;
                  }
                  const std::filesystem::path package =
                      update_package_path(update_download_dir(), result.manifest);
                  // 解包/复验后准备替换：真正的替换由脚本在本进程退出后完成，
                  // 避免覆盖正在运行的二进制；失败时不改动当前程序。
                  const std::filesystem::path staging =
                      update_download_dir() /
                      ("staging-" + result.manifest.version + "-" +
                       std::to_string(result.manifest.build));
                  const std::filesystem::path executable = current_executable_path();
                  const UpdatePackageResult staged = stage_update_package(
                      settings, result.manifest_path, package, staging, executable);
                  if (!staged.ok) {
                    int status = 500;
                    if (staged.error_code == "UPDATE_PACKAGE_MISSING" ||
                        staged.error_code == "UPDATE_PACKAGE_PAYLOAD_MISSING" ||
                        staged.error_code == "UPDATE_PACKAGE_UNSUPPORTED" ||
                        staged.error_code == "UPDATE_EXTRACT_FAILED" ||
                        staged.error_code == "UPDATE_SIGNATURE_INVALID") {
                      status = 409;
                    } else if (staged.error_code == "UPDATE_UNVERIFIED" ||
                               staged.error_code == "UPDATE_VERIFY_UNAVAILABLE") {
                      status = 503;
                    }
                    json_error(res, status, staged.error_code, staged.error_message);
                    return;
                  }
                  UpdateApplyOptions options;
                  options.executable = executable;
                  if (info_.launch_args.size() > 1) {
                    options.args.assign(info_.launch_args.begin() + 1, info_.launch_args.end());
                  }
                  std::error_code cwd_error;
                  options.working_dir = std::filesystem::current_path(cwd_error);
                  if (cwd_error) {
                    options.working_dir = executable.parent_path();
                  }
                  options.log_file = staging / "update-apply.log";
                  const UpdatePackageResult scheduled =
                      schedule_update_replace(staged.path, options, staging);
                  if (!scheduled.ok) {
                    json_error(res, 500, scheduled.error_code, scheduled.error_message);
                    return;
                  }
                  try {
                    services_.db.write([&](Conn& conn) {
                      audit_write(conn, "update_install", "system", 0, result.manifest.version,
                                  "自动替换: " + package.filename().string());
                    });
                  } catch (const std::exception& error) {
                    log_warn(std::string("更新安装审计写入失败: ") + error.what());
                  }
                  json_ok(res, nlohmann::json{{"ok", true},
                                              {"mode", scheduled.mode},
                                              {"restarting", true},
                                              {"file", package.filename().string()},
                                              {"message", scheduled.error_message}});
                  // 先让响应写出，再停止监听：主循环退出后替换脚本接管。
                  // 置维护态，避免退出前的几百毫秒里再接受新的写请求。
                  maintenance_ = true;
                  if (!shutdown_started_.exchange(true)) {
                    shutdown_thread_ = std::thread([this] {
                      std::this_thread::sleep_for(std::chrono::milliseconds(800));
                      server_->stop();
                    });
                  }
                }));
}

}  // namespace xiaotu
