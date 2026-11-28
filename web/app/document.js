/* app/document.js：业务单据列表与单据详情编辑页的装配、加载与事件绑定。
 * 由原 web/app.js 按职责拆出；明细行、草稿、保存与列表操作分别见
 * document_lines / document_draft / document_save / document_actions。
 */
import { can } from "./core_state.js";
import { api, newRequestId } from "./core_api.js";
import { clearDraft, startDraftTimer } from "./core_draft.js";
import { setUnsavedGuard } from "./core.js";
import {
  documentListColumns,
  documentPageHtml,
  partnerOptionsHtml,
  salespersonOptionsHtml,
  warehouseSelectOptionsHtml,
} from "./document_view.js";
import { createDocumentList } from "./documents.js";
import { addDocumentLine, recalcDocumentTotals, resetDocumentLines } from "./document_lines.js";
import { restoreDocumentDraftIfAny, saveDocumentDraftNow } from "./document_draft.js";
import { saveDocument } from "./document_save.js";
import { bindDocumentListActions } from "./document_actions.js";

  export async function documentPage(mount, config) {
    const docType = config.docType;
    const costAdjust = !!config.costAdjust;
    // 只有其他出入库有业务类型；入库可用期初，出库可用领用与报损，赠品两方向通用。
    const otherDoc = docType === "OTH_IN" || docType === "OTH_OUT";
    const salesDoc = docType === "SAL_OUT" || docType === "SAL_RET";
    const flags = { costAdjust, otherDoc, salesDoc };

    mount.innerHTML = documentPageHtml(config, flags);

    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const targetWarehouseSelect = mount.querySelector('[data-role="target-warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const partnerSelect = mount.querySelector('[data-role="partner-select"]');
    const bizCategorySelect = mount.querySelector('[data-role="biz-category"]');
    const salespersonSelect = mount.querySelector('[data-role="salesperson"]');
    const remarkInput = mount.querySelector('[data-role="remark"]');

    const documentList = createDocumentList({
      docType,
      mount: mount.querySelector('[data-role="doc-list"]'),
      emptyText: `还没有${config.docLabel}单`,
      columns: documentListColumns(config, flags),
    });

    // ctx 汇集页面可变状态与 DOM 引用，供 document_* 各模块共用，避免层层传参。
    const ctx = {
      config,
      docType,
      costAdjust,
      otherDoc,
      salesDoc,
      flags,
      mount,
      form,
      warehouseSelect,
      targetWarehouseSelect,
      dateInput,
      linesTable,
      partnerSelect,
      bizCategorySelect,
      salespersonSelect,
      remarkInput,
      documentList,
      warehouses: [],
      items: [],
      editingId: 0,
      // 乐观锁版本与幂等键：版本随详情/列表带回，request_id 每次新建页生成一次。
      editingVersion: -1,
      requestId: newRequestId(),
      // 录入页的脏标记：任一字段变动即置脏，保存成功、重置或切换单据后清除。
      dirty: false,
      // 长表单本地草稿的进入时间：只恢复「进入后确实改过」的草稿。
      draftEnteredAt: Date.now(),
    };
    ctx.addLine = (line) => addDocumentLine(ctx, line);
    ctx.recalcTotals = () => recalcDocumentTotals(ctx);
    ctx.resetLines = () => resetDocumentLines(ctx);
    ctx.reload = () => reload();
    ctx.resetForm = () => resetForm();
    ctx.saveDraftNow = () => saveDocumentDraftNow(ctx);

    dateInput.value = new Date().toISOString().slice(0, 10);
    if (costAdjust && !can("cost.adjust")) {
      mount.querySelector('[data-role="submit"]').hidden = true;
    }

    async function reload() {
      const [warehouseList, itemList, partnerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        config.partnerApi ? api("GET", config.partnerApi) : Promise.resolve([]),
        salesDoc ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      ctx.warehouses = warehouseList;
      ctx.items = itemList;
      if (config.partnerApi) {
        partnerSelect.innerHTML = partnerOptionsHtml(partnerList);
      }
      if (salesDoc) {
        // 停用/离职职员仍列在历史单据里，这里保留可选项并标注，不隐藏。
        salespersonSelect.innerHTML = salespersonOptionsHtml(employeeList);
      }
      const options = warehouseSelectOptionsHtml(ctx.warehouses);
      warehouseSelect.innerHTML = options;
      if (config.twoWarehouse) {
        targetWarehouseSelect.innerHTML = options;
        if (ctx.warehouses.filter((warehouse) => warehouse.status).length > 1) {
          targetWarehouseSelect.selectedIndex = 1;
        }
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        ctx.resetLines();
      }
    }

    function resetForm() {
      ctx.editingId = 0;
      ctx.editingVersion = -1;
      // 提交成功/取消后作废幂等键，下一次新建重新生成。
      ctx.requestId = newRequestId();
      ctx.dirty = false;
      // 提交成功、取消编辑都算显式结束，本地草稿一并清掉。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      form.querySelector('[data-role="partner"]').value = "";
      if (partnerSelect) partnerSelect.value = "";
      if (otherDoc) bizCategorySelect.value = "OTHER";
      if (salesDoc) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      ctx.resetLines();
    }

    // 只有真正录过内容才拦：任一输入/选择变动即置脏，离开前确认一次。
    form.addEventListener("input", () => {
      ctx.dirty = true;
    });
    form.addEventListener("change", () => {
      ctx.dirty = true;
    });
    setUnsavedGuard(() => ctx.dirty);

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => ctx.addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", () => resetForm());
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => saveDocument(ctx, true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => saveDocument(ctx, false));

    bindDocumentListActions(ctx);

    await reload();
    startDraftTimer(ctx.saveDraftNow);
    await restoreDocumentDraftIfAny(ctx);
  }
