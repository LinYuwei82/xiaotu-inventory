/* app/masterdata_actions.js：通用 CRUD 的新增/保存/取消与列表编辑、删除。
 * 由 masterdata.js 拆出；页面状态通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { showNotice } from "./core_state.js";
import { loadOptions, openForm, readForm } from "./masterdata_form.js";

  export function bindFormActions(ctx) {
    const def = ctx.def;
    ctx.mount.querySelector('[data-role="new"]').addEventListener("click", async () => {
      await loadOptions(ctx);
      openForm(ctx, null);
    });
    ctx.mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      ctx.formCard.hidden = true;
    });
    ctx.mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const payload = readForm(ctx);
      for (const field of def.fields) {
        if (field.required && !payload[field.key]) {
          showNotice(field.label + "不能为空", "error");
          return;
        }
        if (field.type === "int") {
          const value = payload[field.key];
          const tooSmall = field.min !== undefined && value < field.min;
          const tooLarge = field.max !== undefined && value > field.max;
          if (!Number.isInteger(value) || tooSmall || tooLarge) {
            const range =
              field.min !== undefined && field.max !== undefined
                ? "应在 " + field.min + "–" + field.max + " 之间"
                : "应为整数";
            showNotice(field.label + range, "error");
            return;
          }
        }
      }
      if (def.beforeSave) {
        const problem = def.beforeSave(payload);
        if (problem) {
          showNotice(problem, "error");
          return;
        }
      }
      try {
        let saved = null;
        if (ctx.editingId) {
          saved = await api("PUT", def.api + "/" + ctx.editingId, payload);
        } else {
          saved = await api("POST", def.api, payload);
        }
        if (def.afterSave) {
          await def.afterSave(saved, payload, ctx.editingId);
        }
        ctx.formCard.hidden = true;
        showNotice("");
        await ctx.reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  export function bindTableActions(ctx) {
    const def = ctx.def;
    ctx.table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const row = event.target.closest("tr");
      const id = Number(row.dataset.id);
      if (button.dataset.role === "edit") {
        await loadOptions(ctx);
        openForm(ctx, ctx.rows.find((item) => item.id === id));
      } else if (button.dataset.role === "delete") {
        if (!(await askConfirm("确认删除这条记录？"))) return;
        try {
          await api("DELETE", def.api + "/" + id);
          await ctx.reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });
  }
