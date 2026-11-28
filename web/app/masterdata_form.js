/* app/masterdata_form.js：通用 CRUD 表单的下拉选项预载、打开与读取。
 * 由 masterdata.js 拆出；页面状态通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { fieldInputHtml } from "./masterdata_view.js";

  export async function loadOptions(ctx) {
    for (const field of ctx.def.fields) {
      if (field.optionsApi && !ctx.optionCache[field.optionsApi]) {
        ctx.optionCache[field.optionsApi] = await api("GET", field.optionsApi);
      }
    }
  }

  export function openForm(ctx, row) {
    const def = ctx.def;
    ctx.editingId = row ? row.id : 0;
    ctx.form.innerHTML =
      def.fields
        .map((field) =>
          fieldInputHtml(field, row ? row[field.key] : field.default, ctx.optionCache)
        )
        .join("") + (def.formExtra ? def.formExtra(row) : "");
    ctx.formCard.hidden = false;
    if (def.bindFormExtra) {
      def.bindFormExtra(ctx.form, row);
    }
  }

  export function readForm(ctx) {
    const payload = {};
    for (const field of ctx.def.fields) {
      const input = ctx.form.querySelector('[data-field="' + field.key + '"]');
      if (field.type === "bool") {
        payload[field.key] = input.checked;
      } else if (field.optionsApi) {
        payload[field.key] = Number(input.value || 0);
      } else if (field.type === "int") {
        payload[field.key] = Math.round(Number(input.value || 0) * (field.scale || 1));
      } else {
        payload[field.key] = input.value.trim();
      }
    }
    return payload;
  }
