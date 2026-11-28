/* app/document_save.js：单据录入页的保存/审核提交与编辑已有草稿。
 * 由 document.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { showNotice } from "./core_state.js";
import { documentLinesHead } from "./document_view.js";
import { collectDocumentLines } from "./document_lines.js";

  export async function saveDocument(ctx, draft) {
    const lines = collectDocumentLines(ctx);
    if (!lines) return;
    const partnerInput = ctx.form.querySelector('[data-role="partner"]');
    const usePartnerSelect = ctx.partnerSelect && !ctx.partnerSelect.hidden;
    const partnerOption = usePartnerSelect ? ctx.partnerSelect.selectedOptions[0] : null;
    const body = {
      doc_type: ctx.docType,
      warehouse_id: ctx.costAdjust ? 0 : Number(ctx.warehouseSelect.value),
      target_warehouse_id: ctx.config.twoWarehouse ? Number(ctx.targetWarehouseSelect.value) : 0,
      doc_date: ctx.dateInput.value,
      partner_id: usePartnerSelect ? Number(ctx.partnerSelect.value || 0) : 0,
      partner: usePartnerSelect
        ? partnerOption && partnerOption.value
          ? partnerOption.dataset.name || ""
          : ""
        : partnerInput.value.trim(),
      biz_category: ctx.otherDoc ? ctx.bizCategorySelect.value : "",
      salesperson_id: ctx.salesDoc ? Number(ctx.salespersonSelect.value || 0) : 0,
      remark: ctx.remarkInput.value.trim(),
      lines,
    };
    const submit = async (confirmCredit) => {
      if (ctx.editingId) {
        // 带上读取时的版本；被他人改过会返回 VERSION_CONFLICT。
        const updated = await api("PUT", "/api/documents/" + ctx.editingId, {
          ...body,
          confirm_credit: confirmCredit,
          version: ctx.editingVersion,
        });
        ctx.editingVersion = updated ? updated.version ?? ctx.editingVersion : ctx.editingVersion;
        if (!draft) {
          await api(
            "POST",
            "/api/documents/" + ctx.editingId + "/approve" +
              (confirmCredit ? "?confirm_credit=1" : ""),
            { version: ctx.editingVersion }
          );
        }
        return null;
      }
      return api("POST", "/api/documents", {
        ...body,
        save_as_draft: draft,
        confirm_credit: confirmCredit,
        request_id: ctx.requestId,
      });
    };
    const finish = async (result) => {
      showNotice(
        result
          ? (draft ? "已存为草稿：" : "已保存并审核：") + result.doc_no
          : draft
            ? "草稿已保存"
            : "已保存并审核",
        "ok"
      );
      ctx.resetForm();
      await ctx.reload();
    };
    try {
      await finish(await submit(false));
    } catch (error) {
      if (error.code !== "CREDIT_LIMIT_WARNING") {
        showNotice(error.message, "error");
        return;
      }
      const ok = await askConfirm(error.message + "。仍要继续审核吗？", {
        okText: "继续出库",
        cancelText: "取消",
      });
      if (!ok) return;
      try {
        await finish(await submit(true));
      } catch (retryError) {
        showNotice(retryError.message, "error");
      }
    }
  }

  export async function editDocument(ctx, doc) {
    try {
      const detail = await api("GET", "/api/documents/" + doc.id);
      ctx.editingId = doc.id;
      ctx.editingVersion = detail.version ?? -1;
      ctx.warehouseSelect.value = String(detail.warehouse_id);
      ctx.dateInput.value = detail.doc_date;
      if (ctx.config.twoWarehouse && detail.target_warehouse_id) {
        ctx.targetWarehouseSelect.value = String(detail.target_warehouse_id);
      }
      ctx.form.querySelector('[data-role="partner"]').value = detail.partner || "";
      if (ctx.partnerSelect && !ctx.partnerSelect.hidden) {
        ctx.partnerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
      }
      if (ctx.otherDoc) ctx.bizCategorySelect.value = detail.biz_category || "OTHER";
      if (ctx.salesDoc) ctx.salespersonSelect.value = String(detail.salesperson_id || 0);
      ctx.remarkInput.value = detail.remark || "";
      ctx.linesTable.innerHTML = documentLinesHead(ctx.costAdjust);
      for (const line of detail.lines) ctx.addLine(line);
      ctx.dirty = false;
      ctx.mount.querySelector('[data-role="cancel-edit"]').hidden = false;
      ctx.form.scrollIntoView({ behavior: "smooth", block: "start" });
      showNotice("正在编辑草稿 " + detail.doc_no, "ok");
    } catch (error) {
      showNotice(error.message, "error");
    }
  }
