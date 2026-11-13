#include <cmath>
#include <iostream>
#include <limits>

#include "print_paper.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

bool near(double a, double b) {
  return std::fabs(a - b) < 0.01;
}

}  // namespace

int main() {
  using xiaotu::mac_print::PaperFailure;
  using xiaotu::mac_print::PaperSpec;

  check(near(xiaotu::mac_print::kMmToPoint * 210.0, 595.2756), "mm→pt 换算常量正确");

  PaperFailure error;
  PaperSpec a4;
  check(xiaotu::mac_print::make_paper_spec(210, 297, 12, false, false, a4, error),
        "A4 纵向参数应通过");
  check(near(a4.width_pt, 595.2756) && near(a4.height_pt, 841.8898), "A4 宽高换算正确");
  check(near(a4.margin_pt, 34.0157), "边距 mm→pt 换算正确");
  check(!a4.landscape, "A4 纵向不应判定为横向");
  check(!a4.fixed_scale, "单页纸 FIT_WIDTH 不固定缩放");

  PaperSpec continuous;
  check(xiaotu::mac_print::make_paper_spec(241, 140, 12, true, false, continuous, error),
        "241×140 连续纸参数应通过");
  check(continuous.landscape, "241×140 应判定为横向");
  check(near(continuous.width_pt, 683.1496) && near(continuous.height_pt, 396.8504),
        "连续纸宽高换算正确");
  check(continuous.fixed_scale, "连续纸应固定 100% 缩放");

  PaperSpec original;
  check(xiaotu::mac_print::make_paper_spec(210, 297, 12, false, true, original, error),
        "原始尺寸参数应通过");
  check(original.fixed_scale, "原始尺寸应固定 100% 缩放");

  PaperSpec zero;
  check(!xiaotu::mac_print::make_paper_spec(0, 297, 12, false, false, zero, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "宽为 0 应拒绝");

  PaperSpec negative;
  check(!xiaotu::mac_print::make_paper_spec(210, -1, 12, false, false, negative, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "高为负数应拒绝");

  PaperSpec too_small;
  check(!xiaotu::mac_print::make_paper_spec(5, 5, 1, false, false, too_small, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "小于 10 mm 应拒绝");

  PaperSpec too_large;
  check(!xiaotu::mac_print::make_paper_spec(210, 2001, 12, false, false, too_large, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "超过 2000 mm 应拒绝");

  PaperSpec nan_size;
  check(!xiaotu::mac_print::make_paper_spec(std::numeric_limits<double>::quiet_NaN(), 297, 12,
                                            false, false, nan_size, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "NaN 尺寸应拒绝");

  PaperSpec infinite_size;
  check(!xiaotu::mac_print::make_paper_spec(210, std::numeric_limits<double>::infinity(), 12,
                                            false, false, infinite_size, error) &&
            error.code == "PAPER_SIZE_INVALID",
        "无穷大尺寸应拒绝");

  PaperSpec negative_margin;
  check(!xiaotu::mac_print::make_paper_spec(210, 297, -1, false, false, negative_margin, error) &&
            error.code == "PAPER_MARGIN_INVALID",
        "负边距应拒绝");

  PaperSpec huge_margin;
  check(!xiaotu::mac_print::make_paper_spec(210, 297, 105, false, false, huge_margin, error) &&
            error.code == "PAPER_MARGIN_INVALID",
        "边距达到短边一半应拒绝");

  PaperSpec zero_margin;
  check(xiaotu::mac_print::make_paper_spec(210, 297, 0, false, false, zero_margin, error),
        "零边距应允许");

  if (failures == 0) {
    std::cout << "print_paper_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
