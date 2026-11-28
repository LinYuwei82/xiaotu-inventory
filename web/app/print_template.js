/* app/print_template.js：打印模板配置页。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, escapeHtml, showNotice } from "./core.js";
import { printColumnsHtml, printTemplateListHtml, printTemplatePageHtml } from "./print_template_view.js";

  export async function printTemplatePage(mount) {
    mount.innerHTML = printTemplatePageHtml();

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
      columnsTable.innerHTML = printColumnsHtml(editingColumns, defs);
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
      listTable.innerHTML = printTemplateListHtml(filtered, targetLabel);
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
