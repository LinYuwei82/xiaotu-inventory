/* app/order_draft.js：开单页的本地草稿写入、恢复提示与恢复应用。
 * 由 order.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { askConfirm } from "./core_dialog.js";
import { clearDraft, readDraft, writeDraft } from "./core_draft.js";
import { showNotice } from "./core_state.js";
import { applyOrderDraft, orderDraftForm } from "./order_lines.js";

  export function saveOrderDraftNow(ctx) {
    if (ctx.editingId || !ctx.formDirty) return false;
    writeDraft(ctx.docType, {
      savedAt: Date.now(),
      enteredAt: ctx.draftEnteredAt,
      form: orderDraftForm(ctx),
    });
    return true;
  }

  export async function restoreOrderDraftIfAny(ctx) {
    const draft = readDraft(ctx.docType);
    if (!draft) return;
    if (!(Number(draft.savedAt) > Number(draft.enteredAt || 0))) {
      clearDraft(ctx.docType);
      return;
    }
    const when = new Date(Number(draft.savedAt));
    const pad = (value) => String(value).padStart(2, "0");
    const stamp =
      pad(when.getMonth() + 1) +
      "-" +
      pad(when.getDate()) +
      " " +
      pad(when.getHours()) +
      ":" +
      pad(when.getMinutes());
    const restore = await askConfirm(
      "发现「" + ctx.config.title + "」未提交的本地草稿（保存于 " + stamp + "），是否恢复？",
      { okText: "恢复", cancelText: "放弃" }
    );
    if (!restore) {
      clearDraft(ctx.docType);
      return;
    }
    applyOrderDraft(ctx, draft);
    showNotice("已恢复本地草稿，可继续填写", "ok");
  }
