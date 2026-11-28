/* app/finance.js：收付款等财务页。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askAlert, askConfirm, askPrompt, escapeHtml, money, showNotice } from "./core.js";

  export async function financePage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>收付款</h2>
          <p class="page-sub">可全额核销、部分核销或作为预收/预付；撤销通过负数记录回冲，不删除历史</p>
          <p class="page-sub">统计范围：仅统计 v7 迁移之后审核的单据，升级前已审核单据不计入往来与账龄</p>
        </div>
        <div class="toolbar">
          <select data-role="direction">
            <option value="1">收款</option>
            <option value="-1">付款</option>
          </select>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card">
        <h3>新建收付款单</h3>
        <div class="grid-form">
          <label class="field"><span>往来单位</span><select data-role="partner"></select></label>
          <label class="field"><span>资金账户</span><select data-role="account"></select></label>
          <label class="field"><span>结算方式</span>
            <select data-role="method">
              <option value="CASH">现金</option>
              <option value="BANK">银行</option>
              <option value="WECHAT">微信</option>
              <option value="ALIPAY">支付宝</option>
              <option value="OTHER">其他</option>
            </select>
          </label>
          <label class="field"><span>金额（元）</span><input type="text" inputmode="decimal" data-role="amount" value="0.00" /></label>
          <label class="field"><span>日期</span><input type="date" data-role="date" value="${today}" /></label>
          <label class="field"><span>经办人</span><input type="text" data-role="operator" placeholder="默认当前用户" /></label>
          <label class="field"><span>备注</span><input type="text" data-role="note" /></label>
        </div>
        <p class="page-sub">分摊明细可留空（预收/预付）或只勾选部分单据；分摊合计不能超过单据金额</p>
        <table class="table" data-role="allocations"></table>
        <div class="toolbar">
          <button class="btn" data-role="submit">保存收付款单</button>
        </div>
      </div>
      <div class="card">
        <h3>未核销单据</h3>
        <table class="table" data-role="rows"></table>
      </div>
      <div class="card">
        <h3>收付款记录</h3>
        <table class="table" data-role="settlements"></table>
      </div>`;

    const directionSelect = mount.querySelector('[data-role="direction"]');
    const partnerSelect = mount.querySelector('[data-role="partner"]');
    const accountSelect = mount.querySelector('[data-role="account"]');
    const methodSelect = mount.querySelector('[data-role="method"]');
    const amountInput = mount.querySelector('[data-role="amount"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const operatorInput = mount.querySelector('[data-role="operator"]');
    const noteInput = mount.querySelector('[data-role="note"]');
    const rowsTable = mount.querySelector('[data-role="rows"]');
    const settlementsTable = mount.querySelector('[data-role="settlements"]');
    const allocationsTable = mount.querySelector('[data-role="allocations"]');
    let entries = [];
    let accounts = [];
    let partners = [];
    let allocations = [];

    const methodText = (value) =>
      ({ CASH: "现金", BANK: "银行", WECHAT: "微信", ALIPAY: "支付宝", OTHER: "其他" })[value] ||
      (value ? value : "未记录");
    const accountName = (id) => {
      const found = accounts.find((row) => String(row.id) === String(id));
      return found ? found.name : "未指定账户";
    };

    function renderAllocations() {
      if (!allocations.length) {
        allocationsTable.innerHTML =
          '<tbody><tr><td class="empty">该往来单位暂无未核销单据，可直接保存为预收/预付</td></tr></tbody>';
        return;
      }
      allocationsTable.innerHTML = `<thead><tr><th>选择</th><th>单号</th><th>日期</th>
          <th>未核销（元）</th><th>本次分摊（元）</th></tr></thead>
        <tbody>${allocations
          .map(
            (row, index) => `<tr>
              <td><input type="checkbox" data-alloc="${index}" ${row.selected ? "checked" : ""} /></td>
              <td>${escapeHtml(row.doc_no)}</td>
              <td>${escapeHtml(row.biz_date)}</td>
              <td>${money(row.outstanding_cents)}</td>
              <td><input type="text" inputmode="decimal" data-alloc-amount="${index}"
                value="${row.amount}" ${row.selected ? "" : "disabled"} /></td>
            </tr>`
          )
          .join("")}</tbody>`;
      allocationsTable.querySelectorAll("[data-alloc]").forEach((box) => {
        box.addEventListener("change", () => {
          const index = Number(box.dataset.alloc);
          allocations[index].selected = box.checked;
          renderAllocations();
        });
      });
      allocationsTable.querySelectorAll("[data-alloc-amount]").forEach((input) => {
        input.addEventListener("input", () => {
          allocations[Number(input.dataset.allocAmount)].amount = input.value;
        });
      });
    }

    async function loadPartnerOptions() {
      const isReceipt = directionSelect.value === "1";
      const list = await api("GET", isReceipt ? "/api/customers" : "/api/suppliers");
      partners = list;
      const previous = partnerSelect.value;
      const prefix = isReceipt ? "C:" : "S:";
      partnerSelect.innerHTML =
        '<option value="">请选择</option>' +
        list
          .filter((row) => row.status)
          .map(
            (row) =>
              `<option value="${prefix}${row.id}">${escapeHtml(row.code)} ${escapeHtml(
                row.name
              )}</option>`
          )
          .join("");
      if (previous && list.some((row) => prefix + row.id === previous)) {
        partnerSelect.value = previous;
      }
    }

    async function loadAccounts() {
      accounts = await api("GET", "/api/fund-accounts");
      const active = accounts.filter((row) => row.status);
      const previous = accountSelect.value;
      accountSelect.innerHTML = active.length
        ? active
            .map(
              (row) =>
                `<option value="${row.id}">${escapeHtml(row.code)} ${escapeHtml(row.name)}</option>`
            )
            .join("")
        : '<option value="">请先在「资金账户」建档</option>';
      if (previous && active.some((row) => String(row.id) === previous)) {
        accountSelect.value = previous;
      }
    }

    async function loadEntries() {
      const partnerKey = partnerSelect.value;
      if (!partnerKey) {
        entries = [];
        allocations = [];
      } else {
        entries = await api(
          "GET",
          "/api/ar-ap?direction=" +
            directionSelect.value +
            "&partner_key=" +
            encodeURIComponent(partnerKey)
        );
        allocations = entries
          .filter((row) => row.outstanding_cents !== 0 && row.status !== "VOIDED")
          .map((row) => ({
            ...row,
            selected: false,
            amount: (Math.abs(row.outstanding_cents) / 100).toFixed(2),
          }));
      }
      const usable = entries.filter((row) => row.status !== "VOIDED");
      rowsTable.innerHTML = !usable.length
        ? '<tbody><tr><td class="empty">暂无未核销单据</td></tr></tbody>'
        : `<thead><tr><th>单号</th><th>类型</th><th>往来单位</th><th>日期</th>
             <th>金额（元）</th><th>已核销</th><th>未核销</th><th>状态</th><th></th></tr></thead>
           <tbody>${usable
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.doc_type)}</td>
                 <td>${escapeHtml(row.partner_name || "未指定")}</td>
                 <td>${escapeHtml(row.biz_date)}</td>
                 <td>${money(row.amount_cents)}</td>
                 <td>${money(row.settled_cents)}</td>
                 <td>${money(row.outstanding_cents)}</td>
                 <td>${escapeHtml(row.status)}</td>
                 <td>${
                   row.outstanding_cents !== 0
                     ? `<button class="btn small" data-settle="${row.id}">核销</button>`
                     : ""
                 }</td>
               </tr>`
             )
             .join("")}</tbody>`;
      renderAllocations();
    }

    async function reload() {
      await Promise.all([loadPartnerOptions(), loadAccounts()]);
      const settlements = await api(
        "GET",
        "/api/settlements?direction=" + directionSelect.value
      );
      await loadEntries();
      settlementsTable.innerHTML = !settlements.length
        ? '<tbody><tr><td class="empty">暂无收付款记录</td></tr></tbody>'
        : `<thead><tr><th>单号</th><th>类型</th><th>往来单位</th><th>账户</th>
             <th>方式</th><th>经办人</th><th>日期</th><th>金额（元）</th><th>备注</th><th></th></tr></thead>
           <tbody>${settlements
             .map(
               (row, index) => `<tr>
                 <td>${escapeHtml(row.settle_no)}</td>
                 <td>${row.direction === 1 ? "收款" : "付款"}</td>
                 <td>${escapeHtml(row.partner_name || "未指定")}</td>
                 <td>${escapeHtml(accountName(row.account_id))}</td>
                 <td>${escapeHtml(methodText(row.method))}</td>
                 <td>${escapeHtml(row.operator || "")}</td>
                 <td>${escapeHtml(row.settled_on)}</td>
                 <td>${money(row.amount_cents)}</td>
                 <td>${escapeHtml(row.note)}</td>
                 <td>${
                   row.reverses_id === 0 && row.amount_cents > 0
                     ? `<button class="btn small" data-reverse="${index}">撤销</button>`
                     : ""
                 }</td>
               </tr>`
             )
             .join("")}</tbody>`;
      settlements.forEach((row, index) => {
        const button = settlementsTable.querySelector(`[data-reverse="${index}"]`);
        if (!button) return;
        button.addEventListener("click", async () => {
          if (!(await askConfirm(`撤销收付款 ${row.settle_no}？`))) return;
          try {
            await api("POST", `/api/settlements/${row.id}/reverse`, {});
            await reload();
          } catch (error) {
            await askAlert(error.message);
          }
        });
      });
    }

    rowsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("[data-settle]");
      if (!button) return;
      const entry = entries.find((row) => String(row.id) === button.dataset.settle);
      if (!entry) return;
      const defaultAmount = (Math.abs(entry.outstanding_cents) / 100).toFixed(2);
      const input = await askPrompt("本次核销金额（元）", defaultAmount);
      if (input === null) return;
      const value = Number(input);
      if (!Number.isFinite(value) || value <= 0) {
        await askAlert("请输入大于 0 的金额");
        return;
      }
      const cents = Math.round(value * 100) * Math.sign(entry.outstanding_cents);
      try {
        await api("POST", "/api/settlements", {
          direction: Number(directionSelect.value),
          partner_key: entry.partner_key,
          amount_cents: Math.abs(cents),
          account_id: Number(accountSelect.value || 0),
          method: methodSelect.value,
          allocations: [{ entry_id: entry.id, amount_cents: cents }],
          note: "",
        });
        await reload();
      } catch (error) {
        await askAlert(error.message);
      }
    });

    partnerSelect.addEventListener("change", loadEntries);
    directionSelect.addEventListener("change", reload);
    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    mount.querySelector('[data-role="submit"]').addEventListener("click", async () => {
      const partnerKey = partnerSelect.value;
      if (!partnerKey) {
        await askAlert("请选择往来单位");
        return;
      }
      if (!accountSelect.value) {
        await askAlert("请先在「资金账户」建档并启用");
        return;
      }
      const amount = Number(amountInput.value);
      if (!Number.isFinite(amount) || amount <= 0) {
        await askAlert("请输入大于 0 的金额");
        return;
      }
      const amountCents = Math.round(amount * 100);
      const payload = [];
      let allocated = 0;
      for (const row of allocations) {
        if (!row.selected) continue;
        const lineAmount = Number(row.amount);
        if (!Number.isFinite(lineAmount) || lineAmount <= 0) {
          await askAlert("分摊金额必须大于 0");
          return;
        }
        const cents = Math.round(lineAmount * 100) * Math.sign(row.outstanding_cents);
        allocated += Math.abs(cents);
        payload.push({ entry_id: row.id, amount_cents: cents });
      }
      if (allocated > amountCents) {
        await askAlert("分摊合计不能超过收付款金额");
        return;
      }
      const partner = partners.find((row) => String(row.id) === partnerKey.slice(2));
      try {
        await api("POST", "/api/settlements", {
          direction: Number(directionSelect.value),
          partner_key: partnerKey,
          partner_name: partner ? partner.name : "",
          amount_cents: amountCents,
          account_id: Number(accountSelect.value),
          method: methodSelect.value,
          operator: operatorInput.value.trim(),
          settled_on: dateInput.value,
          note: noteInput.value.trim(),
          allocations: payload,
        });
        amountInput.value = "0.00";
        noteInput.value = "";
        await reload();
        showNotice("收付款单已保存", "ok");
      } catch (error) {
        await askAlert(error.message);
      }
    });
    await reload();
  }

  // ---------- 报表页面 ----------
