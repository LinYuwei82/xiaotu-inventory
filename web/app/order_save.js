/* app/order_save.js：开单页的保存/确认提交与编辑已有订单草稿。
 * 由 order.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { showNotice } from "./core_state.js";
import { orderLinesHead } from "./order_view.js";
import { collectOrderLines } from "./order_lines.js";

  export async function saveOrder(ctx, draft) {
    const lines = collectOrderLines(ctx);
    if (!lines) return;
    const option = ctx.customerSelect.selectedOptions[0];
    const body = {
      doc_type: ctx.docType,
      warehouse_id: Number(ctx.warehouseSelect.value),
      doc_date: ctx.dateInput.value,
      partner_id: Number(ctx.customerSelect.value || 0),
      partner: option && option.value ? option.dataset.name || "" : "",
      salesperson_id: ctx.salespersonSelect ? Number(ctx.salespersonSelect.value || 0) : 0,
      remark: ctx.remarkInput.value.trim(),
      lines,
    };
    try {
      if (ctx.editingId) {
        const updated = await api("PUT", "/api/documents/" + ctx.editingId, {
          ...body,
          version: ctx.editingVersion,
        });
        ctx.editingVersion = updated ? updated.version ?? ctx.editingVersion : ctx.editingVersion;
        if (!draft) {
          await api("POST", "/api/documents/" + ctx.editingId + "/approve", {
            version: ctx.editingVersion,
          });
        }
        showNotice(draft ? "订单草稿已保存" : "订单已确认", "ok");
      } else {
        const result = await api("POST", "/api/documents", {
          ...body,
          save_as_draft: draft,
          request_id: ctx.requestId,
        });
        showNotice((draft ? "已存为草稿：" : "订单已确认：") + result.doc_no, "ok");
      }
      ctx.resetForm();
      await ctx.reload();
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  export async function editOrder(ctx, doc) {
    try {
      const detail = await api("GET", "/api/documents/" + doc.id);
      ctx.editingId = doc.id;
      ctx.editingVersion = detail.version ?? -1;
      ctx.warehouseSelect.value = String(detail.warehouse_id);
      ctx.dateInput.value = detail.doc_date;
      ctx.customerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
      if (ctx.salespersonSelect) ctx.salespersonSelect.value = String(detail.salesperson_id || 0);
      ctx.remarkInput.value = detail.remark || "";
      ctx.linesTable.innerHTML = orderLinesHead();
      for (const line of detail.lines) ctx.addLine(line);
      ctx.formDirty = false;
      ctx.mount.querySelector('[data-role="cancel-edit"]').hidden = false;
      ctx.form.scrollIntoView({ behavior: "smooth", block: "start" });
    } catch (error) {
      showNotice(error.message, "error");
    }
  }
