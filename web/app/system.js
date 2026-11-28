/* app/system.js：恢复向导、关于页与导航配置。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, can, escapeHtml, showNotice } from "./core.js";
import { formatBytes, renderUpdateCard } from "./update.js";

  export async function restorePage(mount) {
    const canRestore = can("*");
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>备份与恢复</h2>
          <p class="page-sub">从备份目录选择一份恢复；恢复前会自动生成安全备份，失败会回滚</p>
        </div>
      </div>
      <div class="card" data-role="result"></div>
      <div class="card" style="margin-top:12px">
        <div class="page-head" style="margin-bottom:8px">
          <div><h3 style="margin:0">备份文件</h3></div>
          <button class="btn" type="button" data-role="reload">刷新</button>
        </div>
        <div data-role="list">正在读取…</div>
      </div>
      <div class="card" data-role="precheck" style="margin-top:12px;display:none"></div>`;

    const resultCard = mount.querySelector('[data-role="result"]');
    const listBox = mount.querySelector('[data-role="list"]');
    const precheckCard = mount.querySelector('[data-role="precheck"]');

    async function loadResult() {
      try {
        const data = await api("GET", "/api/restore/result");
        const result = data.result;
        if (!result) {
          resultCard.innerHTML = `<p class="page-sub" style="margin:0">暂无恢复记录。</p>`;
          return;
        }
        const ok = result.ok;
        const text = ok
          ? `上次恢复成功：${result.file}（schema v${result.schema_version}）` +
            (result.safety_backup ? `，恢复前安全备份 ${result.safety_backup}` : "")
          : `上次恢复失败：${result.error_message || result.error_code || "未知原因"}`;
        resultCard.innerHTML = `
          <p style="margin:0;color:${ok ? "inherit" : "#b42318"}">${escapeHtml(text)}</p>
          <p class="page-sub" style="margin:4px 0 0">${escapeHtml(result.at || "")}</p>
          ${canRestore ? '<button class="btn" type="button" data-role="clear" style="margin-top:8px">清除记录</button>' : ""}`;
        const clear = resultCard.querySelector('[data-role="clear"]');
        if (clear) {
          clear.addEventListener("click", async () => {
            try {
              await api("POST", "/api/restore/result/clear", {});
              await loadResult();
              showNotice("恢复记录已清除", "ok");
            } catch (error) {
              showNotice(error.message, "error");
            }
          });
        }
      } catch (error) {
        resultCard.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
      }
    }

    function renderPrecheck(inspection) {
      precheckCard.style.display = "";
      const rows = [
        ["备份文件", inspection.file],
        ["修改时间", inspection.modified_at || "—"],
        ["大小", formatBytes(inspection.bytes)],
        ["schema", inspection.schema_version ? `v${inspection.schema_version}` : "—"],
        ["当前程序 schema", `v${inspection.current_schema_version}`],
        ["完整性检查", inspection.integrity_ok ? "通过" : "未通过"],
        ["本程序数据库", inspection.is_app_database ? "是" : "否"],
        ["可恢复", inspection.ok ? "是" : "否"],
      ];
      if (inspection.will_migrate) {
        rows.push(["恢复后", "启动时会自动迁移到当前 schema"]);
      }
      precheckCard.innerHTML = `
        <h3 style="margin-top:0">预检查结果</h3>
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 140px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        ${
          inspection.ok
            ? ""
            : `<p class="page-sub" style="color:#b42318">${escapeHtml(
                (inspection.error && inspection.error.message) || "该备份不可用于恢复"
              )}</p>`
        }
        ${
          inspection.ok && canRestore
            ? '<button class="btn primary" type="button" data-role="apply" style="margin-top:12px">恢复到该备份</button>'
            : ""
        }`;
      const apply = precheckCard.querySelector('[data-role="apply"]');
      if (apply) {
        apply.addEventListener("click", async () => {
          const confirmed = await askConfirm(
            `将用 ${inspection.file} 覆盖当前数据库，程序随后退出并在下次启动时完成恢复。` +
              (inspection.will_migrate ? "该备份 schema 较旧，恢复后会自动迁移。" : "") +
              "是否继续？",
            { okText: "确认恢复", cancelText: "取消" }
          );
          if (!confirmed) return;
          apply.disabled = true;
          try {
            await api("POST", "/api/restore/apply", { file: inspection.file, confirm: true });
            showNotice("已进入维护状态，程序即将退出；重新打开后将完成恢复", "ok");
            precheckCard.innerHTML =
              '<p style="margin:0">恢复已安排，程序正在退出。请重新启动程序，完成后会在本页显示结果。</p>';
          } catch (error) {
            apply.disabled = false;
            if (error.network) {
              showNotice("程序可能已退出，请重新启动后在顶部查看恢复结果", "ok");
            } else {
              showNotice(error.message, "error");
            }
          }
        });
      }
    }

    async function loadList() {
      listBox.textContent = "正在读取…";
      try {
        const data = await api("GET", "/api/restore/backups");
        if (!data.rows.length) {
          listBox.innerHTML = `<p class="empty">备份目录内没有可恢复的 .db 文件</p>`;
          return;
        }
        listBox.innerHTML = `
          <table class="table">
            <thead><tr><th>文件</th><th>修改时间</th><th>大小</th><th>schema</th><th class="actions">操作</th></tr></thead>
            <tbody>${data.rows
              .map(
                (row) => `<tr>
                  <td>${escapeHtml(row.file)}</td>
                  <td>${escapeHtml(row.modified_at || "—")}</td>
                  <td>${formatBytes(row.bytes)}</td>
                  <td>${row.schema_version ? "v" + row.schema_version : "—"}</td>
                  <td class="actions"><button class="btn" type="button" data-file="${escapeHtml(
                    row.file
                  )}">预检查</button></td>
                </tr>`
              )
              .join("")}</tbody>
          </table>`;
        listBox.querySelectorAll("button[data-file]").forEach((button) => {
          button.addEventListener("click", async () => {
            try {
              const inspection = await api("POST", "/api/restore/precheck", {
                file: button.dataset.file,
              });
              renderPrecheck(inspection);
            } catch (error) {
              showNotice(error.message, "error");
            }
          });
        });
      } catch (error) {
        listBox.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
      }
    }

    mount.querySelector('[data-role="reload"]').addEventListener("click", loadList);
    await Promise.all([loadResult(), loadList()]);
  }
  export async function aboutPage(mount) {
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <img src="/icons/about-512.png" alt="小兔进销存" width="64" height="64"
               style="display:block;margin-bottom:10px;border-radius:15px" />
          <h2>关于与诊断</h2>
          <p class="page-sub">只读信息，可直接复制贴给维护人员；不包含口令、密钥或会话令牌</p>
        </div>
        <button class="btn" type="button" data-role="copy">复制诊断信息</button>
      </div>
      <div class="card" data-role="body">正在读取…</div>
      <div class="card" data-role="update" style="margin-top:12px">正在读取更新状态…</div>`;

    const body = mount.querySelector('[data-role="body"]');
    let diagnosticText = "";
    try {
      const data = await api("GET", "/api/diagnostics");
      const mode = data.mode === "local" ? "单机版" : "网络版";
      const edition = data.edition || {};
      const build = data.build || {};
      const rows = [
        ["应用", `${data.app} ${data.version}`],
        ["运行形态", `${mode}（${edition.label || edition.code || "—"} / ${edition.code || "—"}）`],
        ["schema", `v${data.schema_version}，迁移 ${data.migrations} 条`],
        ["构建", `${build.profile || "—"} · ${build.compiler || "—"}`],
        ["构建时间", `${build.date || "—"} ${build.time || "—"}`],
        ["数据目录", data.data_dir || "—"],
        ["数据库文件", `${data.database_path || "—"}（${formatBytes(data.database_bytes)}）`],
        ["备份目录", data.backup_dir || "—"],
        ["监听", data.listen || "—"],
        ["当前用户", `${data.user || "—"}（${data.role || "—"}）`],
      ];
      body.innerHTML = `<table class="table"><tbody>${rows
        .map(
          ([label, value]) =>
            `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
              value
            )}</td></tr>`
        )
        .join("")}</tbody></table>`;
      diagnosticText = rows.map(([label, value]) => `${label}：${value}`).join("\n");
    } catch (error) {
      body.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
    }

    mount.querySelector('[data-role="copy"]').addEventListener("click", async () => {
      if (!diagnosticText) {
        showNotice("诊断信息尚未读取成功", "error");
        return;
      }
      try {
        await navigator.clipboard.writeText(diagnosticText);
      } catch (error) {
        const area = document.createElement("textarea");
        area.value = diagnosticText;
        document.body.appendChild(area);
        area.select();
        document.execCommand("copy");
        area.remove();
      }
      showNotice("诊断信息已复制", "ok");
    });

    await renderUpdateCard(mount);
  }

  // ---------- 导航与启动 ----------

  // 二级导航：组只影响展示，页面 key 与权限判定不变。
  // 报表的归属还没定，先作为顶层入口，不放进任何组。
  export const NAV_GROUPS = [
    { key: "purchase", title: "采购" },
    { key: "sales", title: "销售" },
    { key: "stock", title: "库存" },
    { key: "finance", title: "财务" },
    { key: "master", title: "基础资料" },
    { key: "system", title: "系统" },
  ];
  // 一级顺序：组与顶层页面可以交错；没有列出的组或页面按原顺序补在后面。
  export const NAV_LAYOUT = [
    { type: "group", key: "purchase" },
    { type: "group", key: "sales" },
    { type: "group", key: "stock" },
    { type: "group", key: "finance" },
    { type: "page", key: "dashboard" },
    { type: "page", key: "sales-report" },
    { type: "page", key: "reports" },
    { type: "group", key: "master" },
    { type: "group", key: "system" },
  ];
