/* app/document_draft.js：单据录入页的本地草稿写入、恢复提示与恢复应用。
 * 由 document.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { askConfirm } from "./core_dialog.js";
import { clearDraft, readDraft, writeDraft } from "./core_draft.js";
import { showNotice } from "./core_state.js";
import { applyDocumentDraft, documentDraftForm } from "./document_lines.js";

  export function saveDocumentDraftNow(ctx) {
    // 编辑服务端已有草稿时不写本地草稿，避免和版本校验互相干扰。
    if (ctx.editingId || !ctx.dirty) return false;
    writeDraft(ctx.docType, {
      savedAt: Date.now(),
      enteredAt: ctx.draftEnteredAt,
      form: documentDraftForm(ctx),
    });
    return true;
  }

  export async function restoreDocumentDraftIfAny(ctx) {
    const draft = readDraft(ctx.docType);
    if (!draft) return;
    // 只恢复「进入表单后确实改过」的草稿：写入时间晚于该草稿记录的进入时间。
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
      // 放弃即显式丢弃，清掉本地草稿。
      clearDraft(ctx.docType);
      return;
    }
    applyDocumentDraft(ctx, draft);
    showNotice("已恢复本地草稿，可继续填写", "ok");
  }
