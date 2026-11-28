/* app/print_template.js：打印模板配置页。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, escapeHtml, showNotice } from "./core.js";

  export async function printTemplatePage(mount) {
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>打印模板</h2>
          <p class="page-sub">模板只保存纸张、页眉页脚与列显示；列来自目标注册表，每个目标可设一个默认模板</p>
        </div>
        <div class="toolbar">
          <select data-role="filter-target"></select>
          <button class="btn" data-role="new">新建模板</button>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card" data-role="editor-card" hidden>
        <div class="card-head">
          <h3 data-role="editor-title">新建模板</h3>
          <button class="btn small" type="button" data-role="copy-default">从内置默认复制列</button>
        </div>
        <form data-role="editor">
          <div class="doc-head">
            <label class="field"><span>名称</span><input data-role="name" maxlength="100" /></label>
            <label class="field"><span>编码</span><input data-role="code" maxlength="50" /></label>
            <label class="field"><span>目标</span><select data-role="target"></select></label>
            <label class="field"><span>纸张</span>
              <select data-role="page-size">
                <option value="A4">A4</option>
                <option value="A5">A5</option>
                <option value="241x140">针式 241×140 mm</option>
                <option value="241x93">针式 241×93 mm</option>
                <option value="CUSTOM">自定义</option>
              </select>
            </label>
            <label class="field"><span>方向</span>
              <select data-role="orientation">
                <option value="PORTRAIT">纵向</option>
                <option value="LANDSCAPE">横向</option>
              </select>
            </label>
            <label class="field"><span>纸张类别</span>
              <select data-role="paper-kind">
                <option value="SHEET">单页纸</option>
                <option value="CONTINUOUS">连续纸（针式）</option>
              </select>
            </label>
            <label class="field"><span>缩放</span>
              <select data-role="scale-mode">
                <option value="FIT_WIDTH">按纸宽自适应</option>
                <option value="ORIGINAL">原始尺寸</option>
              </select>
            </label>
            <label class="field"><span>页边距（mm）</span>
              <input type="number" min="0" max="50" data-role="margin" />
            </label>
            <label class="field"><span>字号（pt）</span>
              <input type="number" min="6" max="24" data-role="font-size" />
            </label>
            <label class="field" data-role="custom-size"><span>纸张宽（mm）</span>
              <input type="number" min="1" max="1000" data-role="paper-width" />
            </label>
            <label class="field" data-role="custom-size"><span>纸张高（mm）</span>
              <input type="number" min="1" max="1000" data-role="paper-height" />
            </label>
            <label class="field"><span>页眉文本</span>
              <input data-role="header" maxlength="500" placeholder="可用 {title} {date} {doc_no} 等占位符" />
            </label>
            <label class="field"><span>页脚文本</span>
              <input data-role="footer" maxlength="500" placeholder="纯文本，打印时按占位符替换" />
            </label>
            <label class="checkbox"><input type="checkbox" data-role="show-page-no" />显示页码</label>
            <label class="checkbox"><input type="checkbox" data-role="is-default" />设为该目标默认</label>
            <label class="checkbox"><input type="checkbox" data-role="active" />启用</label>
          </div>
          <p class="page-sub" data-role="layout-hint"></p>
          <table class="table print-columns" data-role="columns"></table>
          <div class="form-actions">
            <button class="btn primary" type="button" data-role="save">保存</button>
            <button class="btn" type="button" data-role="cancel">取消</button>
          </div>
        </form>
      </div>
      <div class="card">
        <table class="table" data-role="list"></table>
      </div>`;

    const filterSelect = mount.querySelector('[data-role="filter-target"]');
    const editorCard = mount.querySelector('[data-role="editor-card"]');
    const editorTitle = mount.querySelector('[data-role="editor-title"]');
    const editorTarget = mount.querySelector('[data-role="target"]');
    const nameInput = mount.querySelector('[data-role="name"]');
    const codeInput = mount.querySelector('[data-role="code"]');
    const pageSizeSelect = mount.querySelector('[data-role="page-size"]');
    const orientationSelect = mount.querySelector('[data-role="orientation"]');
    const paperKindSelect = mount.querySelector('[data-role="paper-kind"]');
    const scaleModeSelect = mount.querySelector('[data-role="scale-mode"]');
    const layoutHint = mount.querySelector('[data-role="layout-hint"]');
    const marginInput = mount.querySelector('[data-role="margin"]');
    const fontInput = mount.querySelector('[data-role="font-size"]');
    const paperWidthInput = mount.querySelector('[data-role="paper-width"]');
    const paperHeightInput = mount.querySelector('[data-role="paper-height"]');
    const headerInput = mount.querySelector('[data-role="header"]');
    const footerInput = mount.querySelector('[data-role="footer"]');
    const showPageNoInput = mount.querySelector('[data-role="show-page-no"]');
    const isDefaultInput = mount.querySelector('[data-role="is-default"]');
    const activeInput = mount.querySelector('[data-role="active"]');
    const customSizeFields = mount.querySelectorAll('[data-role="custom-size"]');
    const columnsTable = mount.querySelector('[data-role="columns"]');
    const listTable = mount.querySelector('[data-role="list"]');

    let targets = [];
    let templates = [];
    let editing = null;
    let editingColumns = [];

    function splitTarget(value) {
      const index = value.indexOf("|");
      return { type: value.slice(0, index), key: value.slice(index + 1) };
    }

    function targetLabel(type, key) {
      const target = targets.find((item) => item.type === type && item.key === key);
      return target ? target.name : type + "/" + key;
    }

    function fillTargetOptions(select, includeAll) {
      const groups = [
        { label: "单据", type: "DOC" },
        { label: "报表", type: "REPORT" },
      ];
      select.innerHTML =
        (includeAll ? '<option value="">全部目标</option>' : "") +
        groups
          .map((group) => {
            const options = targets
              .filter((target) => target.type === group.type)
              .map(
                (target) =>
                  `<option value="${target.type}|${target.key}">${escapeHtml(target.name)}</option>`
              )
              .join("");
            return options ? `<optgroup label="${group.label}">${options}</optgroup>` : "";
          })
          .join("");
    }

    function builtinColumns(type, key) {
      const target = targets.find((item) => item.type === type && item.key === key);
      if (!target) return [];
      return target.builtin.columns.map((column) => ({
        column_key: column.column_key,
        visible: column.visible,
        width_mm: column.width_mm,
        align: column.align,
        font_size_pt: column.font_size_pt,
        bold: column.bold,
      }));
    }

    // 针式预设只改前端选择项，落库仍是 CUSTOM + 宽高，不需要改纸张枚举与表结构。
    const paperPresets = {
      "241x140": { width: 241, height: 140 },
      "241x93": { width: 241, height: 93 },
    };

    function paperPresetKey(template) {
      if (template && template.page_size === "CUSTOM") {
        for (const [key, size] of Object.entries(paperPresets)) {
          if (Number(template.paper_width_mm) === size.width &&
              Number(template.paper_height_mm) === size.height) {
            return key;
          }
        }
      }
      return template ? template.page_size : "A4";
    }

    function updateCustomSize() {
      const preset = paperPresets[pageSizeSelect.value];
      if (preset) {
        paperWidthInput.value = preset.width;
        paperHeightInput.value = preset.height;
        // 针式预设默认走连续纸，避免和 A4 的按纸宽缩放混在一起。
        paperKindSelect.value = "CONTINUOUS";
      } else if (pageSizeSelect.value === "A4" || pageSizeSelect.value === "A5") {
        paperKindSelect.value = "SHEET";
      }
      const custom = pageSizeSelect.value === "CUSTOM";
      for (const field of customSizeFields) field.hidden = !custom;
      updateLayoutHint();
    }

    function pageDimensionsMm() {
      const preset = paperPresets[pageSizeSelect.value];
      let width = 210;
      let height = 297;
      if (preset) {
        width = preset.width;
        height = preset.height;
      } else if (pageSizeSelect.value === "A5") {
        width = 148;
        height = 210;
      } else if (pageSizeSelect.value === "CUSTOM") {
        width = Number(paperWidthInput.value || 0) || 210;
        height = Number(paperHeightInput.value || 0) || 297;
      }
      if (orientationSelect.value === "LANDSCAPE") [width, height] = [height, width];
      return { width, height };
    }

    function updateLayoutHint() {
      if (!layoutHint) return;
      const { width } = pageDimensionsMm();
      const margin = Number(marginInput.value || 0);
      const printable = Math.max(0, width - margin * 2);
      const tableWidth = editingColumns
        .filter((column) => column.visible)
        .reduce((sum, column) => sum + (Number(column.width_mm) || 18), 0);
      const continuous = paperKindSelect.value === "CONTINUOUS";
      const scaleMode = scaleModeSelect.value;
      let text = `可打印宽 ${printable}mm，当前可见列合计 ${tableWidth}mm。`;
      if (continuous) {
        text += " 连续纸固定原始尺寸与套打位置，不参与缩放。";
        if (tableWidth > printable) {
          text += ` 超出 ${tableWidth - printable}mm，请调整纸宽或精简列。`;
        }
      } else if (scaleMode === "ORIGINAL") {
        text += " 原始尺寸";
        text += tableWidth > printable
          ? `，超出 ${tableWidth - printable}mm，打印/导出前会提示改横向或精简列。`
          : "，不会缩放。";
      } else if (tableWidth > printable) {
        const scale = Math.max(10, Math.floor((printable * 100) / tableWidth));
        const font = ((Number(fontInput.value || 10) * scale) / 100).toFixed(1);
        text += ` 将按 ${scale}% 缩放，字号约 ${font}pt`;
        text += Number(font) < 7 ? "，低于 7pt，建议改横向或精简列。" : "。";
      } else {
        text += " 宽度足够，按 100% 输出。";
      }
      if (showPageNoInput && showPageNoInput.checked) {
        text += " 页码随打印引擎而定；网络版可在浏览器打印对话框勾选页眉和页脚。";
      }
      layoutHint.textContent = text;
    }

    function renderColumns() {
      const parts = splitTarget(editorTarget.value);
      const target = targets.find(
        (item) => item.type === parts.type && item.key === parts.key
      );
      const defs = target ? target.columns : [];
      if (!editingColumns.length) {
        columnsTable.innerHTML =
          '<tbody><tr><td class="empty">请先选择打印目标</td></tr></tbody>';
        return;
      }
      columnsTable.innerHTML = `
        <thead><tr><th>显示</th><th>列</th><th>宽度（mm，0 自动）</th><th>对齐</th>
          <th>字号（0 跟随）</th><th>加粗</th><th class="actions">排序</th></tr></thead>
        <tbody>${editingColumns
          .map((column, index) => {
            const def = defs.find((item) => item.key === column.column_key);
            return `<tr data-index="${index}">
              <td><input type="checkbox" data-role="col-visible" ${column.visible ? "checked" : ""} /></td>
              <td>${escapeHtml(def ? def.label : column.column_key)}
                <div class="page-sub">${escapeHtml(column.column_key)}</div></td>
              <td><input type="number" min="0" max="500" class="col-num" data-role="col-width"
                value="${Number(column.width_mm) || 0}" /></td>
              <td><select data-role="col-align">
                <option value="LEFT" ${column.align === "LEFT" ? "selected" : ""}>左</option>
                <option value="CENTER" ${column.align === "CENTER" ? "selected" : ""}>中</option>
                <option value="RIGHT" ${column.align === "RIGHT" ? "selected" : ""}>右</option>
              </select></td>
              <td><input type="number" min="0" max="24" class="col-num" data-role="col-font"
                value="${Number(column.font_size_pt) || 0}" /></td>
              <td><input type="checkbox" data-role="col-bold" ${column.bold ? "checked" : ""} /></td>
              <td class="actions">
                <button class="link" type="button" data-role="col-up" ${index === 0 ? "disabled" : ""}>上移</button>
                <button class="link" type="button" data-role="col-down"
                  ${index === editingColumns.length - 1 ? "disabled" : ""}>下移</button>
              </td>
            </tr>`;
          })
          .join("")}</tbody>`;
    }

    function syncColumnsFromDom() {
      const rows = columnsTable.querySelectorAll("tbody tr[data-index]");
      rows.forEach((row, index) => {
        const column = editingColumns[index];
        if (!column) return;
        column.visible = row.querySelector('[data-role="col-visible"]').checked;
        column.width_mm = Number(row.querySelector('[data-role="col-width"]').value || 0);
        column.align = row.querySelector('[data-role="col-align"]').value;
        column.font_size_pt = Number(row.querySelector('[data-role="col-font"]').value || 0);
        column.bold = row.querySelector('[data-role="col-bold"]').checked;
      });
    }

    function openEditor(template) {
      editing = template;
      editorCard.hidden = false;
      editorTitle.textContent = template ? "编辑模板" : "新建模板";
      editorTarget.disabled = !!template;
      nameInput.value = template ? template.name : "";
      codeInput.value = template ? template.code : "";
      editorTarget.value = template
        ? template.target_type + "|" + template.target_key
        : filterSelect.value || (targets[0] ? targets[0].type + "|" + targets[0].key : "");
      pageSizeSelect.value = template ? paperPresetKey(template) : "A4";
      orientationSelect.value = template ? template.orientation : "PORTRAIT";
      marginInput.value = template ? template.margin_mm : 12;
      fontInput.value = template ? template.font_size_pt : 10;
      paperWidthInput.value = template ? template.paper_width_mm : 0;
      paperHeightInput.value = template ? template.paper_height_mm : 0;
      headerInput.value = template ? template.header_text : "";
      footerInput.value = template ? template.footer_text : "";
      showPageNoInput.checked = template ? template.show_page_no : true;
      isDefaultInput.checked = template ? template.is_default : false;
      activeInput.checked = template ? template.active : true;
      updateCustomSize();
      paperKindSelect.value = template ? template.paper_kind || "SHEET" : paperKindSelect.value;
      scaleModeSelect.value = template ? template.scale_mode || "FIT_WIDTH" : "FIT_WIDTH";
      if (template) {
        editingColumns = template.columns.map((column) => ({ ...column }));
      } else {
        const parts = splitTarget(editorTarget.value);
        editingColumns = builtinColumns(parts.type, parts.key);
      }
      renderColumns();
      updateLayoutHint();
      editorCard.scrollIntoView({ behavior: "smooth", block: "start" });
    }

    function closeEditor() {
      editing = null;
      editorCard.hidden = true;
    }

    function renderList() {
      const filtered = filterSelect.value
        ? templates.filter(
            (template) =>
              template.target_type + "|" + template.target_key === filterSelect.value
          )
        : templates;
      if (!filtered.length) {
        listTable.innerHTML =
          '<tbody><tr><td class="empty">还没有模板，点「新建模板」时会从内置默认复制一份</td></tr></tbody>';
        return;
      }
      listTable.innerHTML = `
        <thead><tr><th>名称</th><th>编码</th><th>目标</th><th>默认</th><th>状态</th>
          <th>版本</th><th>更新时间</th><th class="actions">操作</th></tr></thead>
        <tbody>${filtered
          .map(
            (template) => `<tr data-id="${template.id}">
              <td>${escapeHtml(template.name)}</td>
              <td>${escapeHtml(template.code)}</td>
              <td>${escapeHtml(targetLabel(template.target_type, template.target_key))}</td>
              <td>${template.is_default ? "是" : "—"}</td>
              <td>${template.active ? "启用" : "停用"}</td>
              <td>${template.version}</td>
              <td>${escapeHtml(template.updated_at)}</td>
              <td class="actions">
                <button class="link" data-role="edit">编辑</button>
                ${
                  template.is_default
                    ? ""
                    : '<button class="link" data-role="default">设为默认</button>'
                }
                <button class="link danger" data-role="delete">删除</button>
              </td>
            </tr>`
          )
          .join("")}</tbody>`;
    }

    async function reload() {
      templates = await api("GET", "/api/print-templates");
      renderList();
    }

    async function saveEditor() {
      syncColumnsFromDom();
      const parts = splitTarget(editorTarget.value);
      if (!parts.type || !parts.key) {
        showNotice("请选择打印目标", "error");
        return;
      }
      if (!nameInput.value.trim() || !codeInput.value.trim()) {
        showNotice("请填写模板名称与编码", "error");
        return;
      }
      const paperPreset = paperPresets[pageSizeSelect.value];
      const body = {
        name: nameInput.value.trim(),
        code: codeInput.value.trim(),
        target_type: parts.type,
        target_key: parts.key,
        page_size: paperPreset ? "CUSTOM" : pageSizeSelect.value,
        orientation: orientationSelect.value,
        paper_kind: paperKindSelect.value,
        scale_mode: scaleModeSelect.value,
        paper_width_mm: paperPreset
          ? paperPreset.width
          : Number(paperWidthInput.value || 0),
        paper_height_mm: paperPreset
          ? paperPreset.height
          : Number(paperHeightInput.value || 0),
        margin_mm: Number(marginInput.value || 0),
        font_size_pt: Number(fontInput.value || 10),
        header_text: headerInput.value,
        footer_text: footerInput.value,
        show_page_no: showPageNoInput.checked,
        is_default: isDefaultInput.checked,
        active: activeInput.checked,
        columns: editingColumns.map((column) => ({
          column_key: column.column_key,
          visible: column.visible,
          width_mm: Number(column.width_mm) || 0,
          align: column.align,
          font_size_pt: Number(column.font_size_pt) || 0,
          bold: column.bold,
        })),
      };
      try {
        if (editing) {
          body.version = editing.version;
          await api("PUT", "/api/print-templates/" + editing.id, body);
          showNotice("模板已保存", "ok");
        } else {
          await api("POST", "/api/print-templates", body);
          showNotice("模板已创建", "ok");
        }
        closeEditor();
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    editorTarget.addEventListener("change", () => {
      if (editing) return;
      const parts = splitTarget(editorTarget.value);
      editingColumns = builtinColumns(parts.type, parts.key);
      renderColumns();
    });
    pageSizeSelect.addEventListener("change", updateCustomSize);
    for (const select of [orientationSelect, paperKindSelect, scaleModeSelect]) {
      select.addEventListener("change", updateLayoutHint);
    }
    for (const input of [marginInput, fontInput, paperWidthInput, paperHeightInput]) {
      input.addEventListener("input", updateLayoutHint);
    }
    if (showPageNoInput) showPageNoInput.addEventListener("change", updateLayoutHint);
    columnsTable.addEventListener("input", () => {
      syncColumnsFromDom();
      updateLayoutHint();
    });
    columnsTable.addEventListener("change", () => {
      syncColumnsFromDom();
      updateLayoutHint();
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", saveEditor);
    mount.querySelector('[data-role="cancel"]').addEventListener("click", closeEditor);
    mount.querySelector('[data-role="copy-default"]').addEventListener("click", () => {
      syncColumnsFromDom();
      const parts = splitTarget(editorTarget.value);
      editingColumns = builtinColumns(parts.type, parts.key);
      renderColumns();
      showNotice("已按内置默认重置列", "ok");
    });
    columnsTable.addEventListener("click", (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const row = event.target.closest("tr");
      const index = Number(row.dataset.index);
      syncColumnsFromDom();
      if (button.dataset.role === "col-up" && index > 0) {
        [editingColumns[index - 1], editingColumns[index]] = [
          editingColumns[index],
          editingColumns[index - 1],
        ];
      } else if (button.dataset.role === "col-down" && index < editingColumns.length - 1) {
        [editingColumns[index + 1], editingColumns[index]] = [
          editingColumns[index],
          editingColumns[index + 1],
        ];
      } else {
        return;
      }
      renderColumns();
    });
    listTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      const template = templates.find((item) => item.id === id);
      if (!template) return;
      try {
        if (button.dataset.role === "edit") {
          openEditor(template);
        } else if (button.dataset.role === "default") {
          await api("POST", "/api/print-templates/" + id + "/default");
          showNotice("已设为默认模板", "ok");
          await reload();
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除模板「" + template.name + "」？删除后该目标回退到内置默认。"))) {
            return;
          }
          await api("DELETE", "/api/print-templates/" + id);
          if (editing && editing.id === id) closeEditor();
          showNotice("模板已删除", "ok");
          await reload();
        }
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    filterSelect.addEventListener("change", renderList);
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => {
      reload().catch((error) => showNotice(error.message, "error"));
    });
    mount.querySelector('[data-role="new"]').addEventListener("click", () => openEditor(null));

    targets = await api("GET", "/api/print/targets");
    fillTargetOptions(filterSelect, true);
    fillTargetOptions(editorTarget, false);
    await reload();
  }
