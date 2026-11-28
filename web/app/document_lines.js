/* app/document_lines.js：单据录入页的明细行增删、行内联动计算、草稿表单快照与提交前收集。
 * 由 document.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入，逻辑与拆分前一致。
 */
import { money, parseQty, qtyAmountCents, qtyAmountText, qtyText, taxForAmount } from "./core_format.js";
import { newRequestId } from "./core_api.js";
import { showNotice } from "./core_state.js";
import { documentLineRowHtml, documentLinesHead, itemOptionsHtml, warehouseOptionsHtml } from "./document_view.js";

  export function addDocumentLine(ctx, line) {
    const row = document.createElement("tr");
    row.innerHTML = documentLineRowHtml(
      line,
      ctx.costAdjust,
      (selected) => itemOptionsHtml(ctx.items, selected),
      (selected) => warehouseOptionsHtml(ctx.warehouses, selected)
    );
    ctx.linesTable.querySelector("tbody").appendChild(row);
    if (ctx.costAdjust) {
      row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
      if (line) {
        row.querySelector('[data-role="adjust"]').value = (line.adjust_cents / 100).toFixed(2);
        row.querySelector('[data-role="reason"]').value = line.adjust_reason || "";
      }
      return;
    }
    row.querySelector('[data-role="remove"]').addEventListener("click", () => {
      row.remove();
      ctx.recalcTotals();
    });
    const recalc = () => {
      const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
      const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
      const amount = qtyAmountCents(qty, price);
      const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
      const includesTax = row.querySelector('[data-role="tax-included"]').checked;
      row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
      const lineTax = taxForAmount(amount, rateBp, includesTax).tax;
      row.querySelector('[data-role="tax"]').value = money(lineTax);
      row.querySelector('[data-role="with-tax"]').value = money(
        includesTax ? amount : amount + lineTax
      );
      ctx.recalcTotals();
    };
    row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
    row.querySelector('[data-role="price"]').addEventListener("input", recalc);
    row.querySelector('[data-role="tax-rate"]').addEventListener("input", recalc);
    row.querySelector('[data-role="tax-included"]').addEventListener("change", recalc);
    const itemSelect = row.querySelector('[data-role="item"]');
    const unitHint = row.querySelector('[data-role="unit"]');
    const updateUnit = () => {
      const item = ctx.items.find((candidate) => String(candidate.id) === String(itemSelect.value));
      unitHint.textContent = item && item.unit ? item.unit : "";
      // 新行跟随商品默认税率；编辑历史单据时保留落库税率。
      if (!line) {
        const bp = item && item.tax_rate_bp ? item.tax_rate_bp : 0;
        row.querySelector('[data-role="tax-rate"]').value = (bp / 100)
          .toFixed(2)
          .replace(/\.?0+$/, "");
      }
      recalc();
    };
    itemSelect.addEventListener("change", updateUnit);
    updateUnit();
    if (line) {
      row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
      row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
      row.querySelector('[data-role="tax-rate"]').value = ((line.tax_rate_bp || 0) / 100)
        .toFixed(2)
        .replace(/\.?0+$/, "");
      row.querySelector('[data-role="tax-included"]').checked = !!line.price_includes_tax;
      recalc();
    }
  }

  export function recalcDocumentTotals(ctx) {
    if (ctx.costAdjust) return;
    let qty = 0;
    let amount = 0;
    let tax = 0;
    let withTax = 0;
    for (const row of ctx.linesTable.querySelectorAll("tbody tr")) {
      const qtyScaled = parseQty(row.querySelector('[data-role="qty"]').value, true) || 0;
      const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
      const lineAmount = qtyAmountCents(qtyScaled, price);
      const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
      const includesTax = row.querySelector('[data-role="tax-included"]').checked;
      const lineTax = taxForAmount(lineAmount, rateBp, includesTax).tax;
      qty += Number(qtyScaled);
      amount += lineAmount;
      tax += lineTax;
      withTax += includesTax ? lineAmount : lineAmount + lineTax;
    }
    const totalsEl = ctx.mount.querySelector('[data-role="doc-totals"]');
    if (totalsEl) {
      totalsEl.textContent =
        "合计数量 " + qtyText(qty) + "，金额 " + money(amount) +
        "，税额 " + money(tax) + "，价税合计 " + money(withTax);
    }
  }

  export function resetDocumentLines(ctx) {
    ctx.linesTable.innerHTML = documentLinesHead(ctx.costAdjust);
    ctx.addLine();
    ctx.recalcTotals();
  }

  // 长表单本地草稿：只存业务字段，按单据类型分键，每 30 秒写一次。
  export function documentDraftForm(ctx) {
    const data = {
      warehouse_id: Number(ctx.warehouseSelect.value || 0),
      target_warehouse_id: ctx.config.twoWarehouse
        ? Number(ctx.targetWarehouseSelect.value || 0)
        : 0,
      doc_date: ctx.dateInput.value,
      partner: ctx.form.querySelector('[data-role="partner"]').value.trim(),
      partner_id: ctx.partnerSelect && !ctx.partnerSelect.hidden ? Number(ctx.partnerSelect.value || 0) : 0,
      biz_category: ctx.otherDoc ? ctx.bizCategorySelect.value : "",
      salesperson_id: ctx.salesDoc ? Number(ctx.salespersonSelect.value || 0) : 0,
      remark: ctx.remarkInput.value.trim(),
      lines: [],
    };
    for (const row of ctx.linesTable.querySelectorAll("tbody tr")) {
      const itemId = Number(row.querySelector('[data-role="item"]').value);
      if (!itemId) continue;
      if (ctx.costAdjust) {
        data.lines.push({
          item_id: itemId,
          warehouse_id: Number(row.querySelector('[data-role="line-warehouse"]').value || 0),
          adjust: row.querySelector('[data-role="adjust"]').value.trim(),
          reason: row.querySelector('[data-role="reason"]').value.trim(),
        });
        continue;
      }
      data.lines.push({
        item_id: itemId,
        qty: row.querySelector('[data-role="qty"]').value.trim(),
        price: row.querySelector('[data-role="price"]').value,
        tax_rate: row.querySelector('[data-role="tax-rate"]').value,
        tax_included: row.querySelector('[data-role="tax-included"]').checked,
      });
    }
    return data;
  }

  export function applyDocumentDraft(ctx, draft) {
    const data = draft.form;
    ctx.editingId = 0;
    ctx.editingVersion = -1;
    // 恢复出来的是新的提交意图，重新生成幂等键。
    ctx.requestId = newRequestId();
    ctx.mount.querySelector('[data-role="cancel-edit"]').hidden = true;
    if (data.warehouse_id) ctx.warehouseSelect.value = String(data.warehouse_id);
    if (ctx.config.twoWarehouse && data.target_warehouse_id) {
      ctx.targetWarehouseSelect.value = String(data.target_warehouse_id);
    }
    if (data.doc_date) ctx.dateInput.value = data.doc_date;
    ctx.form.querySelector('[data-role="partner"]').value = data.partner || "";
    if (ctx.partnerSelect && !ctx.partnerSelect.hidden) {
      ctx.partnerSelect.value = data.partner_id ? String(data.partner_id) : "";
    }
    if (ctx.otherDoc) ctx.bizCategorySelect.value = data.biz_category || "OTHER";
    if (ctx.salesDoc) ctx.salespersonSelect.value = String(data.salesperson_id || 0);
    ctx.remarkInput.value = data.remark || "";
    ctx.linesTable.innerHTML = documentLinesHead(ctx.costAdjust);
    for (const line of data.lines || []) {
      ctx.addLine(
        ctx.costAdjust
          ? {
              item_id: line.item_id,
              qty: "0",
              price_cents: 0,
              warehouse_id: line.warehouse_id,
              adjust_cents: Math.round(Number(line.adjust || 0) * 100),
              adjust_reason: line.reason || "",
            }
          : {
              item_id: line.item_id,
              // 草稿里存的就是十进制文本，这里只做合法性校验。
              qty: parseQty(line.qty, true) === null ? "0" : String(line.qty).trim(),
              price_cents: Math.round(Number(line.price || 0) * 100),
              tax_rate_bp: Math.round(Number(line.tax_rate || 0) * 100),
              price_includes_tax: Boolean(line.tax_included),
            }
      );
    }
    if (!ctx.linesTable.querySelector("tbody tr")) ctx.addLine();
    if (!ctx.costAdjust) ctx.recalcTotals();
    ctx.dirty = true;
  }

  export function collectDocumentLines(ctx) {
    const lines = [];
    for (const row of ctx.linesTable.querySelectorAll("tbody tr")) {
      const itemId = Number(row.querySelector('[data-role="item"]').value);
      if (ctx.costAdjust) {
        const warehouseId = Number(row.querySelector('[data-role="line-warehouse"]').value);
        const adjustText = row.querySelector('[data-role="adjust"]').value.trim();
        const adjust = Math.round(Number(adjustText) * 100);
        const reason = row.querySelector('[data-role="reason"]').value.trim();
        if (!itemId || !warehouseId || !adjustText || !Number.isFinite(adjust) || adjust === 0) {
          showNotice("请检查明细行的商品、仓库与调整金额", "error");
          return null;
        }
        if (!reason) {
          showNotice("请填写调整原因", "error");
          return null;
        }
        lines.push({
          item_id: itemId,
          qty: "0",
          price_cents: 0,
          warehouse_id: warehouseId,
          adjust_cents: adjust,
          adjust_reason: reason,
        });
        continue;
      }
      // 接口数量是十进制字符串；parseQty 只用于校验（它返回 ×10000 的定点整数）。
      const qtyInput = row.querySelector('[data-role="qty"]').value.trim();
      const qty = parseQty(qtyInput, ctx.docType === "STK_CNT");
      const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
      const rateBp = Math.round(
        Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100
      );
      if (!itemId || qty === null) {
        showNotice("请检查明细行的商品与数量", "error");
        return null;
      }
      lines.push({
        item_id: itemId,
        qty: qtyInput,
        price_cents: price,
        tax_rate_bp: rateBp,
        price_includes_tax: row.querySelector('[data-role="tax-included"]').checked,
      });
    }
    if (!lines.length) {
      showNotice("请至少添加一行明细", "error");
      return null;
    }
    return lines;
  }
