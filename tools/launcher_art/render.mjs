// ランチャー用の画像を、動いているエンジン（MCP の TCP ブリッジ）へ撮らせる。
//
//   node tools/launcher_art/render.mjs --port 8852 --project <撮影用プロジェクト> --out <PNG の出力先> [--scenes fps,tps,2d,empty,hero] [--mode play|game|final]
//
// 前提: エンジンは `DX12Engine.exe --background --dpi-scale 2.0 --mcp-port <port>` で起動済み（--project は付けない）。
//   ・--background は画面外・仮想入力（人の画面/カーソルに触れない）。
//   ・--dpi-scale 2.0 は撮影解像度を上げるため（シーンビューが 2 倍の大きさで描かれる）。
// 撮影用プロジェクトは build_scenes.py が作る。読み込み後、各シーンを開いて数十フレーム進め（TAA・フォグ・SSR の履歴を落ち着かせる）、
// game（MainCamera 視点）または final（エディタカメラのシーンビュー）で撮る。ImGui は写らない。
import net from "node:net";
import fs from "node:fs";
import path from "node:path";

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith("--") ? [...a, [v.slice(2), all[i + 1]]] : a), []));
const port = Number(args.port ?? 8852);
const project = args.project;
const outDir = args.out;
const scenes = (args.scenes ?? "fps,tps,2d,empty,hero").split(",");
const mode = args.mode ?? "play";
if (!project || !outDir) { console.error("usage: --project <dir> --out <dir> [--port N] [--scenes a,b] [--mode play|game|final]"); process.exit(2); }
fs.mkdirSync(outDir, { recursive: true });

const sock = net.connect(port, "127.0.0.1");
let buf = "";
let nextId = 1;
const pending = new Map();
sock.on("data", (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    try { const m = JSON.parse(line); const p = pending.get(m.id); if (p) { pending.delete(m.id); p(m); } } catch { /* 通知など */ }
  }
});
sock.on("error", (e) => { console.error("connect error:", e.message); process.exit(3); });
await new Promise((r) => sock.on("connect", r));

const call = (method, params = {}, tmo = 60000) => new Promise((resolve, reject) => {
  const id = nextId++;
  const t = setTimeout(() => { pending.delete(id); reject(new Error("timeout " + method)); }, tmo);
  pending.set(id, (m) => { clearTimeout(t); m.ok === false ? reject(new Error(method + ": " + JSON.stringify(m.error ?? m))) : resolve(m.result); });
  sock.write(JSON.stringify({ id, method, params }) + "\n");
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

console.log("open_project", project);
await call("open_project", { path: project });
for (let i = 0; i < 120; i++) {           // 非同期ロードの完了待ち（最大 ~60 秒）
  await sleep(500);
  const p = await call("ping");
  if (p.currentScene) break;
}
for (const name of scenes) {
  const scene = `scenes/launcher_${name}.json`;
  console.log("open_scene", scene);
  await call("open_scene", { path: scene });
  await sleep(1500);
  const file = path.resolve(outDir, `raw_${name}.png`);
  if (mode === "play") {
    // Play 中はアクティブな MainCamera の画角（FOV / 正射）で、ポスト適用後の絵が撮れる。
    await call("play", {}, 60000);
    await sleep(1500);
    await call("step_frames", { frames: 120 }, 120000);
    await call("screenshot_final", { path: file, settleFrames: 40 }, 120000);
    await call("stop", {}, 60000);
    await sleep(800);
  } else if (mode === "game") {
    // ポスト前の絵になる（ブルーム/フォグ/ビネットが写らない）。構図の確認用。
    await call("step_frames", { frames: 60 }, 120000);
    const r = await call("screenshot_game_view", {}, 120000);
    if (r.path) fs.copyFileSync(r.path, file);
  } else {
    await call("step_frames", { frames: 60 }, 120000);
    await call("screenshot_final", { path: file, gizmos: false, settleFrames: 30 }, 120000);
  }
  console.log("saved", file);
}
sock.end();
process.exit(0);
