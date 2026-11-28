/* app/order_convert.js：订单转出库卡片：打开下推界面、收集本次数量并提交。
 * 由 order.js 拆出；页面状态与 DOM 引用统一通过 ctx 传入。
 */
import { itemOptionText, parseQty } from "./core_format.js";
import { api, newRequestId } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { showNotice } from "./core_state.js";
import { orderConvertLinesHtml } from "./order_view.js";

  function itemLabelById(ctx, itemId) {
    const item = ctx.items.find((row) => row.id === itemId);
    return item ? itemOptionText(item) : String(itemId);
  }

  export async function openOrderConvert(ctx, doc) {
    try {
      const detail = await api("GET", "/api/documents/" + doc.id);
      ctx.convertingId = doc.id;
      // 幂等键随弹窗会话生成一次：提交按钮双击/重试都复用同一个 request_id。
      ctx.convertRequestId = newRequestId();
      ctx.convertTitle.textContent =
        detail.doc_no + " · " + (detail.partner || "未指定" + ctx.config.partnerLabel);
      ctx.convertWarehouse.value = String(detail.warehouse_id);
      ctx.convertDate.value = new Date().toISOString().slice(0, 10);
      ctx.convertLines.innerHTML = orderConvertLinesHtml(detail.lines, (itemId) =>
        itemLabelById(ctx, itemId)
      );
      ctx.convertCard.hidden = false;
      ctx.convertCard.scrollIntoView({ behavior: "smooth", block: "start" });
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  export async function submitOrderConvert(ctx) {
    const lines = [];
    for (const row of ctx.convertLines.querySelectorAll("tbody tr")) {
      const input = row.querySelector('[data-role="convert-qty"]');
      if (input.disabled) continue;
      // 接口数量是十进制字符串；parseQty 只用于校验与判断是否为 0。
      const qtyInput = input.value.trim();
      const qty = parseQty(qtyInput, true);
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
      const entry = { order_line_id: Number(row.dataset.lineId), qty: qtyInput };
      // 价格未改动时不传，交给后端沿用订单价。
      if (priceCents !== Number(row.dataset.priceCents)) entry.price_cents = priceCents;
      lines.push(entry);
    }
    if (!lines.length) {
      showNotice("请填写要下推的数量", "error");
      return;
    }
    const submit = async (confirmCredit) =>
      api("POST", "/api/documents/" + ctx.convertingId + "/convert", {
        warehouse_id: Number(ctx.convertWarehouse.value || 0),
        doc_date: ctx.convertDate.value,
        request_id: ctx.convertRequestId,
        confirm_credit: confirmCredit,
        lines,
      });
    const finish = async (result) => {
      showNotice(ctx.config.convertNotice + result.doc_no, "ok");
      ctx.convertCard.hidden = true;
      ctx.convertingId = 0;
      await ctx.reload();
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
