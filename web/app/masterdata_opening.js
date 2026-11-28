/* app/masterdata_opening.js：期初导入页（库存/应收/应付/资金账户）。
 * 由 masterdata.js 拆出，供 pages.js 与入口共同加载。
 */
import { API_BASE, showNotice, state } from "./core_state.js";
import { escapeHtml } from "./core_format.js";
import { askAlert, askConfirm } from "./core_dialog.js";
import { api } from "./core_api.js";
import { downloadXlsx, isWinWebview } from "./core_export.js";

  // 期初导入：库存 / 应收 / 应付 / 资金账户四类，有业务流水后服务端会拒绝。
  export async function openingPage(mount) {
    let status = [];
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>期初导入</h2>
          <p class="page-sub">只做库存、应收、应付、资金账户四类期初；有业务流水后不再允许导入，需要重来须清空数据目录重建</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div data-role="list"></div>`;
    const list = mount.querySelector('[data-role="list"]');

    function render() {
      list.innerHTML = status
        .map(
          (item) => `<div class="card">
            <div class="card-head">
              <h3>${escapeHtml(item.label)}</h3>
              <span class="page-sub">${item.allowed ? "可导入" : escapeHtml(item.reason)}</span>
            </div>
            <div class="form-actions">
              <button class="btn" data-role="template" data-kind="${escapeHtml(item.kind)}">下载模板</button>
              <button class="btn primary" data-role="import" data-kind="${escapeHtml(item.kind)}"
                ${item.allowed ? "" : "disabled"}>导入</button>
            </div>
          </div>`
        )
        .join("");
    }

    async function reload() {
      status = await api("GET", "/api/openings/status");
      render();
    }

    list.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const kind = button.dataset.kind;
      if (button.dataset.role === "template") {
        try {
          await downloadXlsx("/api/openings/" + kind + "/template");
        } catch (error) {
          showNotice(error.message, "error");
        }
        return;
      }
      if (button.dataset.role === "import") {
        // 共用导入处理函数：接收一个 File 对象
        const runOpeningImport = async (file) => {
          const confirmed = await askConfirm(
            `将从「${file.name}」导入期初。\n\n` +
              "· 导入前会自动备份数据库，备份失败则中止；\n" +
              "· 整批校验，任何一行有错误都不会写入；\n" +
              "· 库存期初会生成「其他入库 + 期初」并直接审核。\n\n确认开始导入？",
            { okText: "开始导入", cancelText: "取消" }
          );
          if (!confirmed) return;
          try {
            const headers = {
              "Content-Type":
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              "X-File-Name": encodeURIComponent(file.name),
            };
            if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
            const response = await fetch(API_BASE + "/api/openings/" + kind + "/import", {
              method: "POST",
              credentials: "include",
              headers,
              body: file,
            });
            let payload = null;
            try {
              payload = await response.json();
            } catch (error) {
              payload = null;
            }
            if (response.ok) {
              const docs = (payload.doc_nos || []).join("、");
              showNotice(
                `导入完成：写入 ${payload.inserted} 条；导入前已备份为 ${payload.backup}。` +
                  (docs ? "期初单据：" + docs : "")
              );
              await reload();
              return;
            }
            if (payload && payload.errors && payload.errors.length) {
              const lines = payload.errors
                .slice(0, 100)
                .map((item) => `第 ${item.row} 行 · ${item.column}：${item.reason}`);
              await askAlert(payload.message + "\n\n" + lines.join("\n"));
              return;
            }
            throw new Error(
              payload && payload.message ? payload.message : "导入失败（HTTP " + response.status + "）"
            );
          } catch (error) {
            showNotice(error.message, "error");
          }
        };
        // Windows WebView2：走原生打开对话框
        if (isWinWebview()) {
          try {
            const nativeIo = await window.chrome.webview.hostObjects.native_io;
            const resultJson = await nativeIo.openFile("Excel 文件", "*.xlsx");
            const result = JSON.parse(resultJson);
            if (!result.ok) return;
            const binary = atob(result.base64);
            const bytes = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
            const file = new File([bytes], result.name, {
              type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            });
            await runOpeningImport(file);
          } catch (error) {
            showNotice(error.message, "error");
          }
          return;
        }
        const fileInput = document.createElement("input");
        fileInput.type = "file";
        fileInput.accept = ".xlsx";
        fileInput.hidden = true;
        mount.append(fileInput);
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          fileInput.remove();
          if (!file) return;
          await runOpeningImport(file);
        });
        fileInput.click();
      }
    });

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    await reload();
  }
