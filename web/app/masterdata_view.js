/* app/masterdata_view.js：基础资料通用 CRUD 页的静态模板与表格渲染。
 * 由 masterdata.js 拆出，只放不依赖页面闭包状态的视图代码，模板文本与拆分前一致。
 */
import { escapeHtml } from "./core.js";

  // CRUD 页骨架：标题栏（导入/导出/合并/新增）+ 表单卡片 + 往来单位合并卡片 + 筛选条 + 列表。
  export function crudPageHtml(def, flags) {
    const { ioButtons, isTrader } = flags;
    return `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(def.title)}</h2>
          <p class="page-sub">${escapeHtml(def.api)}</p>
        </div>
        <div class="page-actions">
          ${ioButtons}
          ${isTrader && flags.mergeButton ? '<button class="btn" data-role="merge-open">合并单位</button>' : ""}
          <button class="btn primary" data-role="new">新增</button>
        </div>
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form"></form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      ${isTrader ? `<div class="card" data-role="merge-card" hidden>
        <div class="card-head">
          <h3>合并往来单位</h3>
          <button class="btn" type="button" data-role="merge-close">关闭</button>
        </div>
        <p class="page-sub" data-role="merge-hint"></p>
        <div data-role="merge-body"></div>
        <div class="card-head"><h3>最近合并与撤销</h3></div>
        <div data-role="merge-log"></div>
      </div>` : ""}
      ${def.filters ? '<div class="card filters" data-role="filters"></div>' : ""}
      <div class="card">
        <table class="table" data-role="table"></table>
      </div>`;
  }

  // 下拉选项的统一显示名：往来单位用全称，其余用「编码 名称」，都没有时退回 id。
  export function optionLabel(option) {
    if (option.full_name) return option.full_name;
    return [option.code, option.name].filter(Boolean).join(" ") || String(option.id);
  }

  // 表单字段：布尔是勾选框，optionsApi 走下拉，int 按 scale 缩放显示，其余按文本。
  export function fieldInputHtml(field, value, optionCache) {
    if (field.type === "bool") {
      return `<label class="checkbox">
            <input type="checkbox" data-field="${escapeHtml(field.key)}" ${
              value ? "checked" : ""
            } /> ${escapeHtml(field.label)}
          </label>`;
    }
    if (field.optionsApi) {
      const options = optionCache[field.optionsApi] || [];
      return `<label class="field">
            <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
            <select data-field="${escapeHtml(field.key)}" ${field.required ? "required" : ""}>
              <option value="0">未指定</option>
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}" ${
                      String(option.id) === String(value) ? "selected" : ""
                    }>${escapeHtml(
                      "　".repeat(Math.max(0, (option.depth || 1) - 1)) + optionLabel(option)
                    )}</option>`
                )
                .join("")}
            </select>
          </label>`;
    }
    if (field.type === "int") {
      const scale = field.scale || 1;
      const min = field.min === undefined ? "" : ` min="${Number(field.min) / scale}"`;
      const max = field.max === undefined ? "" : ` max="${Number(field.max) / scale}"`;
      const shown =
        value === undefined || value === null
          ? ""
          : Number(value) / scale;
      return `<label class="field">
          <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
          <input type="number" step="${scale === 1 ? "1" : "0.01"}"${min}${max} data-field="${escapeHtml(
          field.key
        )}" value="${escapeHtml(shown)}" ${field.required ? "required" : ""} />
        </label>`;
    }
    return `<label class="field">
          <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
          <input type="text" data-field="${escapeHtml(field.key)}" value="${escapeHtml(
        value === undefined || value === null ? "" : value
      )}" ${field.required ? "required" : ""} />
        </label>`;
  }

  // 列表表格：布尔显示是/否，万分比按百分数显示，其余转义后原样输出。
  export function crudTableHtml(rows, columns) {
    if (!rows.length) {
      return '<tbody><tr><td class="empty">暂无数据，点击「新增」开始</td></tr></tbody>';
    }
    const head = columns
      .map((column) => `<th>${escapeHtml(column.label)}</th>`)
      .join("");
    const body = rows
      .map((row) => {
        const cells = columns
          .map((column) => {
            const value = row[column.key];
            if (column.type === "bool") {
              return `<td>${value ? "是" : "否"}</td>`;
            }
            if (column.type === "percent_bp") {
              return `<td>${(Number(value || 0) / 100).toFixed(2)}%</td>`;
            }
            return `<td>${escapeHtml(value)}</td>`;
          })
          .join("");
        return `<tr data-id="${row.id}">${cells}
              <td class="actions">
                <button class="link" data-role="edit">编辑</button>
                <button class="link danger" data-role="delete">删除</button>
              </td>
            </tr>`;
      })
      .join("");
    return `<thead><tr>${head}<th class="actions">操作</th></tr></thead><tbody>${body}</tbody>`;
  }

  export function mergeOriginLabel(origin) {
    if (origin === "AUTO_ARCHIVE") return "自动补建";
    if (origin === "MANUAL") return "手工/合并";
    return origin || "—";
  }

  // 合并候选表：勾选要并入的档案，再单选一条作为保留档案。
  export function mergePanelHtml(partners, defKey) {
    if (!partners.length) return '<p class="page-sub">当前没有可合并的档案</p>';
    return `<table class="table"><thead><tr><th>合并</th><th>保留</th><th>编码</th>
               <th>名称</th><th>来源</th></tr></thead><tbody>${partners
               .map(
                 (row) => `<tr>
                   <td><input type="checkbox" data-merge-source value="${row.id}" /></td>
                   <td><input type="radio" name="merge-target-${defKey}" value="${row.id}" /></td>
                   <td>${escapeHtml(row.code)}</td>
                   <td>${escapeHtml(row.name)}</td>
                   <td>${escapeHtml(mergeOriginLabel(row.origin))}</td>
                 </tr>`
               )
               .join("")}</tbody></table>
             <div class="form-actions">
               <button class="btn primary" type="button" data-role="merge-run">合并所选</button>
               <span class="page-sub">先勾选要并入的档案，再选一条作为保留档案</span>
             </div>`;
  }

  // 最近合并记录：可撤销的给按钮，不可撤销的给出原因。
  export function mergeLogsHtml(mergeLogs) {
    return !mergeLogs.length
      ? '<p class="page-sub">还没有合并记录</p>'
      : `<table class="table"><thead><tr><th>时间</th><th>保留档案</th><th>被合并</th>
             <th>执行人</th><th>状态</th><th>操作</th></tr></thead><tbody>${mergeLogs
             .map(
               (log) => `<tr>
                 <td>${escapeHtml(log.created_at)}</td>
                 <td>${escapeHtml(log.target_code + " " + log.target_name)}</td>
                 <td>${escapeHtml(log.source_ids)}</td>
                 <td>${escapeHtml(log.actor)}</td>
                 <td>${log.undone_at ? "已撤销 " + escapeHtml(log.undone_at) : "已合并"}</td>
                 <td class="actions">${
                   log.undone_at
                     ? '<span class="page-sub">—</span>'
                     : log.can_undo
                       ? `<button class="link" data-role="undo" data-log="${log.id}" data-name="${escapeHtml(log.target_name)}">撤销</button>`
                       : `<span class="page-sub" title="${escapeHtml(log.undo_reason)}">不可撤销</span>`
                 }</td>
               </tr>`
             )
             .join("")}</tbody></table>`;
  }
