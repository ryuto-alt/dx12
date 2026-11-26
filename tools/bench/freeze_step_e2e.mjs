// freeze_time / frame_step（FU-E-24）のエンジン実機チェック。
//   node tools/bench/freeze_step_e2e.mjs [--exe <DX12Engine.exe の入ったフォルダ>] [--port N]
//
// 確かめること:
//   1. 凍結したまま「frame_step 1 + screenshot_final」を 8 回繰り返すと、8 枚がビット一致（NB3 の静止 8 フレーム）。
//   2. 凍結を解いて張り直しても、同じ状態なら同じ絵（実行内で再現する）。
//   3. pinRender の切り替え・解除のあとに固定が居座らない（screenshot_final が ModeConflict にならない）。
//   4. 凍結中でも screenshot / imgui_screenshot が撮れる。
//   5. MCP が切れて 5 秒すぎたら凍結が自動で解ける。
// 窓は出さない（--background）。止めるのは自分のインスタンスだけ。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { launchInstance, connect, sleep, REPO_ROOT } from "./lib/engine.mjs";

const args = process.argv.slice(2);
const argOf = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const buildDir = argOf("--exe", path.join(REPO_ROOT, "build", "release"));
const port = Number(argOf("--port", "0")) || undefined;
// ★エンジンは起動中にシーンを自動保存することがある。ゴールデンのプロジェクトを汚さないよう、一時フォルダへ複製して使う。
const srcProject = path.join(REPO_ROOT, "tests", "golden", "projects", "golden_core");
const project = fs.mkdtempSync(path.join(os.tmpdir(), "fue24-proj-"));
fs.cpSync(srcProject, project, { recursive: true });

let failures = 0, checks = 0;
const check = (cond, msg) => { checks++; if (!cond) { failures++; console.log(`FAIL ${msg}`); } };
const hashFile = (p) => createHash("sha1").update(fs.readFileSync(p)).digest("hex");

const inst = await launchInstance({
  name: "fue24-e2e", project, port, buildDir, mode: "background",
  extraArgs: ["--scene", "scenes/csm_shadows.json", "--size", "640x360"],
});
try {
  let c = await connect(inst.port);
  const shot = async (p = {}) => hashFile((await c.call("screenshot_final", p)).path);
  await sleep(1500);

  // --- 1. 凍結 + 1 フレームずつ + 撮影 ×8 ---
  let st = await c.call("freeze_time", { frozen: true });
  check(st.frozen && st.renderPinned, "freeze_time{frozen:true} で frozen && renderPinned");
  await c.call("frame_step", { frames: 16 });
  const round = [];
  for (let i = 0; i < 8; i++) {
    await c.call("frame_step", { frames: 1 });
    round.push(await shot());
  }
  check(new Set(round).size === 1, `凍結中の 8 枚がビット一致（種類: ${new Set(round).size}）`);

  // --- 2. 解除して張り直しても同じ絵 ---
  await c.call("freeze_time", { frozen: false });
  await sleep(500);
  await c.call("freeze_time", { frozen: true });
  await c.call("frame_step", { frames: 1 });
  check((await shot()) === round[0], "解除 → 再凍結しても同じ絵（実行内で再現）");

  // --- 3. pinRender の切替と解除で固定が居座らない ---
  st = await c.call("freeze_time", { frozen: true, pinRender: false });
  check(st.frozen && !st.renderPinned, "pinRender:false で固定が外れる");
  st = await c.call("freeze_time", { frozen: false });
  check(!st.frozen && !st.renderPinned, "解除後は renderPinned も false（バグ: pin が残って frozen:false なのに固定のまま）");
  await sleep(300);
  let ok = true;
  try { await c.call("screenshot_final", { deterministic: true }); } catch (e) { ok = false; console.log(String(e)); }
  check(ok, "解除後に screenshot_final{deterministic:true} が通る（ModeConflict にならない）");
  ok = true;
  try { await c.call("screenshot_final", {}); } catch (e) { ok = false; console.log(String(e)); }
  check(ok, "解除後に screenshot_final が通る");

  await c.call("freeze_time", { frozen: true });
  st = await c.call("frame_step", { frames: 2, pinRender: false });
  check(!st.renderPinned, "frame_step{pinRender:false} の応答が実態と一致");
  st = await c.call("freeze_time", {});
  check(st.frozen && !st.renderPinned, "frame_step{pinRender:false} のあとの状態");
  await c.call("freeze_time", { frozen: false });
  st = await c.call("freeze_time", {});
  check(!st.frozen && !st.renderPinned, "frame_step{pinRender:false} → 解除で固定が残らない");

  // --- 4. 凍結中の他の撮影 ---
  await c.call("freeze_time", { frozen: true });
  ok = true;
  for (const [m, p] of [["screenshot", { deterministic: true }], ["screenshot", {}], ["imgui_screenshot", {}]]) {
    try { await c.call(m, p); } catch (e) { ok = false; console.log(String(e)); }
  }
  check(ok, "凍結中に screenshot / imgui_screenshot が撮れる");
  st = await c.call("freeze_time", {});
  check(st.frozen && st.renderPinned, "撮影のあとも凍結が保たれる");
  await sleep(300);
  st = await c.call("freeze_time", {});
  check(st.frozen && st.renderPinned, "撮影の中断の後始末なしでも固定が戻っている");

  // --- 5. 切断リース ---
  c.close();
  await sleep(7000);
  c = await connect(inst.port);
  st = await c.call("freeze_time", {});
  check(!st.frozen && !st.renderPinned, "MCP が 5 秒以上切れたら凍結が自動で解ける");
  c.close();
} finally {
  inst.kill();
  try { fs.rmSync(project, { recursive: true, force: true }); } catch { /* 残っても害はない */ }
}
console.log(`freeze_step_e2e: ${checks} checks, ${failures} failures`);
process.exit(failures === 0 ? 0 : 1);
