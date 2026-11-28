/* app/order.js：开单页（销售订单 / 采购订单）的配置、装配、加载与事件绑定。
 * 由原 web/app.js 按职责拆出；明细行、草稿、保存、下推与列表操作分别见
 * order_lines / order_draft / order_save / order_convert / order_actions。
 */
import { api, newRequestId } from "./core_api.js";
import { clearDraft, startDraftTimer } from "./core_draft.js";
import {
  partnerOptionsHtml,
  salespersonOptionsHtml,
  warehouseSelectOptionsHtml,
} from "./select_options.js";
import { createDocumentList } from "./documents.js";
import { orderListColumns, orderPageHtml } from "./order_view.js";
import { addOrderLine, resetOrderLines } from "./order_lines.js";
import { restoreOrderDraftIfAny, saveOrderDraftNow } from "./order_draft.js";
import { saveOrder } from "./order_save.js";
import { submitOrderConvert } from "./order_convert.js";
import { bindOrderListActions } from "./order_actions.js";

  export async function orderPage(mount, options = {}) {
    const config = {
      docType: "SAL_ORDER",
      title: "销售订单",
      subtitle: "只做需求确认与转出库来源，不占库存、不写流水、不产生往来",
      partnerLabel: "客户",
      partnerApi: "/api/customers",
      warehouseLabel: "预计出库仓",
      convertTitle: "转出库",
      convertWarehouseLabel: "出库仓库",
      convertDateLabel: "出库日期",
      convertAction: "转出库",
      convertSubmit: "确认转出",
      convertNotice: "已生成销售出库单：",
      convertedLabel: "已转出库",
      partialLabel: "部分转出",
      emptyLabel: "还没有销售订单",
      ...options,
    };
    const docType = config.docType;

    mount.innerHTML = orderPageHtml(config);

    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const customerSelect = mount.querySelector('[data-role="customer"]');
    const salespersonSelect = mount.querySelector('[data-role="salesperson"]');
    const remarkInput = mount.querySelector('[data-role="remark"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const convertCard = mount.querySelector('[data-role="convert-card"]');
    const convertTitle = mount.querySelector('[data-role="convert-title"]');
    const convertWarehouse = mount.querySelector('[data-role="convert-warehouse"]');
    const convertDate = mount.querySelector('[data-role="convert-date"]');
    const convertLines = mount.querySelector('[data-role="convert-lines"]');

    const documentList = createDocumentList({
      docType,
      mount: mount.querySelector('[data-role="doc-list"]'),
      emptyText: config.emptyLabel,
      columns: orderListColumns(config, docType),
    });

    // ctx 汇集页面可变状态与 DOM 引用，供 order_* 各模块共用。
    const ctx = {
      config,
      docType,
      mount,
      form,
      warehouseSelect,
      dateInput,
      customerSelect,
      salespersonSelect,
      remarkInput,
      linesTable,
      convertCard,
      convertTitle,
      convertWarehouse,
      convertDate,
      convertLines,
      documentList,
      warehouses: [],
      items: [],
      customers: [],
      editingId: 0,
      editingVersion: -1,
      requestId: newRequestId(),
      convertingId: 0,
      convertRequestId: "",
      // 订单录入的本地草稿：只有改过表单才写，按订单类型分键。
      formDirty: false,
      draftEnteredAt: Date.now(),
    };
    ctx.addLine = (line) => addOrderLine(ctx, line);
    ctx.resetLines = () => resetOrderLines(ctx);
    ctx.reload = () => reload();
    ctx.resetForm = () => resetForm();
    ctx.saveDraftNow = () => saveOrderDraftNow(ctx);

    dateInput.value = new Date().toISOString().slice(0, 10);
    convertDate.value = dateInput.value;

    async function reload() {
      const [warehouseList, itemList, customerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        api("GET", config.partnerApi),
        docType === "SAL_ORDER" ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      ctx.warehouses = warehouseList;
      ctx.items = itemList;
      ctx.customers = customerList;
      const warehouseOptions = warehouseSelectOptionsHtml(ctx.warehouses);
      warehouseSelect.innerHTML = warehouseOptions;
      convertWarehouse.innerHTML = warehouseOptions;
      customerSelect.innerHTML = partnerOptionsHtml(ctx.customers);
      if (salespersonSelect) {
        salespersonSelect.innerHTML = salespersonOptionsHtml(employeeList);
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        ctx.resetLines();
      }
    }

    function resetForm() {
      ctx.editingId = 0;
      ctx.editingVersion = -1;
      ctx.requestId = newRequestId();
      ctx.formDirty = false;
      // 确认保存成功、取消编辑都清掉本地草稿。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      customerSelect.value = "";
      if (salespersonSelect) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      ctx.resetLines();
    }

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => ctx.addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    // 订单表单只有改过才写本地草稿。
    form.addEventListener("input", () => {
      ctx.formDirty = true;
    });
    form.addEventListener("change", () => {
      ctx.formDirty = true;
    });
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", resetForm);
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => saveOrder(ctx, true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => saveOrder(ctx, false));
    mount.querySelector('[data-role="convert-submit"]').addEventListener("click", () => submitOrderConvert(ctx));
    mount.querySelector('[data-role="convert-cancel"]').addEventListener("click", () => {
      convertCard.hidden = true;
      ctx.convertingId = 0;
    });

    bindOrderListActions(ctx);

    await reload();
    startDraftTimer(ctx.saveDraftNow);
    await restoreOrderDraftIfAny(ctx);
  }
