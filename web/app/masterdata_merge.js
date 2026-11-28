/* app/masterdata_merge.js：客户/供应商档案合并面板：候选预览、执行合并与撤销。
 * 由 masterdata.js 拆出；页面状态通过 ctx 传入。
 */
import { api, invalidateMergedTraders } from "./core_api.js";
import { askConfirm } from "./core_dialog.js";
import { showNotice } from "./core_state.js";
import { mergeLogsHtml, mergePanelHtml } from "./masterdata_view.js";

  export function bindMergePanel(ctx) {
    const def = ctx.def;
    const mergeCard = ctx.mount.querySelector('[data-role="merge-card"]');
    const mergeBody = ctx.mount.querySelector('[data-role="merge-body"]');
    const mergeHint = ctx.mount.querySelector('[data-role="merge-hint"]');
    const mergeLog = ctx.mount.querySelector('[data-role="merge-log"]');
    let mergePreview = null;
    let mergePartners = [];
    let mergeLogs = [];

    function renderMergeLogs() {
      if (!mergeLog) return;
      mergeLog.innerHTML = mergeLogsHtml(mergeLogs);
    }

    async function openMergePanel() {
      if (!mergeCard) return;
      const role = def.key === "customer" ? "customer" : "supplier";
      mergeCard.hidden = false;
      mergeHint.textContent = "正在读取候选与合并记录…";
      try {
        const [partners, preview, logs] = await Promise.all([
          api("GET", "/api/partners?role=" + role),
          api("GET", "/api/partners/merge-preview"),
          api("GET", "/api/partners/merge-log?limit=10"),
        ]);
        mergePreview = preview;
        mergeLogs = logs;
        mergePartners = partners.filter((row) => !row.merged_into && row.status);
        mergeHint.textContent =
          `候选组 ${preview.groups} 组（高置信 ${preview.high}、中高 ${preview.medium}、` +
          `仅名称 ${preview.name_only}）。仅名称相同属低置信，不会自动合并，请逐组核对。`;
        mergeBody.innerHTML = mergePanelHtml(mergePartners, def.key);
        renderMergeLogs();
      } catch (error) {
        mergeHint.textContent = "读取失败：" + error.message;
      }
    }

    if (mergeCard) {
      mergeCard.addEventListener("click", async (event) => {
        const button = event.target.closest("button");
        if (!button) return;
        if (button.dataset.role === "merge-close") {
          mergeCard.hidden = true;
          return;
        }
        if (button.dataset.role === "merge-run") {
          const sources = Array.from(
            mergeBody.querySelectorAll("input[data-merge-source]:checked")
          )
            .map((input) => Number(input.value))
            .filter(Boolean);
          const targetInput = mergeBody.querySelector(
            `input[name="merge-target-${def.key}"]:checked`
          );
          const target = targetInput ? Number(targetInput.value) : 0;
          if (!target) {
            showNotice("请选择保留档案", "error");
            return;
          }
          const selected = sources.filter((id) => id !== target);
          if (!selected.length) {
            showNotice("请至少勾选一条要并入的档案，且不能只选保留档案", "error");
            return;
          }
          const byId = new Map(mergePartners.map((row) => [row.id, row]));
          const names = selected.map((id) => (byId.get(id) || {}).name || "#" + id);
          const targetName = (byId.get(target) || {}).name || "#" + target;
          const candidates = (mergePreview && mergePreview.candidates) || [];
          const matched = candidates.filter((group) =>
            (group.rows || []).some((row) => names.includes(row.name))
          );
          const matchText = matched.length
            ? matched.map((group) => group.match + "：" + group.match_value).join("；")
            : "未命中自动候选，按人工选择合并";
          const ok = await askConfirm(
            `将把以下 ${selected.length} 条档案合并到「${targetName}」：\n` +
              names.join("、") +
              `\n\n预览结果：${matchText}\n\n` +
              "合并后这些档案不再出现在列表与下拉，历史台账、账龄与对账单归到保留档案。确认继续？",
            { okText: "确认合并" }
          );
          if (!ok) return;
          try {
            const result = await api("POST", "/api/partners/merge", {
              target_id: target,
              source_ids: selected,
            });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice(`已合并 ${selected.length} 条到「${result.target_name || targetName}」`);
            await ctx.reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
          return;
        }
        if (button.dataset.role === "undo") {
          const logId = Number(button.dataset.log);
          const ok = await askConfirm(
            `撤销合并到「${button.dataset.name || ""}」？\n` +
              "仅在合并后没有新业务写入被合并档案时才允许撤销。",
            { okText: "确认撤销" }
          );
          if (!ok) return;
          try {
            await api("POST", "/api/partners/undo", { log_id: logId });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice("已撤销合并");
            await ctx.reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
        }
      });
      const openButton = ctx.mount.querySelector('[data-role="merge-open"]');
      if (openButton) openButton.addEventListener("click", openMergePanel);
    }
  }
