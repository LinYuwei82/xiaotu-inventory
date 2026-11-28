/* app/order_actions.js：订单列表的操作按钮（打印、导出、确认、编辑、删除、下推、关闭）。
 * 由 order.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { showNotice } from "./core_state.js";
import { openOrderConvert } from "./order_convert.js";
import { editOrder } from "./order_save.js";
import { exportPdfTarget, printTarget } from "./print.js";

  export function bindOrderListActions(ctx) {
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
        if (button.dataset.role === "approve") {
          if (!(await askConfirm(`确认订单后不可再改单，只能${ctx.config.convertTitle}或关闭。继续？`))) return;
          const doc = ctx.documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("订单已确认", "ok");
        } else if (button.dataset.role === "edit") {
          await editOrder(ctx, ctx.documentList.rowById(id));
          return;
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张订单草稿？"))) return;
          const doc = ctx.documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (ctx.editingId === id) ctx.resetForm();
          showNotice("订单草稿已删除", "ok");
        } else if (button.dataset.role === "convert") {
          await openOrderConvert(ctx, ctx.documentList.rowById(id));
          return;
        } else if (button.dataset.role === "close") {
          if (!(await askConfirm(`关闭后订单不能再${ctx.config.convertTitle}，确认？`))) return;
          await api("POST", "/api/documents/" + id + "/close");
          showNotice("订单已关闭", "ok");
        }
        await ctx.reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }
