/* app/masterdata_filters.js：通用 CRUD 列表的筛选条件渲染与查询串拼装。
 * 由 masterdata.js 拆出；页面状态通过 ctx 传入。
 */
import { api } from "./core_api.js";
import { escapeHtml } from "./core_format.js";
import { optionLabel } from "./masterdata_view.js";

  export async function bindFilters(ctx) {
    const def = ctx.def;
    const filtersBar = ctx.filtersBar;
    if (!def.filters) return;
    const parts = [];
    for (const filter of def.filters) {
      if (filter.optionsApi) {
        const options = await api("GET", filter.optionsApi);
        parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.zeroLabel ? `<option value="0">${escapeHtml(filter.zeroLabel)}</option>` : ""}
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}">${escapeHtml(
                      "　".repeat(Math.max(0, (option.depth || 1) - 1)) + optionLabel(option)
                    )}</option>`
                )
                .join("")}
            </select>
          </label>`);
      } else if (filter.options) {
        parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.options
                .map(
                  (option) =>
                    `<option value="${escapeHtml(option.value)}">${escapeHtml(option.label)}</option>`
                )
                .join("")}
            </select>
          </label>`);
      } else {
        parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <input type="text" data-filter="${escapeHtml(filter.key)}" placeholder="${escapeHtml(
            filter.placeholder || ""
          )}" />
          </label>`);
      }
    }
    parts.push('<button class="btn" type="button" data-role="filter-reset">重置</button>');
    filtersBar.innerHTML = parts.join("");
    filtersBar.addEventListener("change", () => ctx.reload());
    filtersBar.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
      filtersBar.querySelectorAll("[data-filter]").forEach((input) => {
        input.value = "";
      });
      ctx.reload();
    });
  }

  export function filterQuery(ctx) {
    const def = ctx.def;
    if (!def.filters) return "";
    const params = [];
    for (const filter of def.filters) {
      const input = ctx.filtersBar.querySelector('[data-filter="' + filter.key + '"]');
      const value = input ? input.value.trim() : "";
      if (value) params.push(encodeURIComponent(filter.key) + "=" + encodeURIComponent(value));
    }
    return params.length ? "?" + params.join("&") : "";
  }
