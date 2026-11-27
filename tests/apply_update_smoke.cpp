// Windows 更新协助器的跨平台可测部分：
//   - apply-state 四态读写（原子写、覆盖语义、损坏文件容错）
//   - interrupted 推导（installing 超阈值）
//   - 清单包偏好选择（安装版优先 setup / 绿色版优先 portable）
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "xiaotu/apply_update.h"
#include "xiaotu/update.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

namespace fs = std::filesystem;

fs::path temp_root() {
  static const fs::path root =
      fs::temp_directory_path() /
      ("xiaotu-apply-update-smoke-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  return root;
}

// 把 apply-state.json 的 mtime 拨回 seconds 秒前，模拟协助器失联。
void age_apply_state(const fs::path& data_dir, int seconds) {
  std::error_code ec;
  const fs::path file = data_dir / "updates" / "apply-state.json";
  fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(seconds), ec);
  check(!ec, "拨回 apply-state mtime 成功");
}

void test_state_names() {
  check(std::string(xiaotu::apply_state_name(xiaotu::ApplyState::Installing)) == "installing",
        "installing 态名");
  check(std::string(xiaotu::apply_state_name(xiaotu::ApplyState::Verifying)) == "verifying",
        "verifying 态名");
  check(std::string(xiaotu::apply_state_name(xiaotu::ApplyState::Done)) == "done", "done 态名");
  check(std::string(xiaotu::apply_state_name(xiaotu::ApplyState::Failed)) == "failed",
        "failed 态名");
}

void test_state_roundtrip() {
  const fs::path data = temp_root() / "roundtrip";
  std::string error;

  // 文件不存在：present=false。
  auto missing = xiaotu::read_apply_state(data);
  check(!missing.present, "未写入时 present=false");
  check(missing.state.empty(), "未写入时 state 为空");

  check(xiaotu::write_apply_state(data, xiaotu::ApplyState::Done, "安装器退出码 0", &error),
        "写入 done 态");
  check(error.empty(), "写入成功时 error 为空");
  auto snap = xiaotu::read_apply_state(data);
  check(snap.present && snap.state == "done", "读回 done 态");
  check(snap.detail == "安装器退出码 0", "detail 往返一致");
  check(!snap.updated_at.empty(), "updated_at 已填写");
  check(snap.age_seconds >= 0 && snap.age_seconds < 60, "刚写入的 age_seconds 接近 0");
  check(!fs::exists(data / "updates" / "apply-state.json.tmp"), "原子写不留 .tmp");

  // 覆盖语义：后写覆盖先写。
  check(xiaotu::write_apply_state(data, xiaotu::ApplyState::Failed, "退出码 1", &error),
        "覆盖写 failed 态");
  snap = xiaotu::read_apply_state(data);
  check(snap.present && snap.state == "failed" && snap.detail == "退出码 1", "覆盖后读回 failed");

  // 损坏 JSON：present=true 但 state=unknown，不崩。
  const fs::path file = data / "updates" / "apply-state.json";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "{not json";
  }
  snap = xiaotu::read_apply_state(data);
  check(snap.present && snap.state == "unknown", "损坏 JSON 归为 unknown");
}

void test_interrupted() {
  const fs::path data = temp_root() / "interrupted";
  std::string error;

  // installing 且未超阈值：不算中断。
  xiaotu::write_apply_state(data, xiaotu::ApplyState::Installing, "协助器即将启动", &error);
  check(!xiaotu::apply_state_interrupted(xiaotu::read_apply_state(data)), "新鲜 installing 不中断");

  // installing 超阈值：算中断。
  age_apply_state(data, xiaotu::kApplyInterruptSeconds + 100);
  check(xiaotu::apply_state_interrupted(xiaotu::read_apply_state(data)),
        "超阈值 installing 判为中断");

  // done/failed 无论多久都不算中断。
  xiaotu::write_apply_state(data, xiaotu::ApplyState::Done, "ok", &error);
  age_apply_state(data, xiaotu::kApplyInterruptSeconds + 100);
  check(!xiaotu::apply_state_interrupted(xiaotu::read_apply_state(data)), "陈旧 done 不中断");

  // 自定义阈值参与推导。
  xiaotu::write_apply_state(data, xiaotu::ApplyState::Installing, "x", &error);
  auto snap = xiaotu::read_apply_state(data);
  check(!xiaotu::apply_state_interrupted(snap, 3600), "阈值内不中断");
  check(xiaotu::apply_state_interrupted(snap, -1), "超阈值中断");
}

// 包偏好：v2 清单 windows 段同时带 setup 与 portable 时的角色选择。
void test_artifact_preference() {
  nlohmann::json body{
      {"manifest_version", 2},
      {"product", "xiaotu"},
      {"channel", "stable"},
      {"key_id", "2026-11"},
      {"updated_at", "2026-11-18T04:48:54Z"},
      {"signature", "c2lnbmF0dXJl"}};
  body["platforms"]["windows"] = {
      {"version", "1.0.19"},
      {"build", 15},
      {"min_os", "10.0"},
      {"rollout_percent", 100},
      {"artifacts",
       {{"setup",
         {{"url", "https://dl/xiaotu-1.0.19-win64-setup.exe"},
          {"size", 100},
          {"sha256", std::string(64, '8')}}},
        {"portable",
         {{"url", "https://dl/xiaotu-1.0.19-win64-portable.zip"},
          {"size", 200},
          {"sha256", std::string(64, '7')}}}}}};

  xiaotu::UpdateManifest manifest;
  xiaotu::ManifestError error;

  check(xiaotu::parse_manifest(body, "windows", manifest, error, false) &&
            manifest.url.find("setup.exe") != std::string::npos,
        "默认（安装版）优先 setup");
  check(xiaotu::parse_manifest(body, "windows", manifest, error, true) &&
            manifest.url.find("portable.zip") != std::string::npos,
        "prefer_portable（绿色版）优先 portable");

  // 缺 portable：偏好 portable 也回落 setup。
  body["platforms"]["windows"]["artifacts"].erase("portable");
  check(xiaotu::parse_manifest(body, "windows", manifest, error, true) &&
            manifest.url.find("setup.exe") != std::string::npos,
        "缺 portable 回落 setup");

  // 缺 setup：默认也选 portable。
  body["platforms"]["windows"]["artifacts"] = {
      {"portable",
       {{"url", "https://dl/xiaotu-1.0.19-win64-portable.zip"},
        {"size", 200},
        {"sha256", std::string(64, '7')}}}};
  check(xiaotu::parse_manifest(body, "windows", manifest, error, false) &&
            manifest.url.find("portable.zip") != std::string::npos,
        "缺 setup 选 portable");

  // 两者都缺：报 ARTIFACT_MISSING。
  body["platforms"]["windows"]["artifacts"] = nlohmann::json::object();
  check(!xiaotu::parse_manifest(body, "windows", manifest, error), "两者都缺应拒绝");
}

}  // namespace

int main() {
  test_state_names();
  test_state_roundtrip();
  test_interrupted();
  test_artifact_preference();

  std::error_code ec;
  fs::remove_all(temp_root(), ec);

  if (failures != 0) {
    std::cerr << failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "apply_update_smoke 全部通过" << std::endl;
  return 0;
}
