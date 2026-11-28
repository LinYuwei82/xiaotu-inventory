/* app/document.js：业务单据列表与单据详情编辑页。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, can, clearDraft, downloadCsv, downloadXlsx, escapeHtml, itemOptionText, money, newRequestId, parseQty, qtyAmountCents, qtyAmountText, qtyText, readDraft, setUnsavedGuard, showNotice, startDraftTimer, taxForAmount, writeDraft } from "./core.js";
import { documentLineRowHtml, documentLinesHead, documentListColumns, documentPageHtml } from "./document_view.js";
import { createDocumentList } from "./documents.js";
import { exportPdfTarget, printTarget } from "./print.js";

  export async function documentPage(mount, config) {
    const docType = config.docType;
    const costAdjust = !!config.costAdjust;
    // 只有其他出入库有业务类型；入库可用期初，出库可用领用与报损，赠品两方向通用。
    const otherDoc = docType === "OTH_IN" || docType === "OTH_OUT";
    const salesDoc = docType === "SAL_OUT" || docType === "SAL_RET";
    const flags = { costAdjust, otherDoc, salesDoc };
    let warehouses = [];
    let items = [];
    let editingId = 0;
    // 乐观锁版本与幂等键：版本随详情/列表带回，request_id 每次新建页生成一次。
    let editingVersion = -1;
    let requestId = newRequestId();
    // 录入页的脏标记：任一字段变动即置脏，保存成功、重置或切换单据后清除。
    let dirty = false;

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
    const documentsTable = documentList.table;

    dateInput.value = new Date().toISOString().slice(0, 10);
    if (costAdjust && !can("cost.adjust")) {
      mount.querySelector('[data-role="submit"]').hidden = true;
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

    function warehouseOptions(selected) {
      return warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}" ${
              String(warehouse.id) === String(selected) ? "selected" : ""
            }>${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      row.innerHTML = documentLineRowHtml(line, costAdjust, itemOptions, warehouseOptions);
      linesTable.querySelector("tbody").appendChild(row);
      if (costAdjust) {
        row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
        if (line) {
          row.querySelector('[data-role="adjust"]').value = (line.adjust_cents / 100).toFixed(2);
          row.querySelector('[data-role="reason"]').value = line.adjust_reason || "";
        }
        return;
      }
      row.querySelector('[data-role="remove"]').addEventListener("click", () => {
        row.remove();
        recalcTotals();
      });
      const recalc = () => {
        const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const amount = qtyAmountCents(qty, price);
        const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
        const includesTax = row.querySelector('[data-role="tax-included"]').checked;
        row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
        const lineTax = taxForAmount(amount, rateBp, includesTax).tax;
        row.querySelector('[data-role="tax"]').value = money(lineTax);
        row.querySelector('[data-role="with-tax"]').value = money(
          includesTax ? amount : amount + lineTax
        );
        recalcTotals();
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      row.querySelector('[data-role="tax-rate"]').addEventListener("input", recalc);
      row.querySelector('[data-role="tax-included"]').addEventListener("change", recalc);
      const itemSelect = row.querySelector('[data-role="item"]');
      const unitHint = row.querySelector('[data-role="unit"]');
      const updateUnit = () => {
        const item = items.find((candidate) => String(candidate.id) === String(itemSelect.value));
        unitHint.textContent = item && item.unit ? item.unit : "";
        // 新行跟随商品默认税率；编辑历史单据时保留落库税率。
        if (!line) {
          const bp = item && item.tax_rate_bp ? item.tax_rate_bp : 0;
          row.querySelector('[data-role="tax-rate"]').value = (bp / 100)
            .toFixed(2)
            .replace(/\.?0+$/, "");
        }
        recalc();
      };
      itemSelect.addEventListener("change", updateUnit);
      updateUnit();
      if (line) {
        row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        row.querySelector('[data-role="tax-rate"]').value = ((line.tax_rate_bp || 0) / 100)
          .toFixed(2)
          .replace(/\.?0+$/, "");
        row.querySelector('[data-role="tax-included"]').checked = !!line.price_includes_tax;
        recalc();
      }
    }

    function recalcTotals() {
      if (costAdjust) return;
      let qty = 0;
      let amount = 0;
      let tax = 0;
      let withTax = 0;
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const qtyScaled = parseQty(row.querySelector('[data-role="qty"]').value, true) || 0;
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const lineAmount = qtyAmountCents(qtyScaled, price);
        const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
        const includesTax = row.querySelector('[data-role="tax-included"]').checked;
        const lineTax = taxForAmount(lineAmount, rateBp, includesTax).tax;
        qty += Number(qtyScaled);
        amount += lineAmount;
        tax += lineTax;
        withTax += includesTax ? lineAmount : lineAmount + lineTax;
      }
      const totalsEl = mount.querySelector('[data-role="doc-totals"]');
      if (totalsEl) {
        totalsEl.textContent =
          "合计数量 " + qtyText(qty) + "，金额 " + money(amount) +
          "，税额 " + money(tax) + "，价税合计 " + money(withTax);
      }
    }

    function resetLines() {
      linesTable.innerHTML = documentLinesHead(costAdjust);
      addLine();
      recalcTotals();
    }

    async function reload() {
      const [warehouseList, itemList, partnerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        config.partnerApi ? api("GET", config.partnerApi) : Promise.resolve([]),
        salesDoc ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      warehouses = warehouseList;
      items = itemList;
      if (config.partnerApi) {
        partnerSelect.innerHTML =
          '<option value="">不指定</option>' +
          partnerList
            .filter((partner) => partner.status)
            .map(
              (partner) =>
                `<option value="${partner.id}" data-name="${escapeHtml(partner.name)}">${escapeHtml(
                  partner.code + " " + partner.name
                )}</option>`
            )
            .join("");
      }
      if (salesDoc) {
        // 停用/离职职员仍列在历史单据里，这里保留可选项并标注，不隐藏。
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
      const warehouseOptions = warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}">${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
      warehouseSelect.innerHTML = warehouseOptions;
      if (config.twoWarehouse) {
        targetWarehouseSelect.innerHTML = warehouseOptions;
        if (warehouses.filter((warehouse) => warehouse.status).length > 1) {
          targetWarehouseSelect.selectedIndex = 1;
        }
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      editingVersion = -1;
      // 提交成功/取消后作废幂等键，下一次新建重新生成。
      requestId = newRequestId();
      dirty = false;
      // 提交成功、取消编辑都算显式结束，本地草稿一并清掉。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      form.querySelector('[data-role="partner"]').value = "";
      if (partnerSelect) partnerSelect.value = "";
      if (otherDoc) bizCategorySelect.value = "OTHER";
      if (salesDoc) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
    }

    // 长表单本地草稿：只存业务字段，按单据类型分键，每 30 秒写一次。
    const draftEnteredAt = Date.now();

    function draftFormData() {
      const data = {
        warehouse_id: Number(warehouseSelect.value || 0),
        target_warehouse_id: config.twoWarehouse
          ? Number(targetWarehouseSelect.value || 0)
          : 0,
        doc_date: dateInput.value,
        partner: form.querySelector('[data-role="partner"]').value.trim(),
        partner_id: partnerSelect && !partnerSelect.hidden ? Number(partnerSelect.value || 0) : 0,
        biz_category: otherDoc ? bizCategorySelect.value : "",
        salesperson_id: salesDoc ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines: [],
      };
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        if (!itemId) continue;
        if (costAdjust) {
          data.lines.push({
            item_id: itemId,
            warehouse_id: Number(row.querySelector('[data-role="line-warehouse"]').value || 0),
            adjust: row.querySelector('[data-role="adjust"]').value.trim(),
            reason: row.querySelector('[data-role="reason"]').value.trim(),
          });
          continue;
        }
        data.lines.push({
          item_id: itemId,
          qty: row.querySelector('[data-role="qty"]').value.trim(),
          price: row.querySelector('[data-role="price"]').value,
          tax_rate: row.querySelector('[data-role="tax-rate"]').value,
          tax_included: row.querySelector('[data-role="tax-included"]').checked,
        });
      }
      return data;
    }

    function saveDraftNow() {
      // 编辑服务端已有草稿时不写本地草稿，避免和版本校验互相干扰。
      if (editingId || !dirty) return false;
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
      // 恢复出来的是新的提交意图，重新生成幂等键。
      requestId = newRequestId();
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      if (data.warehouse_id) warehouseSelect.value = String(data.warehouse_id);
      if (config.twoWarehouse && data.target_warehouse_id) {
        targetWarehouseSelect.value = String(data.target_warehouse_id);
      }
      if (data.doc_date) dateInput.value = data.doc_date;
      form.querySelector('[data-role="partner"]').value = data.partner || "";
      if (partnerSelect && !partnerSelect.hidden) {
        partnerSelect.value = data.partner_id ? String(data.partner_id) : "";
      }
      if (otherDoc) bizCategorySelect.value = data.biz_category || "OTHER";
      if (salesDoc) salespersonSelect.value = String(data.salesperson_id || 0);
      remarkInput.value = data.remark || "";
      linesTable.innerHTML = documentLinesHead(costAdjust);
      for (const line of data.lines || []) {
        addLine(
          costAdjust
            ? {
                item_id: line.item_id,
                qty: "0",
                price_cents: 0,
                warehouse_id: line.warehouse_id,
                adjust_cents: Math.round(Number(line.adjust || 0) * 100),
                adjust_reason: line.reason || "",
              }
            : {
                item_id: line.item_id,
                qty: parseQty(line.qty, true) || "0",
                price_cents: Math.round(Number(line.price || 0) * 100),
                tax_rate_bp: Math.round(Number(line.tax_rate || 0) * 100),
                price_includes_tax: Boolean(line.tax_included),
              }
        );
      }
      if (!linesTable.querySelector("tbody tr")) addLine();
      if (!costAdjust) recalcTotals();
      dirty = true;
    }

    async function restoreDraftIfAny() {
      const draft = readDraft(docType);
      if (!draft) return;
      // 只恢复「进入表单后确实改过」的草稿：写入时间晚于该草稿记录的进入时间。
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
        // 放弃即显式丢弃，清掉本地草稿。
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
        if (costAdjust) {
          const warehouseId = Number(row.querySelector('[data-role="line-warehouse"]').value);
          const adjustText = row.querySelector('[data-role="adjust"]').value.trim();
          const adjust = Math.round(Number(adjustText) * 100);
          const reason = row.querySelector('[data-role="reason"]').value.trim();
          if (!itemId || !warehouseId || !adjustText || !Number.isFinite(adjust) || adjust === 0) {
            showNotice("请检查明细行的商品、仓库与调整金额", "error");
            return null;
          }
          if (!reason) {
            showNotice("请填写调整原因", "error");
            return null;
          }
          lines.push({
            item_id: itemId,
            qty: "0",
            price_cents: 0,
            warehouse_id: warehouseId,
            adjust_cents: adjust,
            adjust_reason: reason,
          });
          continue;
        }
        const qty = parseQty(
          row.querySelector('[data-role="qty"]').value,
          docType === "STK_CNT"
        );
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const rateBp = Math.round(
          Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100
        );
        if (!itemId || qty === null) {
          showNotice("请检查明细行的商品与数量", "error");
          return null;
        }
        lines.push({
          item_id: itemId,
          qty,
          price_cents: price,
          tax_rate_bp: rateBp,
          price_includes_tax: row.querySelector('[data-role="tax-included"]').checked,
        });
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
      const partnerInput = form.querySelector('[data-role="partner"]');
      const usePartnerSelect = partnerSelect && !partnerSelect.hidden;
      const partnerOption = usePartnerSelect ? partnerSelect.selectedOptions[0] : null;
      const body = {
        doc_type: docType,
        warehouse_id: costAdjust ? 0 : Number(warehouseSelect.value),
        target_warehouse_id: config.twoWarehouse ? Number(targetWarehouseSelect.value) : 0,
        doc_date: dateInput.value,
        partner_id: usePartnerSelect ? Number(partnerSelect.value || 0) : 0,
        partner: usePartnerSelect
          ? partnerOption && partnerOption.value
            ? partnerOption.dataset.name || ""
            : ""
          : partnerInput.value.trim(),
        biz_category: otherDoc ? bizCategorySelect.value : "",
        salesperson_id: salesDoc ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines,
      };
      const submit = async (confirmCredit) => {
        if (editingId) {
          // 带上读取时的版本；被他人改过会返回 VERSION_CONFLICT。
          const updated = await api("PUT", "/api/documents/" + editingId, {
            ...body,
            confirm_credit: confirmCredit,
            version: editingVersion,
          });
          editingVersion = updated ? updated.version ?? editingVersion : editingVersion;
          if (!draft) {
            await api(
              "POST",
              "/api/documents/" + editingId + "/approve" +
                (confirmCredit ? "?confirm_credit=1" : ""),
              { version: editingVersion }
            );
          }
          return null;
        }
        return api("POST", "/api/documents", {
          ...body,
          save_as_draft: draft,
          confirm_credit: confirmCredit,
          request_id: requestId,
        });
      };
      const finish = async (result) => {
        showNotice(
          result
            ? (draft ? "已存为草稿：" : "已保存并审核：") + result.doc_no
            : draft
              ? "草稿已保存"
              : "已保存并审核",
          "ok"
        );
        resetForm();
        await reload();
      };
      try {
        await finish(await submit(false));
      } catch (error) {
        if (error.code !== "CREDIT_LIMIT_WARNING") {
          showNotice(error.message, "error");
          return;
        }
        const ok = await askConfirm(error.message + "。仍要继续审核吗？", {
          okText: "继续出库",
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

    async function startEdit(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        editingId = doc.id;
        editingVersion = detail.version ?? -1;
        warehouseSelect.value = String(detail.warehouse_id);
        dateInput.value = detail.doc_date;
        if (config.twoWarehouse && detail.target_warehouse_id) {
          targetWarehouseSelect.value = String(detail.target_warehouse_id);
        }
        form.querySelector('[data-role="partner"]').value = detail.partner || "";
        if (partnerSelect && !partnerSelect.hidden) {
          partnerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
        }
        if (otherDoc) bizCategorySelect.value = detail.biz_category || "OTHER";
        if (salesDoc) salespersonSelect.value = String(detail.salesperson_id || 0);
        remarkInput.value = detail.remark || "";
        linesTable.innerHTML = documentLinesHead(costAdjust);
        for (const line of detail.lines) addLine(line);
        dirty = false;
        mount.querySelector('[data-role="cancel-edit"]').hidden = false;
        form.scrollIntoView({ behavior: "smooth", block: "start" });
        showNotice("正在编辑草稿 " + detail.doc_no, "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    // 只有真正录过内容才拦：任一输入/选择变动即置脏，离开前确认一次。
    form.addEventListener("input", () => {
      dirty = true;
    });
    form.addEventListener("change", () => {
      dirty = true;
    });
    setUnsavedGuard(() => dirty);

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", () => resetForm());
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => save(true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => save(false));

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
        if (button.dataset.role === "export-csv") {
          await downloadCsv("/api/documents/" + id + "/export?format=csv");
          showNotice("已导出 CSV", "ok");
          return;
        }
        if (button.dataset.role === "export-xlsx") {
          await downloadXlsx("/api/documents/" + id + "/export?format=xlsx");
          showNotice("已导出 Excel", "ok");
          return;
        }
        if (button.dataset.role === "reverse") {
          if (!(await askConfirm("反审核会生成反向库存流水，确认继续？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/reverse",
            doc ? { version: doc.version } : {}
          );
          showNotice("已反审核", "ok");
        } else if (button.dataset.role === "approve") {
          if (!(await askConfirm("审核后才会写库存流水，确认继续？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("已审核", "ok");
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张草稿？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (editingId === id) resetForm();
          showNotice("草稿已删除", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documentList.rowById(id));
          return;
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

  // ---------- 订单页面（销售订单 / 采购订单共用） ----------
