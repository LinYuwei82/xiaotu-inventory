/* app/select_options.js：单据页与开单页共用的下拉选项 HTML（商品 / 仓库 / 往来单位 / 业务员）。
 * 由 document_view.js 拆出，供 document_* 与 order_* 共用，保证全站选项口径一致。
 */
import { escapeHtml, itemOptionText } from "./core_format.js";

  export function itemOptionsHtml(items, selected) {
    return items
      .map(
        (item) =>
          `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
            itemOptionText(item)
          )}</option>`
      )
      .join("");
  }

  export function warehouseOptionsHtml(warehouses, selected) {
    return warehouses
      .filter((warehouse) => warehouse.status)
      .map(
        (warehouse) =>
          `<option value="${warehouse.id}" ${
            String(warehouse.id) === String(selected) ? "selected" : ""
          }>${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
      )
      .join("");
  }

  export function warehouseSelectOptionsHtml(warehouses) {
    return warehouses
      .filter((warehouse) => warehouse.status)
      .map(
        (warehouse) =>
          `<option value="${warehouse.id}">${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
      )
      .join("");
  }

  export function partnerOptionsHtml(partners) {
    return (
      '<option value="">不指定</option>' +
      partners
        .filter((partner) => partner.status)
        .map(
          (partner) =>
            `<option value="${partner.id}" data-name="${escapeHtml(partner.name)}">${escapeHtml(
              partner.code + " " + partner.name
            )}</option>`
        )
        .join("")
    );
  }

  // 停用/离职职员仍列在历史单据里，这里保留可选项并标注，不隐藏。
  export function salespersonOptionsHtml(employees) {
    return (
      '<option value="0">未指定</option>' +
      employees
        .map(
          (person) =>
            `<option value="${person.id}">${escapeHtml(
              person.code + " " + person.name
            )}${person.status ? "" : "（停用）"}</option>`
        )
        .join("")
    );
  }
