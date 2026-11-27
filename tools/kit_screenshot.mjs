// 起動中のエンジン(MCP ポート)から画面を 1 枚撮って保存する。tools/kit.ps1 screenshot の実体。
//   node tools/kit_screenshot.mjs --port 8881 [--out build/kit/shot.png] [--ui | --scene] [--frames 8]
//   --ui     imgui_screenshot(ImGui 込みの最終画面。既定)
//   --scene  screenshot_final(3D シーンだけ。決定論モード)
// 実マウス・実キーボード・フォーカスは使わない(MCP の仮想入力と撮影だけ)。出力は 1 行の JSON {ok, path, width, height}。
import path from "node:path";
import fs from "node:fs";
import { connect } from "./bench/lib/engine.mjs";

const a = process.argv.slice(2);
const opt = (n, d) => { const i = a.indexOf(n); return i >= 0 ? a[i + 1] : d; };
const port = Number(opt("--port", "0"));
if (!port) { console.error("usage: node tools/kit_screenshot.mjs --port N [--out file] [--ui|--scene] [--frames N]"); process.exit(2); }
const out = path.resolve(opt("--out", path.join("build", "kit", `shot-${port}-${Date.now()}.png`)));
const frames = Number(opt("--frames", "4"));
fs.mkdirSync(path.dirname(out), { recursive: true });

let conn;
try {
  conn = await connect(port);
  if (frames > 0) await conn.call("step_frames", { frames, deterministic: true }, 120_000);
  const r = a.includes("--scene")
    ? await conn.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: 4, path: out }, 300_000)
    : await conn.call("imgui_screenshot", { hideCursor: true, path: out }, 120_000);
  console.log(JSON.stringify({ ok: true, path: r.path ?? out, width: r.width, height: r.height }));
} catch (e) {
  console.log(JSON.stringify({ ok: false, error: String(e.message ?? e) }));
  process.exitCode = 1;
} finally {
  try { conn?.close(); } catch { /* 無視 */ }
}
