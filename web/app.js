/* 小兔进销存前端入口：无构建步骤，页面脚本按职责拆到 ./app/ 下的 ES modules。
 * 模块之间只通过显式 import/export 连接，加载顺序由浏览器按依赖解析。
 */
import "./app/boot.js";
