/* app/document_actions.js：单据列表的操作按钮（打印、导出、审核、反审核、删除、编辑）。
 * 由 document.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { downloadCsv, downloadXlsx } from "./core_export.js";
import { showNotice } from "./core_state.js";
import { editDocument } from "./document_save.js";
import { exportPdfTarget, printTarget } from "./print.js";

  export function bindDocumentListActions(ctx) {
    ctx.documentList.table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      try {
        if (button.dataset.role === "print") {
          await printTarget(
            "/api/print/render?target_type=DOC&target_key=" +
              ctx.docType +
              "&doc_id=" +
              id
          );
          return;
        }
        if (button.dataset.role === "pdf") {
          const doc = ctx.documentList.rowById(id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + ctx.docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "export-csv") {
          await downloadCsv("/api/documents/" + id + "/export?format=csv");
          showNotice("已导出 CSV", "ok");
          return;
        }
        if (button.dataset.role === "export-xlsx") {
          await downloadXlsx("/api/documents/" + id + "/export?format=xlsx");
          showNotice("已导出 Excel", "ok");
          return;
        }
        if (button.dataset.role === "reverse") {
          if (!(await askConfirm("反审核会生成反向库存流水，确认继续？"))) return;
          const doc = ctx.documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/reverse",
            doc ? { version: doc.version } : {}
          );
          showNotice("已反审核", "ok");
        } else if (button.dataset.role === "approve") {
          if (!(await askConfirm("审核后才会写库存流水，确认继续？"))) return;
          const doc = ctx.documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("已审核", "ok");
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张草稿？"))) return;
          const doc = ctx.documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (ctx.editingId === id) ctx.resetForm();
          showNotice("草稿已删除", "ok");
        } else if (button.dataset.role === "edit") {
          await editDocument(ctx, ctx.documentList.rowById(id));
          return;
        }
        await ctx.reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }
