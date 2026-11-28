/* app/order.js：开单页：采购/销售/调拨等下单流程。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { showNotice } from "./core_state.js";
import { escapeHtml, itemOptionText, parseQty, qtyAmountText, qtyText } from "./core_format.js";
import { askConfirm } from "./core_dialog.js";
import { clearDraft, readDraft, startDraftTimer, writeDraft } from "./core_draft.js";
import { api, newRequestId } from "./core_api.js";
import { createDocumentList } from "./documents.js";
import { orderConvertLinesHtml, orderLineRowHtml, orderLinesHead, orderListColumns, orderPageHtml } from "./order_view.js";
import { exportPdfTarget, printTarget } from "./print.js";

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
    let warehouses = [];
    let items = [];
    let customers = [];
    let editingId = 0;
    let editingVersion = -1;
    let requestId = newRequestId();
    let convertingId = 0;
    let convertRequestId = "";
    // 订单录入的本地草稿：只有改过表单才写，按订单类型分键。
    let formDirty = false;
    const draftEnteredAt = Date.now();

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
    const documentsTable = documentList.table;

    dateInput.value = new Date().toISOString().slice(0, 10);
    convertDate.value = dateInput.value;

    function itemLabelById(itemId) {
      const item = items.find((row) => row.id === itemId);
      return item ? itemOptionText(item) : String(itemId);
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              itemOptionText(item)
            )}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      row.innerHTML = orderLineRowHtml(line, itemOptions);
      linesTable.querySelector("tbody").appendChild(row);
      row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
      const recalc = () => {
        const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      const itemSelect = row.querySelector('[data-role="item"]');
      const unitHint = row.querySelector('[data-role="unit"]');
      const updateUnit = () => {
        const item = items.find((candidate) => String(candidate.id) === String(itemSelect.value));
        unitHint.textContent = item && item.unit ? item.unit : "";
      };
      itemSelect.addEventListener("change", updateUnit);
      updateUnit();
      if (line) {
        row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        recalc();
      }
    }

    function resetLines() {
      linesTable.innerHTML = orderLinesHead();
      addLine();
    }

    async function reload() {
      const [warehouseList, itemList, customerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        api("GET", config.partnerApi),
        docType === "SAL_ORDER" ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      warehouses = warehouseList;
      items = itemList;
      customers = customerList;
      const warehouseOptions = warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}">${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
      warehouseSelect.innerHTML = warehouseOptions;
      convertWarehouse.innerHTML = warehouseOptions;
      customerSelect.innerHTML =
        '<option value="">不指定</option>' +
        customers
          .filter((customer) => customer.status)
          .map(
            (customer) =>
              `<option value="${customer.id}" data-name="${escapeHtml(customer.name)}">${escapeHtml(
                customer.code + " " + customer.name
              )}</option>`
          )
          .join("");
      if (salespersonSelect) {
        salespersonSelect.innerHTML =
          '<option value="0">未指定</option>' +
          employeeList
            .map(
              (person) =>
                `<option value="${person.id}">${escapeHtml(
                  person.code + " " + person.name
                )}${person.status ? "" : "（停用）"}</option>`
            )
            .join("");
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      editingVersion = -1;
      requestId = newRequestId();
      formDirty = false;
      // 确认保存成功、取消编辑都清掉本地草稿。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      customerSelect.value = "";
      if (salespersonSelect) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
    }

    // 订单本地草稿：只存业务字段，按订单类型分键。
    function draftFormData() {
      const data = {
        warehouse_id: Number(warehouseSelect.value || 0),
        doc_date: dateInput.value,
        partner_id: Number(customerSelect.value || 0),
        salesperson_id: salespersonSelect ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines: [],
      };
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        if (!itemId) continue;
        data.lines.push({
          item_id: itemId,
          qty: row.querySelector('[data-role="qty"]').value.trim(),
          price: row.querySelector('[data-role="price"]').value,
        });
      }
      return data;
    }

    function saveDraftNow() {
      if (editingId || !formDirty) return false;
      writeDraft(docType, {
        savedAt: Date.now(),
        enteredAt: draftEnteredAt,
        form: draftFormData(),
      });
      return true;
    }

    function applyDraft(draft) {
      const data = draft.form;
      editingId = 0;
      editingVersion = -1;
      requestId = newRequestId();
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      if (data.warehouse_id) warehouseSelect.value = String(data.warehouse_id);
      if (data.doc_date) dateInput.value = data.doc_date;
      customerSelect.value = data.partner_id ? String(data.partner_id) : "";
      if (salespersonSelect) salespersonSelect.value = String(data.salesperson_id || 0);
      remarkInput.value = data.remark || "";
      linesTable.innerHTML = orderLinesHead();
      for (const line of data.lines || []) {
        addLine({
          item_id: line.item_id,
          // 草稿里存的就是十进制文本，这里只做合法性校验。
          qty: parseQty(line.qty, true) === null ? "0" : String(line.qty).trim(),
          price_cents: Math.round(Number(line.price || 0) * 100),
        });
      }
      if (!linesTable.querySelector("tbody tr")) addLine();
      formDirty = true;
    }

    async function restoreDraftIfAny() {
      const draft = readDraft(docType);
      if (!draft) return;
      if (!(Number(draft.savedAt) > Number(draft.enteredAt || 0))) {
        clearDraft(docType);
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
        "发现「" + config.title + "」未提交的本地草稿（保存于 " + stamp + "），是否恢复？",
        { okText: "恢复", cancelText: "放弃" }
      );
      if (!restore) {
        clearDraft(docType);
        return;
      }
      applyDraft(draft);
      showNotice("已恢复本地草稿，可继续填写", "ok");
    }

    function collectLines() {
      const lines = [];
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        // 接口数量是十进制字符串；parseQty 只用于校验（它返回 ×10000 的定点整数）。
        const qtyInput = row.querySelector('[data-role="qty"]').value.trim();
        const qty = parseQty(qtyInput, docType === "STK_CNT");
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        if (!itemId || qty === null) {
          showNotice("请检查明细行的商品与数量", "error");
          return null;
        }
        lines.push({ item_id: itemId, qty: qtyInput, price_cents: price });
      }
      if (!lines.length) {
        showNotice("请至少添加一行明细", "error");
        return null;
      }
      return lines;
    }

    async function save(draft) {
      const lines = collectLines();
      if (!lines) return;
      const option = customerSelect.selectedOptions[0];
      const body = {
        doc_type: docType,
        warehouse_id: Number(warehouseSelect.value),
        doc_date: dateInput.value,
        partner_id: Number(customerSelect.value || 0),
        partner: option && option.value ? option.dataset.name || "" : "",
        salesperson_id: salespersonSelect ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines,
      };
      try {
        if (editingId) {
          const updated = await api("PUT", "/api/documents/" + editingId, {
            ...body,
            version: editingVersion,
          });
          editingVersion = updated ? updated.version ?? editingVersion : editingVersion;
          if (!draft) {
            await api("POST", "/api/documents/" + editingId + "/approve", {
              version: editingVersion,
            });
          }
          showNotice(draft ? "订单草稿已保存" : "订单已确认", "ok");
        } else {
          const result = await api("POST", "/api/documents", {
            ...body,
            save_as_draft: draft,
            request_id: requestId,
          });
          showNotice((draft ? "已存为草稿：" : "订单已确认：") + result.doc_no, "ok");
        }
        resetForm();
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function startEdit(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        editingId = doc.id;
        editingVersion = detail.version ?? -1;
        warehouseSelect.value = String(detail.warehouse_id);
        dateInput.value = detail.doc_date;
        customerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
        if (salespersonSelect) salespersonSelect.value = String(detail.salesperson_id || 0);
        remarkInput.value = detail.remark || "";
        linesTable.innerHTML = orderLinesHead();
        for (const line of detail.lines) addLine(line);
        formDirty = false;
        mount.querySelector('[data-role="cancel-edit"]').hidden = false;
        form.scrollIntoView({ behavior: "smooth", block: "start" });
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function openConvert(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        convertingId = doc.id;
        // 幂等键随弹窗会话生成一次：提交按钮双击/重试都复用同一个 request_id。
        convertRequestId = newRequestId();
        convertTitle.textContent =
          detail.doc_no + " · " + (detail.partner || "未指定" + config.partnerLabel);
        convertWarehouse.value = String(detail.warehouse_id);
        convertDate.value = new Date().toISOString().slice(0, 10);
        convertLines.innerHTML = orderConvertLinesHtml(detail.lines, itemLabelById);
        convertCard.hidden = false;
        convertCard.scrollIntoView({ behavior: "smooth", block: "start" });
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function submitConvert() {
      const lines = [];
      for (const row of convertLines.querySelectorAll("tbody tr")) {
        const input = row.querySelector('[data-role="convert-qty"]');
        if (input.disabled) continue;
        const qty = parseQty(input.value, true);
        if (qty === null) {
          showNotice("数量格式不正确，最多 4 位小数", "error");
          return;
        }
        if (qty === "0") continue;
        const priceInput = row.querySelector('[data-role="convert-price"]');
        const priceCents = Math.round(Number(priceInput.value || 0) * 100);
        if (!Number.isFinite(priceCents) || priceCents < 0) {
          showNotice("单价必须是不小于 0 的数字", "error");
          return;
        }
        const entry = { order_line_id: Number(row.dataset.lineId), qty };
        // 价格未改动时不传，交给后端沿用订单价。
        if (priceCents !== Number(row.dataset.priceCents)) entry.price_cents = priceCents;
        lines.push(entry);
      }
      if (!lines.length) {
        showNotice("请填写要下推的数量", "error");
        return;
      }
      const submit = async (confirmCredit) =>
        api("POST", "/api/documents/" + convertingId + "/convert", {
          warehouse_id: Number(convertWarehouse.value || 0),
          doc_date: convertDate.value,
          request_id: convertRequestId,
          confirm_credit: confirmCredit,
          lines,
        });
      const finish = async (result) => {
        showNotice(config.convertNotice + result.doc_no, "ok");
        convertCard.hidden = true;
        convertingId = 0;
        await reload();
      };
      try {
        await finish(await submit(false));
      } catch (error) {
        if (error.code !== "CREDIT_LIMIT_WARNING") {
          showNotice(error.message, "error");
          return;
        }
        // 下推超额度：与手工审核同口径，二次确认后带 confirm_credit 重试；
        // 首次被拦截未生成单据，重试复用同一 request_id 属同一下推动作。
        const ok = await askConfirm(error.message + "。仍要继续下推吗？", {
          okText: "继续下推",
          cancelText: "取消",
        });
        if (!ok) return;
        try {
          await finish(await submit(true));
        } catch (retryError) {
          showNotice(retryError.message, "error");
        }
      }
    }

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    // 订单表单只有改过才写本地草稿。
    form.addEventListener("input", () => {
      formDirty = true;
    });
    form.addEventListener("change", () => {
      formDirty = true;
    });
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", resetForm);
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => save(true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => save(false));
    mount.querySelector('[data-role="convert-submit"]').addEventListener("click", submitConvert);
    mount.querySelector('[data-role="convert-cancel"]').addEventListener("click", () => {
      convertCard.hidden = true;
      convertingId = 0;
    });

    documentsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      try {
        if (button.dataset.role === "print") {
          await printTarget(
            "/api/print/render?target_type=DOC&target_key=" +
              docType +
              "&doc_id=" +
              id
          );
          return;
        }
        if (button.dataset.role === "pdf") {
          const doc = documentList.rowById(id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "approve") {
          if (!(await askConfirm(`确认订单后不可再改单，只能${config.convertTitle}或关闭。继续？`))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("订单已确认", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documentList.rowById(id));
          return;
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张订单草稿？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (editingId === id) resetForm();
          showNotice("订单草稿已删除", "ok");
        } else if (button.dataset.role === "convert") {
          await openConvert(documentList.rowById(id));
          return;
        } else if (button.dataset.role === "close") {
          if (!(await askConfirm(`关闭后订单不能再${config.convertTitle}，确认？`))) return;
          await api("POST", "/api/documents/" + id + "/close");
          showNotice("订单已关闭", "ok");
        }
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    await reload();
    startDraftTimer(saveDraftNow);
    await restoreDraftIfAny();
  }

  // ---------- 库存页面 ----------
