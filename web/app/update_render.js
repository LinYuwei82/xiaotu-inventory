/* app/update_render.js：软件更新卡片渲染（壳内原生更新与浏览器服务端更新两条路径）。
 */
import { api, askConfirm, can, escapeHtml, openExternalUrl, showNotice } from "./core.js";
import { nativeUpdateBridge, nativeUpdatePlatform } from "./update_bridge.js";
import { formatBytes, mandatoryActive, mandatoryReasonText, normalizeUpdateStatus, versionWithBuild, winFailureText } from "./update_normalize.js";

  // 软件更新：状态、下载、安装都在服务端完成；前端只展示结果与发起动作。
  // 不自动安装：下载后仍需管理员点「安装更新」并二次确认。
  // 原生自动更新卡片：由壳内 XiaotuUpdater 提供状态、检查与安装。
  export function renderNativeUpdate(card, nativeBridge) {
    // P3：壳回调里都是引擎原始 status（+平台扩展键），统一在此归一后再渲染。
    const statusPlatform = nativeUpdatePlatform();
    const resultText = (status) => {
      const labels = {
        UPDATE_AVAILABLE: "发现新版本",
        NO_UPDATE: "已是最新版本",
        CHECK_FAILED: "检查失败（网络或清单验签未通过）",
        SKIPPED: "已跳过该版本",
        INCOMPATIBLE_SCHEMA: "版本不兼容，需先升级到中间版本",
        INTERMEDIATE_REQUIRED: "需先安装中间版本",
        OS_UNSUPPORTED: "当前系统版本过低，不能安装该更新",
        INCOMPATIBLE_API: "客户端接口版本不兼容",
        NOT_IN_ROLLOUT: "该版本暂未对当前安装开放（灰度）",
        NOT_CONFIGURED: "未配置更新源",
      };
      const last = status && status.last_result;
      // 防御：后端已明确 update_available=false（pending_version ≤ current_version），
      // 但 last_result 还留着上次的 UPDATE_AVAILABLE——这种安装后残留状态会让
      // "结果文案"一直显示"发现新版本"，必须按当前状态覆盖修正。
      // 2026-11-19 在线更新 1.0.3→1.0.4 后仍显示"发现新版本"即因此。
      if (!status || status.update_available === false) {
        if (last === "UPDATE_AVAILABLE") return labels["NO_UPDATE"];
      }
      return last ? labels[last] || last : "尚未检查";
    };

    // 后台预下载状态文案：下载中显示进度，重试与失败显示原因，完成后显示包名与大小。
    function prefetchText(status) {
      const state = status && status.prefetch_state;
      const bytes = status && status.prefetch_bytes ? formatBytes(status.prefetch_bytes) : "0 B";
      const total = status && status.prefetch_total ? formatBytes(status.prefetch_total) : "";
      if (state === "running") {
        return total
          ? `正在后台下载 ${status.prefetch_version || ""}（${bytes} / ${total}）`
          : `正在后台下载 ${status.prefetch_version || ""}（${bytes}）`;
      }
      if (state === "done") {
        return `已下载并校验：${status.prefetch_file || ""}（${bytes}）`;
      }
      if (state === "failed") {
        return "后台下载失败：可手动下载";
      }
      if (state === "canceled") {
        return "已取消本次后台下载，可手动下载";
      }
      return "—";
    }

    // Windows 点「安装更新」后壳内可能先补下载；期间按钮保持禁用与提示文案。
    let installPreparing = false;
    let pollTimer = null;
    function stopPoll() {
      if (pollTimer) {
        clearTimeout(pollTimer);
        pollTimer = null;
      }
    }
    // 下载进度只在页面停留时轮询，离开关于页后定时器自行结束。
    function schedulePoll(status) {
      stopPoll();
      if (!status || status.prefetch_state !== "running") return;
      pollTimer = setTimeout(() => {
        pollTimer = null;
        if (!document.body.contains(card)) return;
        nativeBridge.xiaotuUpdateStatus.postMessage({});
      }, 1500);
    }

    function render(status) {
      status = status || { update_available: false };
      // Sparkle 安装引擎：下载 / 验签 / 替换 / 重启都在安装流程内完成，
      // 不再提供「后台预下载、下载并校验」这些自研入口。
      const sparkle = status.install_engine === "sparkle";
      const prefetchRunning = status.prefetch_state === "running";
      // Windows 与 macOS 同一套界面：只在真有下载在跑时显示进度行，
      // 不再暴露「后台下载」开关与手动下载入口（安装时壳内自动补下载）。
      const rows = [
        ["检查通道", status.channel || "stable"],
        [
          "上次检查",
          status.last_check_at ? new Date(status.last_check_at * 1000).toLocaleString() : "—",
        ],
        ["检查结果", resultText(status)],
        [
          "待安装版本",
          status.update_available
            ? versionWithBuild(status.pending_version, status.pending_build)
            : "—",
        ],
        !sparkle && prefetchRunning ? ["后台下载", prefetchText(status)] : null,
      ].filter(Boolean);
      // 强制升级：到期后阻塞式提示，不提供“跳过此版本”；截止前仍按普通更新提示。
      const reason = mandatoryReasonText(status.reason);
      const forceNow = mandatoryActive(status);
      const mandatory =
        status.update_available && status.mandatory
          ? `<div class="notice ${forceNow ? "error" : "warn"}" style="margin:10px 0">
               <strong>${forceNow ? "必须升级" : "计划升级"}</strong>：${
                 reason ? escapeHtml(reason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 status.pending_version || "新版本"
               )}。${
                 forceNow
                   ? "此更新已到截止时间，不能跳过。"
                   : status.pending_deadline
                     ? `请在 ${escapeHtml(status.pending_deadline)} 前完成升级，到期后将无法跳过。`
                     : ""
               }
             </div>`
          : "";
      card.innerHTML = `
        <div class="page-head" style="margin-bottom:8px">
          <div>
            <h2>软件更新</h2>
            <p class="page-sub">检查与下载均通过签名校验；安装前会再次确认并关闭程序</p>
          </div>
        </div>
        ${mandatory}
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        <div style="display:flex;gap:16px;flex-wrap:wrap;margin-top:10px">
          <label class="checkbox"><input type="checkbox" data-role="auto-check" ${
            status.auto_check ? "checked" : ""
          } /> 自动检查更新</label>
        </div>
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">
          <button class="btn" type="button" data-role="check">检查更新</button>
          ${
            status.update_available && status.notes_url
              ? `<button class="btn" type="button" data-role="notes">查看更新详情</button>`
              : ""
          }
          <button class="btn primary" type="button" data-role="install" ${
            status.update_available && !installPreparing ? "" : "disabled"
          }>${installPreparing ? "正在准备安装包…" : "安装更新"}</button>
        </div>`;

      const checkButton = card.querySelector('[data-role="check"]');
      checkButton.addEventListener("click", () => {
        checkButton.disabled = true;
        checkButton.textContent = "检查中…";
        nativeBridge.xiaotuUpdateCheckNow.postMessage({});
      });
      const notesButton = card.querySelector('[data-role="notes"]');
      if (notesButton) {
        notesButton.addEventListener("click", () => openExternalUrl(status.notes_url));
      }
      const autoInput = card.querySelector('[data-role="auto-check"]');
      autoInput.addEventListener("change", () => {
        autoInput.disabled = true;
        nativeBridge.xiaotuUpdateOptions.postMessage({ auto_check: autoInput.checked });
      });
      card.querySelector('[data-role="install"]').addEventListener("click", async () => {
        const size = status.pending_size
          ? `约 ${formatBytes(status.pending_size)}`
          : "安装包";
        const downloaded = !sparkle && status.prefetch_state === "done";
        const forceNow = mandatoryActive(status);
        const deadlineNote =
          !forceNow && status.mandatory && status.pending_deadline
            ? `该版本为强制更新，请在 ${status.pending_deadline} 前完成，到期后将无法跳过。`
            : "";
        // Sparkle 负责下载、验签、原子替换并自动重启；不再有「拖拽安装」这一步。
        const installTail = sparkle
          ? `安装时由 Sparkle 下载并校验${size}，完成后自动重启。是否继续？`
          : downloaded
            ? "将使用已下载并校验的安装包，随后关闭程序并完成安装。是否继续？"
            : `安装时会先下载并校验${size}，随后关闭程序并完成安装。是否继续？`;
        const confirmed = await askConfirm(
          forceNow
            ? `此更新已到截止时间（${mandatoryReasonText(status.reason) || "服务端要求"}），不能跳过。` +
                installTail
            : `将安装 ${versionWithBuild(
                status.pending_version,
                status.pending_build
              )}。` + deadlineNote + installTail,
          { okText: "安装", cancelText: "取消" }
        );
        if (!confirmed) return;
        if (!sparkle) {
          installPreparing = true;
          render(status);
        }
        nativeBridge.xiaotuUpdateInstall.postMessage({});
      });
      schedulePoll(status);
    }

    // 状态回调直接传 status 本体；检查回调里是 {status: ...} 包装。
    window.xiaotuUpdateStatusDone = (payload) =>
      render(normalizeUpdateStatus((payload && payload.status) || payload || {}, statusPlatform));
    window.xiaotuUpdateCheckDone = (payload) => {
      const status = normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform);
      render(status);
      if (payload && payload.update_available) {
        showNotice("发现新版本，可点「安装更新」完成替换", "ok");
      } else if (payload && payload.ok === false) {
        showNotice(resultText(status), "error");
      } else {
        showNotice(resultText(status), "ok");
      }
    };
    window.xiaotuUpdateInstallDone = (payload) => {
      installPreparing = false;
      if (payload && payload.ok === false) {
        const reason =
          (payload && payload.detail) ||
          (payload && payload.status && payload.status.last_error) ||
          "";
        showNotice(
          "无法启动安装流程：" +
            winFailureText(payload.code) +
            (reason ? "（" + reason + "）" : ""),
          "error"
        );
        // 补下载失败等场景：拉一次状态恢复按钮，让用户能直接重试。
        nativeBridge.xiaotuUpdateStatus.postMessage({});
      } else if (payload && (payload.code === "OPENED" || payload.code === "STARTED")) {
        showNotice("安装程序已启动，程序会自动关闭并完成安装", "ok");
      } else if (payload && payload.code === "MANUAL") {
        // 绿色版：壳已打开 zip 所在目录并退出，需用户手动解压覆盖。
        showNotice("更新包已下载并校验，已打开所在目录；关闭程序后解压覆盖原文件，重新打开即完成更新", "ok");
      }
    };
    window.xiaotuUpdateOptionsDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice("设置未保存：" + (payload.code || ""), "error");
      }
      render(normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform));
    };
    window.xiaotuUpdateDownloadDone = (payload) => {
      const status = normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform);
      render(status);
      if (payload && payload.ok === false) {
        showNotice(winFailureText(payload.code), "error");
      } else if (payload && payload.code === "STARTED") {
        showNotice("已开始下载，完成后会自动校验", "ok");
      }
    };
    window.xiaotuUpdateCancelDone = (payload) => {
      render(normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform));
      if (payload && payload.code === "CANCELED") {
        showNotice("已取消本次后台下载", "ok");
      }
    };
    nativeBridge.xiaotuUpdateStatus.postMessage({});
  }
  export async function renderUpdateCard(mount) {
    const card = mount.querySelector('[data-role="update"]');
    if (!card) return;
    // 桌面壳内：检查走壳内清单验签（Ed25519），安装交给原生完成；
    // 浏览器访问时没有该桥，仍走服务端更新源。
    const nativeBridge = nativeUpdateBridge();
    if (nativeBridge && nativeBridge.xiaotuUpdateCheckNow) {
      renderNativeUpdate(card, nativeBridge);
      return;
    }
    const canManage = can("setting.write");
    let polling = false;

    function statusText(data) {
      if (!data.configured) return "未配置更新源（[update] manifest_url / manifest_file 为空）";
      if (data.error) return "检查失败：" + data.error.message;
      if (data.update_available && data.latest) {
        return `发现新版本 ${data.latest.version}（构建 ${data.latest.build}）`;
      }
      if (data.up_to_date) return "已是最新版本";
      return "当前无需更新（" + data.code + "）";
    }

    function render(data) {
      const latest = data.latest;
      const download = data.download || {};
      const rows = [
        ["当前版本", `${data.current.version}（构建 ${data.current.build}，schema v${data.current.schema_version}）`],
        ["最新版本", latest ? `${latest.version}（构建 ${latest.build}）` : "—"],
        ["发布日期", latest && latest.published_at ? latest.published_at : "—"],
        ["状态", statusText(data)],
        [
          "清单来源",
          data.manifest_source === "network"
            ? "厂商更新源（本次已刷新）"
            : data.manifest_source === "cache"
              ? "本地缓存" + (data.manifest_stale ? "（网络不可用，已回退）" : "")
              : data.manifest_source === "file"
                ? "本地清单文件"
                : "—",
        ],
        [
          "后台策略",
          `${data.auto_check === false ? "自动检查已关闭" : "自动检查已开启"}；` +
            `${data.prefetch === false ? "不自动下载" : "发现新版本自动下载"}（config.toml）`,
        ],
      ];
      if (data.manifest_warning) {
        rows.push(["清单提示", data.manifest_warning]);
      }
      // 强制升级：服务端 min_supported_version 或清单 mandatory 都会置位；
      // 设了期限时，截止前仍按普通更新提示，到期才阻塞。
      const mandatoryReason = mandatoryReasonText(data.mandatory_reason || "");
      const deadlineRaw = data.pending_deadline || data.mandatory_deadline || "";
      const forceNow = mandatoryActive({
        mandatory: data.mandatory,
        update_available: data.update_available,
        pending_deadline: deadlineRaw,
      });
      const mandatory =
        data.update_available && data.mandatory
          ? `<div class="notice ${forceNow ? "error" : "warn"}" style="margin:0 0 10px">
               <strong>${forceNow ? "必须升级" : "计划升级"}</strong>：${
                 mandatoryReason ? escapeHtml(mandatoryReason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 latest ? latest.version : "新版本"
               )}。${
                 forceNow
                   ? "此更新已到截止时间，不能跳过。"
                   : deadlineRaw
                     ? `请在 ${escapeHtml(deadlineRaw)} 前完成升级，到期后将无法跳过。`
                     : ""
               }
             </div>`
          : "";
      let downloadLine = "";
      if (download.state === "running") {
        downloadLine = "正在下载并校验安装包…";
      } else if (download.state === "done") {
        downloadLine = `已下载并校验：${download.file}（${formatBytes(download.bytes)}）`;
      } else if (download.state === "canceled") {
        downloadLine = "已取消本次下载，可重新下载";
      } else if (download.state === "failed") {
        downloadLine = `下载或校验失败：${download.message || download.code || ""}`;
      }
      card.innerHTML = `
        <div class="page-head" style="margin-bottom:8px">
          <div>
            <h2>软件更新</h2>
            <p class="page-sub">安装会退出服务、自动替换程序并重启；替换失败自动回滚旧版本</p>
          </div>
        </div>
        ${mandatory}
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        ${downloadLine ? `<p class="page-sub" style="margin-top:8px">${escapeHtml(downloadLine)}</p>` : ""}
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">
          <button class="btn" type="button" data-role="check">检查更新</button>
          ${
            canManage
              ? `<button class="btn" type="button" data-role="download" ${
                  data.update_available && download.state !== "running" ? "" : "disabled"
                }>下载并校验</button>
                 ${
                   download.state === "running"
                     ? `<button class="btn" type="button" data-role="cancel">取消下载</button>`
                     : ""
                 }
                 <button class="btn primary" type="button" data-role="install" ${
                   data.update_available && download.state === "done" ? "" : "disabled"
                 }>安装更新</button>`
              : ""
          }
        </div>`;

      card.querySelector('[data-role="check"]').addEventListener("click", async () => {
        await refresh();
        showNotice("已重新检查更新", "ok");
      });
      const downloadButton = card.querySelector('[data-role="download"]');
      if (downloadButton) {
        downloadButton.addEventListener("click", async () => {
          try {
            await api("POST", "/api/update/download", {});
            showNotice("已开始下载，完成后会自动校验", "ok");
            poll();
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const cancelButton = card.querySelector('[data-role="cancel"]');
      if (cancelButton) {
        cancelButton.addEventListener("click", async () => {
          cancelButton.disabled = true;
          try {
            await api("POST", "/api/update/download/cancel", {});
            showNotice("已请求取消下载", "ok");
            poll();
          } catch (error) {
            showNotice(error.message, "error");
            cancelButton.disabled = false;
          }
        });
      }
      const installButton = card.querySelector('[data-role="install"]');
      if (installButton) {
        installButton.addEventListener("click", async () => {
          if (!latest) return;
          const confirmed = await askConfirm(
            `将安装 ${latest.version}（构建 ${latest.build}）。服务会先退出，` +
              "随后自动替换程序并重启；替换失败会回滚旧版本。是否继续？",
            { okText: "安装并重启", cancelText: "取消" }
          );
          if (!confirmed) return;
          try {
            const result = await api("POST", "/api/update/install", { confirm: true });
            installButton.disabled = true;
            showNotice(result.message || "已准备替换，服务即将重启", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
    }

    async function refresh() {
      const data = await api("GET", "/api/update/status");
      render(data);
      return data;
    }

    async function poll() {
      if (polling) return;
      polling = true;
      try {
        for (;;) {
          if (!document.body.contains(card)) return;
          const data = await refresh();
          if (!data.download || data.download.state !== "running") return;
          await new Promise((resolve) => setTimeout(resolve, 2000));
        }
      } catch (error) {
        showNotice(error.message, "error");
      } finally {
        polling = false;
      }
    }

    try {
      await refresh();
    } catch (error) {
      card.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
    }
  }
