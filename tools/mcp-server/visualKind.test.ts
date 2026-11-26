// 視覚回帰(MCP-01 / testing/visualKind.ts・visualCore.ts・toolset/visual.ts)のテスト。エンジン不要(偽エンジン + 合成 PNG)。
//   [1] 比較の純ロジック: bitExact / 1 画素の変化 = 不合格 + diffBBox / adapter 違い = skipped
//   [2] project suite(visual_regression ジョブ本体): approve → compare / reason 無しの approve / 履歴と needsHumanReview
//   [3] golden suite の判定が golden.mjs と同じ式(環境違い・基準画像なし・許容差)
//   [4] ツール dx12_visual: reason 無しは E_MISSING_PARAM、tests/golden の approve は E_GUARDED(dx12_call は confirm が要る)
import fs from "node:fs";
import path from "node:path";
import { PNG } from "pngjs";
import "./testEnv.ts";
import { startMcp } from "./stdioClient.ts";
import { startMockEngine } from "./mockEngine.ts";
import { tmpDir, rmTree } from "./fleetTestKit.ts";
import { loadJobsConfig } from "./jobs/config.ts";
import type { KindEnv } from "./jobs/env.ts";
import { JobFailure } from "./jobs/manager.ts";
import { visualRegressionKind } from "./testing/visualKind.ts";
import { approveProjectItem, compareImagesPair, judgeGolden, keyCompat, loadDiffLib, PROJECT_TOLERANCE, readBaselines, readHistoryLog, pendingHumanReview } from "./testing/visualCore.ts";
import type { EngineProvider } from "./testing/engineProvider.ts";
import { readPng } from "./testing/visualPng.ts";

let failed = 0, total = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  total++;
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${typeof detail === "string" ? detail : JSON.stringify(detail).slice(0, 900)}` : ""}`); }
}
const dirs: string[] = [];
const mk = (l: string) => { const d = tmpDir(l); dirs.push(d); return d; };
const REPO = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1")), "..", "..");

// ── 偽エンジン ───────────────────────────────────────────────────────────────
const world = { scene: "scenes/a.json", adapter: "FakeGPU A", w: 64, h: 48, base: [40, 90, 160], bad: null as null | [number, number] };
function makePng(): Buffer {
  const png = new PNG({ width: world.w, height: world.h });
  for (let i = 0; i < world.w * world.h; i++) {
    const x = i % world.w, y = (i / world.w) | 0;
    const isBad = world.bad && world.bad[0] === x && world.bad[1] === y;
    png.data[i * 4] = isBad ? 255 : world.base[0]; png.data[i * 4 + 1] = isBad ? 0 : world.base[1]; png.data[i * 4 + 2] = isBad ? 0 : world.base[2]; png.data[i * 4 + 3] = 255;
  }
  return PNG.sync.write(png);
}
const projectDir = mk("vproj");
const engineCall = async (method: string, params: any = {}): Promise<any> => {
  switch (method) {
    case "ping": return { currentScene: world.scene, baseDir: projectDir, gpu: { name: world.adapter, driver: "1.0" }, engineVersion: "9.9.9", sceneGeneration: 1, sceneLoading: false };
    case "open_scene": world.scene = params.path; return {};
    case "set_editor_camera": case "step_frames": return {};
    case "screenshot_final": { fs.mkdirSync(path.dirname(params.path), { recursive: true }); fs.writeFileSync(params.path, makePng()); return { path: params.path, width: world.w, height: world.h }; }
    default: throw new Error(`unknown method ${method}`);
  }
};
const provider: EngineProvider = {
  attach: () => ({ call: engineCall as any, close: async () => {} }),
  launch: async () => { throw new Error("launch は使わない"); },
};
const cfg = { ...loadJobsConfig({ ...process.env, DX12_JOBS_DIR: mk("jobs") }), repoDir: REPO };
const env = { cfg, resolveEngineId: () => undefined, enginePort: () => undefined, callEngine: (_e: any, m: string, p: any) => engineCall(m, p), fleet: () => null, callTool: async () => ({ text: "", isError: true, images: 0 }), now: () => Date.now() } as unknown as KindEnv;
const kind = visualRegressionKind(env, { provider, repoDir: REPO });
const artifacts: string[] = [];
const mkCtx = () => {
  const dir = mk("art");
  return { job: { id: "j-test" }, signal: new AbortController().signal, progress() {}, log() {}, artifactsDir: dir, addArtifact: (a: any) => artifacts.push(a.path), setEngine() {} } as any;
};
async function errOf(fn: () => Promise<unknown>): Promise<any> { try { await fn(); return null; } catch (e: any) { return e?.body ?? e?.code ?? { thrown: String(e?.message ?? e) }; } }

const lib = await loadDiffLib(REPO);

console.log("[1] 比較の純ロジック");
{
  const a = readPng(writeTmp(makePng()));
  const b = readPng(writeTmp(makePng()));
  const same = compareImagesPair(lib, a, b, PROJECT_TOLERANCE, lib.sha256(a) === lib.sha256(b));
  check("同じ入力は bitExact:true・合格", same.bitExact === true && same.pass && same.diffBBox === null, same);
  world.bad = [10, 5];
  const c = readPng(writeTmp(makePng()));
  world.bad = null;
  const diff = compareImagesPair(lib, a, c, PROJECT_TOLERANCE, false);
  check("1 画素を変えると不合格で diffBBox が正しい", !diff.pass && diff.diffBBox?.x === 10 && diff.diffBBox?.y === 5 && diff.diffBBox?.w === 1 && diff.diffBBox?.h === 1 && diff.bitExact === false, diff);
  check("keyCompat: adapter が違えば比べない", !keyCompat({ adapter: "A", driverVersion: null, engineVersion: null, renderSettingsHash: null, qualityPreset: null }, { adapter: "B", driverVersion: null, engineVersion: null, renderSettingsHash: null, qualityPreset: null }).comparable);
  const kc = keyCompat({ adapter: "A", driverVersion: "1", engineVersion: "1", renderSettingsHash: "x", qualityPreset: null }, { adapter: "A", driverVersion: "2", engineVersion: "1", renderSettingsHash: "x", qualityPreset: null });
  check("keyCompat: ドライバ違いは比べるが警告に出す", kc.comparable && kc.changes.length === 1 && /driverVersion/.test(kc.changes[0]), kc);
}
function writeTmp(buf: Buffer): string { const f = path.join(mk("png"), "x.png"); fs.writeFileSync(f, buf); return f; }

console.log("[2] project suite(ジョブ本体)");
{
  const e0 = await errOf(() => approveProjectItem({ lib, call: engineCall as any, projectDir, scene: "scenes/a.json", name: "hall", reason: undefined }));
  check("reason なしの approve は E_MISSING_PARAM", e0?.code === "E_MISSING_PARAM", e0);
  check("拒否された approve は何も書かない", Object.keys(readBaselines(projectDir).baselines).length === 0);
  const rec = await approveProjectItem({ lib, call: engineCall as any, projectDir, scene: "scenes/a.json", name: "hall", reason: "初回の基準", camera: { position: [0, 1, 2], target: [0, 0, 0] } });
  check("approve: approvedBy:ai・humanReviewed:false・adapter を記録", rec.approvedBy === "ai" && rec.humanReviewed === false && rec.key.adapter === "FakeGPU A", rec);
  const hist = readHistoryLog(projectDir);
  check("history.jsonl に approvedBy:'ai' と reason が残る", hist.length === 1 && (hist[0] as any).approvedBy === "ai" && (hist[0] as any).reason === "初回の基準", hist);
  check("pendingHumanReview に出る(quality_gate 向け)", pendingHumanReview(projectDir).includes("scenes/a.json/hall"));

  const r1 = await kind.run({ suite: "project", project: projectDir }, mkCtx());
  check("同じ絵なら合格・bitExact", r1.ok === true && (r1.summary as any).passed === 1 && (r1.result as any).results[0].bitExact === true, r1.summary);
  check("AI が approve した基準は needsHumanReview", (r1.summary as any).needsHumanReview?.length === 1, r1.summary);

  world.bad = [10, 5];
  const r2 = await kind.run({ suite: "project", project: projectDir }, mkCtx());
  const it = (r2.result as any).results[0];
  check("1 画素の変化は不合格・diffBBox・diff 画像", r2.ok === false && it.status === "fail" && it.diffBBox?.x === 10 && it.diffBBox?.y === 5 && fs.existsSync(it.diffPng) && fs.existsSync(it.sideBySide), it);
  check("不合格のジョブは E_JOB_FAILED", (r2.error as any)?.code === "E_JOB_FAILED", r2.error);
  world.bad = null;

  world.adapter = "FakeGPU B";
  const r3 = await kind.run({ suite: "project", project: projectDir }, mkCtx());
  check("adapter が違う基準は比べず skipped(不合格にしない)", r3.ok === true && (r3.result as any).results[0].status === "skipped" && /adapter/.test((r3.result as any).results[0].skippedReason), r3.result);
  world.adapter = "FakeGPU A";
}

console.log("[3] golden suite の判定(golden.mjs と同じ式)");
{
  const goldenDir = path.join(REPO, "tests", "golden");
  const store: any = await import(new URL(`file:///${path.join(REPO, "tools/bench/lib/goldenStore.mjs").replace(/\\/g, "/")}`).href);
  const cfgG = store.loadGoldenEntries(goldenDir), base = store.loadBaseline(goldenDir);
  const withRef = cfgG.scenes.filter((s: any) => base.scenes[s.name] && fs.existsSync(path.join(goldenDir, s.kind === "ui" ? "ui" : "", `${s.name}.png`)));
  check("基準の付いたゴールデンが 1 件以上ある", withRef.length > 0, withRef.length);
  let allOk = true, allBad = true;
  for (const s of withRef) {
    const refFile = path.join(goldenDir, s.kind === "ui" ? "ui" : "", `${s.name}.png`);
    const ref = readPng(refFile);
    const b = base.scenes[s.name];
    // ネイティブ = 基準 PNG そのもの(縮小は恒等)。基準の native サイズに合わせた偽のメタを渡して判定だけを見る
    const okV = judgeGolden(lib, { native: ref, ref, baseline: { ...b, nativeWidth: ref.width, nativeHeight: ref.height }, width: ref.width, height: ref.height, tol: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 }, strict: true });
    if (!okV.pass) allOk = false;
    const broken = { width: ref.width, height: ref.height, data: Buffer.from(ref.data) };
    for (let i = 0; i < broken.data.length; i += 4) { broken.data[i] = 255 - broken.data[i]; }
    const badV = judgeGolden(lib, { native: broken, ref, baseline: { ...b, nativeWidth: ref.width, nativeHeight: ref.height }, width: ref.width, height: ref.height, tol: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 }, strict: true });
    if (badV.pass) allBad = false;
  }
  check("基準と同じ絵は全件合格", allOk);
  check("赤を反転した絵は全件不合格", allBad);
  const s0 = withRef[0], r0 = readPng(path.join(goldenDir, s0.kind === "ui" ? "ui" : "", `${s0.name}.png`));
  const env1 = judgeGolden(lib, { native: r0, ref: r0, baseline: { nativeWidth: r0.width + 1, nativeHeight: r0.height, nativeSha256: "x" }, width: r0.width, height: r0.height, tol: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 }, strict: true });
  check("ネイティブ解像度が違えば環境違い: strict は不合格", env1.envMismatch === true && env1.pass === false && !env1.skipped, env1);
  const env2 = judgeGolden(lib, { native: r0, ref: r0, baseline: { nativeWidth: r0.width + 1, nativeHeight: r0.height, nativeSha256: "x" }, width: r0.width, height: r0.height, tol: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 }, strict: false });
  check("ネイティブ解像度が違えば環境違い: 非 strict は SKIP", env2.skipped === true && env2.pass === true, env2);
  const noRef = judgeGolden(lib, { native: r0, ref: null, baseline: undefined, width: r0.width, height: r0.height, tol: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 }, strict: true });
  check("基準画像が無ければ不合格", noRef.pass === false && /基準画像が無い/.test(noRef.reasons[0]), noRef);
  const tolD = { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 };
  const mis = { nativeWidth: r0.width + 1, nativeHeight: r0.height, nativeSha256: "x" };
  const uiMis = judgeGolden(lib, { native: r0, ref: null, baseline: mis, width: r0.width, height: r0.height, tol: tolD, strict: false, ui: true });
  const d3Mis = judgeGolden(lib, { native: r0, ref: null, baseline: mis, width: r0.width, height: r0.height, tol: tolD, strict: false });
  check("UI は基準画像なしを環境違いより先に見る(--no-strict でも FAIL)/ 3D は環境違いが先(SKIP)", uiMis.pass === false && !uiMis.skipped && d3Mis.skipped === true, { uiMis, d3Mis });
}

console.log("[4] ツール dx12_visual(stdio)");
{
  const project = mk("tproj");
  const M = (name: string) => ({ name, category: "test", summary: name, effect: "runtime", mode: "any", timeoutMs: 8000, params: [], source: "meta" }) as any;
  const mock = await startMockEngine({
    safety: true, pingExtra: () => ({ baseDir: project, currentScene: "scenes/a.json" }),
    methods: ["get_editor_camera", "set_editor_camera", "screenshot_final"].map((n) => ({ ...M(n), handler: () => ({}) })),
  });
  const c = startMcp({ DX12_MCP_SURFACE: "full", DX12_MCP_PORT: String(mock.port), DX12_MCP_PORT_FILE: path.join(mk("pf"), "none.port"), DX12_JOBS_DIR: mk("jobs2"), DX12_PROJECT_DIR: project, DX12_REPO_DIR: REPO });
  try {
    await c.initialize();
    const names = (await c.rpc("tools/list")).result.tools.map((t: any) => t.name);
    check("dx12_visual / dx12_perf_gate は tools/list に出ない(長尾。dx12_call で使う)", !names.includes("dx12_visual") && !names.includes("dx12_perf_gate"));
    const g0 = await c.call("dx12_call", { name: "dx12_visual", args: { op: "approve", suite: "golden", only: ["core_pbr_spheres"], reason: "x" } });
    check("tests/golden の approve は confirm 無しで E_GUARDED", g0.error_code === "E_GUARDED", g0);
    const g1 = await c.call("dx12_call", { name: "dx12_visual", args: { op: "approve", name: "hall" } });
    check("reason 無しの approve は E_MISSING_PARAM", g1.error_code === "E_MISSING_PARAM", g1);
    const g2 = await c.call("dx12_call", { name: "dx12_visual", args: { op: "list" } });
    check("list は基準が無ければ空", (g2.result?.baselines ?? g2.baselines)?.length === 0, g2);
    const g3 = await c.call("dx12_call", { name: "dx12_visual", args: { op: "bogus" } });
    check("知らない op は構造化エラー", g3.ok === false || !!g3.error_code, g3);
  } finally { c.close(); await mock.close?.(); }
}

for (const d of dirs) rmTree(d);
console.log(failed ? `\nNG: ${failed}/${total} 件失敗` : `\nOK: visual ${total} 項目すべて通過`);
process.exit(failed ? 1 : 0);
void JobFailure;
