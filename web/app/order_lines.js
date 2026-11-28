/* app/order_lines.js：开单页的明细行增删、行内金额联动、草稿表单快照与提交前收集。
 * 由 order.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { parseQty, qtyAmountText, qtyText } from "./core_format.js";
import { newRequestId } from "./core_api.js";
import { showNotice } from "./core_state.js";
import { itemOptionsHtml } from "./select_options.js";
import { orderLineRowHtml, orderLinesHead } from "./order_view.js";

  export function addOrderLine(ctx, line) {
    const row = document.createElement("tr");
    row.innerHTML = orderLineRowHtml(line, (selected) => itemOptionsHtml(ctx.items, selected));
    ctx.linesTable.querySelector("tbody").appendChild(row);
    row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
    const recalc = () => {
      const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
      const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
      row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
    };
    row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
    row.querySelector('[data-role="price"]').addEventListener("input", recalc);
    const itemSelect = row.querySelector('[data-role="item"]');
    const unitHint = row.querySelector('[data-role="unit"]');
    const updateUnit = () => {
      const item = ctx.items.find((candidate) => String(candidate.id) === String(itemSelect.value));
      unitHint.textContent = item && item.unit ? item.unit : "";
    };
    itemSelect.addEventListener("change", updateUnit);
    updateUnit();
    if (line) {
      row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
      row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
      recalc();
    }
  }

  export function resetOrderLines(ctx) {
    ctx.linesTable.innerHTML = orderLinesHead();
    ctx.addLine();
  }

  // 订单本地草稿：只存业务字段，按订单类型分键。
  export function orderDraftForm(ctx) {
    const data = {
      warehouse_id: Number(ctx.warehouseSelect.value || 0),
      doc_date: ctx.dateInput.value,
      partner_id: Number(ctx.customerSelect.value || 0),
      salesperson_id: ctx.salespersonSelect ? Number(ctx.salespersonSelect.value || 0) : 0,
      remark: ctx.remarkInput.value.trim(),
      lines: [],
    };
    for (const row of ctx.linesTable.querySelectorAll("tbody tr")) {
      const itemId = Number(row.querySelector('[data-role="item"]').value);
      if (!itemId) continue;
      data.lines.push({
        item_id: itemId,
        qty: row.querySelector('[data-role="qty"]').value.trim(),
        price: row.querySelector('[data-role="price"]').value,
      });
    }
    return data;
  }

  export function applyOrderDraft(ctx, draft) {
    const data = draft.form;
    ctx.editingId = 0;
    ctx.editingVersion = -1;
    ctx.requestId = newRequestId();
    ctx.mount.querySelector('[data-role="cancel-edit"]').hidden = true;
    if (data.warehouse_id) ctx.warehouseSelect.value = String(data.warehouse_id);
    if (data.doc_date) ctx.dateInput.value = data.doc_date;
    ctx.customerSelect.value = data.partner_id ? String(data.partner_id) : "";
    if (ctx.salespersonSelect) ctx.salespersonSelect.value = String(data.salesperson_id || 0);
    ctx.remarkInput.value = data.remark || "";
    ctx.linesTable.innerHTML = orderLinesHead();
    for (const line of data.lines || []) {
      ctx.addLine({
        item_id: line.item_id,
        // 草稿里存的就是十进制文本，这里只做合法性校验。
        qty: parseQty(line.qty, true) === null ? "0" : String(line.qty).trim(),
        price_cents: Math.round(Number(line.price || 0) * 100),
      });
    }
    if (!ctx.linesTable.querySelector("tbody tr")) ctx.addLine();
    ctx.formDirty = true;
  }

  export function collectOrderLines(ctx) {
    const lines = [];
    for (const row of ctx.linesTable.querySelectorAll("tbody tr")) {
      const itemId = Number(row.querySelector('[data-role="item"]').value);
      // 接口数量是十进制字符串；parseQty 只用于校验（它返回 ×10000 的定点整数）。
      const qtyInput = row.querySelector('[data-role="qty"]').value.trim();
      const qty = parseQty(qtyInput, ctx.docType === "STK_CNT");
      const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
      if (!itemId || qty === null) {
        showNotice("请检查明细行的商品与数量", "error");
        return null;
      }
      lines.push({ item_id: itemId, qty: qtyInput, price_cents: price });
    }
    if (!lines.length) {
      showNotice("请至少添加一行明细", "error");
      return null;
    }
    return lines;
  }
