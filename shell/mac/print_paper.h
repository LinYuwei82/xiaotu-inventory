#pragma once

#include <algorithm>
#include <cmath>
#include <string>

// 打印纸型的纯计算部分：与 Cocoa 解耦，便于在 C++ 测试里直接断言 mm→pt、
// 方向与缩放规则；shell/mac/webview.mm 只负责把 NSDictionary 转成入参、
// 把结果写回 NSPrintInfo。
namespace xiaotu::mac_print {

constexpr double kMmToPoint = 72.0 / 25.4;
constexpr double kMinPaperMm = 10.0;    // 小于 10 mm 基本不可能是有效纸张
constexpr double kMaxPaperMm = 2000.0;  // 超过 2 m 视为参数错误

struct PaperSpec {
  double width_pt = 0;
  double height_pt = 0;
  double margin_pt = 0;
  bool landscape = false;   // 宽 > 高：显式告知驱动，避免二次旋转
  bool fixed_scale = false; // 连续纸或原始尺寸：固定 100%，不跟随驱动缩放
};

struct PaperFailure {
  std::string code;
  std::string message;
};

inline bool finite_in_range(double mm) {
  return std::isfinite(mm) && mm >= kMinPaperMm && mm <= kMaxPaperMm;
}

// 校验并换算纸型。连续纸（CONTINUOUS）或原始尺寸（ORIGINAL）固定缩放 100%。
inline bool make_paper_spec(double width_mm, double height_mm, double margin_mm,
                            bool continuous, bool original, PaperSpec& spec,
                            PaperFailure& failure) {
  if (!finite_in_range(width_mm) || !finite_in_range(height_mm)) {
    failure.code = "PAPER_SIZE_INVALID";
    failure.message = "纸张宽高必须在 10–2000 mm 之间。";
    return false;
  }
  if (!std::isfinite(margin_mm) || margin_mm < 0 || margin_mm * 2 >= std::min(width_mm, height_mm)) {
    failure.code = "PAPER_MARGIN_INVALID";
    failure.message = "页边距必须为非负数，且小于纸张短边的一半。";
    return false;
  }
  spec.width_pt = width_mm * kMmToPoint;
  spec.height_pt = height_mm * kMmToPoint;
  spec.margin_pt = margin_mm * kMmToPoint;
  spec.landscape = width_mm > height_mm;
  spec.fixed_scale = continuous || original;
  return true;
}

}  // namespace xiaotu::mac_print
