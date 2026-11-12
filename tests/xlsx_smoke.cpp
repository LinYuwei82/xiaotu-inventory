// xlsx 读取器冒烟：deflate + sharedStrings、store、合并单元格拒绝、表头映射。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "xiaotu/error.h"
#include "xiaotu/xlsx_reader.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool rejected(const std::string& bytes, std::string* message) {
  try {
    xiaotu::read_first_sheet(bytes);
  } catch (const xiaotu::AppError& error) {
    if (message != nullptr) *message = error.what();
    return error.code() == "IMPORT_INVALID";
  }
  return false;
}

void verify_table(const xiaotu::SheetTable& table, const std::string& label) {
  const std::vector<std::string> header = {"商品编码", "商品名称", "规格", "计量单位", "启用"};
  check(table.header == header, label + ": 表头应为 5 列");

  check(table.rows.size() == 4, label + ": 应跳过空行，保留 4 行数据");
  if (table.rows.size() != 4) return;

  const auto& first = table.rows[0];
  check(first.number == 2, label + ": 第一行数据行号应为 2");
  check(first.cells.size() == 5, label + ": 第一行应有 5 列");
  check(first.cells[0] == "SP001", label + ": A2");
  check(first.cells[1] == "矿泉水", label + ": B2");
  check(first.cells[2] == "550ml", label + ": C2");
  check(first.cells[3] == "瓶", label + ": D2");
  check(first.cells[4] == "是", label + ": E2 共享字符串");

  const auto& second = table.rows[1];
  check(second.number == 3, label + ": 第二行行号应为 3");
  check(second.cells[2] == "箱装", label + ": C3 inlineStr");
  check(second.cells[4] == "0", label + ": E3 布尔应读作 0");

  const auto& third = table.rows[2];
  check(third.number == 4, label + ": 第三行行号应为 4");
  check(third.cells.size() == 4, label + ": 第 4 行只到 D 列");
  check(third.cells[0] == "00123", label + ": A4 前导零应保留");
  check(third.cells[1] == "测试品", label + ": B4");
  check(third.cells[2].empty(), label + ": C4 空单元格应补空串");
  check(third.cells[3] == "瓶", label + ": D4 跳列后仍应落在第 4 列");

  const auto& fourth = table.rows[3];
  check(fourth.number == 6, label + ": 第四行行号应为 6（第 5 行是空行）");
  check(fourth.cells[2] == "5 & 6 <小>", label + ": C6 XML 转义应还原");
  check(fourth.cells[4] == "否", label + ": E6");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "用法: xiaotu_xlsx_smoke <data-dir>" << std::endl;
    return 1;
  }
  const std::filesystem::path data = argv[1];

  try {
    const std::string deflate = read_file(data / "sample-deflate.xlsx");
    check(!deflate.empty(), "deflate 样本应能读取");
    verify_table(xiaotu::read_first_sheet(deflate), "deflate");

    const std::string stored = read_file(data / "sample-store.xlsx");
    verify_table(xiaotu::read_first_sheet(stored), "store");

    std::string message;
    check(rejected(read_file(data / "sample-merged.xlsx"), &message),
          "合并单元格应被拒绝");
    check(message.find("合并") != std::string::npos, "合并单元格错误应说明原因");
    check(rejected("not a zip file", &message), "非 ZIP 内容应被拒绝");

    const xiaotu::ColumnMap map = xiaotu::map_columns(
        {"商品编码", "商品名称", "规格"}, {"商品编码", "商品名称", "规格", "计量单位"},
        {"商品编码", "商品名称"});
    check(map.has("商品名称"), "已知列应能映射");
    check(!map.has("计量单位"), "缺省列不应出现在映射里");

    bool unknown_rejected = false;
    try {
      xiaotu::map_columns({"商品编码", "备注"}, {"商品编码"}, {"商品编码"});
    } catch (const xiaotu::AppError& error) {
      unknown_rejected = error.code() == "IMPORT_INVALID" &&
                         std::string(error.what()).find("备注") != std::string::npos;
    }
    check(unknown_rejected, "未知表头应报错并指出列名");

    bool duplicate_rejected = false;
    try {
      xiaotu::map_columns({"商品编码", "商品编码"}, {"商品编码"}, {"商品编码"});
    } catch (const xiaotu::AppError&) {
      duplicate_rejected = true;
    }
    check(duplicate_rejected, "重复表头应报错");

    bool missing_rejected = false;
    try {
      xiaotu::map_columns({"商品编码"}, {"商品编码", "商品名称"}, {"商品编码", "商品名称"});
    } catch (const xiaotu::AppError&) {
      missing_rejected = true;
    }
    check(missing_rejected, "缺少必填列应报错");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: 未预期异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "xlsx_smoke OK" << std::endl;
    return 0;
  }
  std::cerr << "xlsx_smoke 失败: " << g_failures << " 项" << std::endl;
  return 1;
}
