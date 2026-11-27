#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct ItemBarcodeRow {
  std::string barcode;
  bool primary = false;
};

// 某商品的全部条码，主码在前、其余按录入顺序。
std::vector<ItemBarcodeRow> item_barcodes(Db& db, std::int64_t item_id);

// 去首尾空白、丢弃空码；同一列表内重复时抛 INVALID_INPUT（消息带重复的码）。
std::vector<std::string> normalize_barcodes(const std::vector<std::string>& raw);

// 事务内整体替换某商品的条码：第一项为主码并同步 item.barcode；
// 与其他商品或其条码冲突时抛 AppError("DUPLICATE_BARCODE")。传空列表表示清空。
void replace_item_barcodes(Conn& conn, std::int64_t item_id,
                           const std::vector<std::string>& raw);
void replace_item_barcodes(Db& db, std::int64_t item_id, const std::vector<std::string>& raw);

}  // namespace xiaotu
