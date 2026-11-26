// visual_regression の中身(MCP-01)。
//   ・比較ロジックは移植しない。tools/bench/lib/imageDiff.mjs(sha256 / resampleArea / compareImages / judge)を遅延 import して使い、
//     判定の出どころを golden.mjs と 1 つに保つ(配布版 dx12-mcp にはこのファイルが無いので、無ければ E_UNSUPPORTED)。
//   ・判定は 3 段: (1) bitExact = ネイティブ解像度の SHA-256 /(2) 面積平均で縮めた絵の許容差(合否はここ)/(3) 任意の FLIP(parity の venv があるときだけ)。
//   ・suite は 2 種類: golden(tests/golden。golden.mjs と同じ基準・許容差。全件の合否一致は実機照合待ち)/ project(<project>/.dx12/baselines。adapter 違いは比べず skipped)。
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { spawnSync } from "node:child_process";
import { diffStats, heatmap, sideBySide, readPng, writePng, type Rgba, type DiffBBox } from "./visualPng.ts";
import { appendJsonl, errBody, fail, readJson, readJsonl, round, sleep, StepError, writeJsonAtomic, type EngineCall } from "./common.ts";
import { importBench } from "./engineProvider.ts";

export type Tolerance = { pixelThreshold: number; maxDiffRatio: number; maxMeanAbs: number };
export type DiffLib = {
  sha256(img: Rgba): string;
  resampleArea(img: Rgba, w: number, h: number): Rgba;
  compareImages(a: Rgba, b: Rgba, th: number): { diffPixels: number; diffRatio: number; meanAbs: number; maxDist: number; diffImage: Rgba };
  judge(c: any, tol: Tolerance): { pass: boolean; reasons: string[] };
  /** golden.mjs と共有する既定の許容差(imageDiff.mjs が持つ。ここに値を写さない)。 */
  DEFAULT_TOLERANCE: Tolerance;
};

let libCache: { dir: string; lib: DiffLib } | null = null;
export async function loadDiffLib(repoDir: string): Promise<DiffLib> {
  if (libCache && libCache.dir === repoDir) return libCache.lib;
  const m = await importBench(repoDir, "imageDiff.mjs");
  if (!m || typeof m.compareImages !== "function" || !m.DEFAULT_TOLERANCE) fail("E_UNSUPPORTED", "visual_regression: tools/bench/lib/imageDiff.mjs が見つからない(リポジトリの外では比較ロジックを持てない)", { fix: [{ command: "DX12_REPO_DIR にエンジンのリポジトリのルートを渡す", why: "判定の出どころを golden.mjs と共有するため" }] });
  libCache = { dir: repoDir, lib: m as DiffLib };
  return m as DiffLib;
}

/** project suite の既定。基準は同じ adapter で撮ったものだけなので、golden より厳しい(1 画素でも 2 LSB を超えて違えば不合格。±2 LSB のゆらぎは許す)。 */
export const PROJECT_TOLERANCE: Tolerance = { pixelThreshold: 2, maxDiffRatio: 0, maxMeanAbs: 0.25 };

// ── 1 組の比較(段 1〜2) ───────────────────────────────────────────────────────
export type CompareResult = {
  pass: boolean; bitExact: boolean | null; diffRatio: number; maxDelta: number; meanAbs: number; maxDist: number;
  diffBBox: DiffBBox | null; reasons: string[];
  diffPng?: string; sideBySide?: string;
};

/** a = 基準、b = 今回(同じ大きさ)。許容差の判定は golden.mjs と同じ judge。 */
export function compareImagesPair(lib: DiffLib, a: Rgba, b: Rgba, tol: Tolerance, bitExactRef?: boolean | null): CompareResult {
  const c = lib.compareImages(a, b, tol.pixelThreshold);
  const j = lib.judge(c, tol);
  const st = diffStats(a, b, 0);   // 1 LSB でも違う画素の外接矩形と最大差
  return {
    pass: j.pass, bitExact: bitExactRef ?? (st.diffPixels === 0), diffRatio: c.diffRatio, maxDelta: st.maxDelta, meanAbs: c.meanAbs, maxDist: c.maxDist,
    diffBBox: st.diffBBox, reasons: j.reasons,
  };
}

/** 差分画像と並べ画像(長辺 1024px)を書く。★JPEG のエンコーダを持たない(依存は pngjs だけ)ので PNG で書く。 */
export function writeEvidence(dir: string, stem: string, base: Rgba, cur: Rgba): { diffPng: string; sideBySide: string; sideBySideFormat: "png" } {
  fs.mkdirSync(dir, { recursive: true });
  const diffPng = path.join(dir, `${stem}.diff.png`);
  writePng(diffPng, heatmap(base, cur));
  const sbs = sideBySide([base, cur, heatmap(base, cur)], 340);
  const long = Math.max(sbs.width, sbs.height);
  const f = long > 1024 ? 1024 / long : 1;
  const out = f < 1 ? scaleDown(sbs, Math.max(1, Math.round(sbs.width * f)), Math.max(1, Math.round(sbs.height * f))) : sbs;
  const sp = path.join(dir, `${stem}.side.png`);
  writePng(sp, out);
  return { diffPng, sideBySide: sp, sideBySideFormat: "png" };
}
function scaleDown(src: Rgba, w: number, h: number): Rgba {
  const out = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const sx = Math.min(src.width - 1, Math.floor(x * src.width / w)), sy = Math.min(src.height - 1, Math.floor(y * src.height / h));
    const so = (sy * src.width + sx) * 4, o = (y * w + x) * 4;
    out[o] = src.data[so]; out[o + 1] = src.data[so + 1]; out[o + 2] = src.data[so + 2]; out[o + 3] = 255;
  }
  return { width: w, height: h, data: out };
}

/** 段 3: FLIP(任意)。parity の venv の python があるときだけ。無ければ skipped。 */
export function flipMean(repoDir: string, aPng: string, bPng: string, env: NodeJS.ProcessEnv = process.env): { status: "ok"; mean: number } | { status: "skipped"; reason: string } {
  const cands = [env.DX12_PARITY_PYTHON, path.join(repoDir, "tools", "parity", ".venv", "Scripts", "python.exe"), path.join(repoDir, "tools", "parity", ".venv", "bin", "python")].filter(Boolean) as string[];
  const py = cands.find((c) => fs.existsSync(c));
  if (!py) return { status: "skipped", reason: "tools/parity の venv が無い(tools/parity/setup.ps1 で作る)" };
  const code = "import sys,flip_evaluator as f\nr=f.evaluate(sys.argv[1],sys.argv[2],'LDR',inputsRGB=True)\nprint(float(r[1]))";
  const r = spawnSync(py, ["-I", "-c", code, aPng, bPng], { encoding: "utf8", timeout: 60_000 });
  const v = Number(String(r.stdout ?? "").trim().split(/\r?\n/).pop());
  if (r.status !== 0 || !Number.isFinite(v)) return { status: "skipped", reason: `flip-evaluator を実行できない(${String(r.stderr ?? "").trim().slice(0, 120)})` };
  return { status: "ok", mean: round(v, 5) as number };
}

// ── golden suite の判定(golden.mjs と同じ式) ─────────────────────────────────
export type GoldenBaseline = { nativeWidth: number; nativeHeight: number; nativeSha256: string; compareSha256?: string };
export type GoldenVerdict = { pass: boolean; skipped?: boolean; envMismatch?: boolean; bitExact?: boolean; diffRatio?: number; meanAbs?: number; maxDist?: number; reasons: string[]; native: string; sha: string };

/**
 * 3D ゴールデン 1 件の判定。golden.mjs の runScenes と同じ順序・同じ文言の条件:
 *   ネイティブ解像度の違い → 環境違い(strict は不合格、そうでなければ SKIP)/ 基準画像なし → 不合格 / 縮めて許容差。
 * native = 今回のネイティブ画像、ref = 基準 PNG(width x height)。maskedNative は UI のとき applyMasks 後のもの。
 */
export function judgeGolden(lib: DiffLib, o: { native: Rgba; ref: Rgba | null; baseline: GoldenBaseline | undefined; width: number; height: number; tol: Tolerance; strict: boolean; ui?: boolean }): GoldenVerdict {
  const sha = lib.sha256(o.native);
  const native = `${o.native.width}x${o.native.height}`;
  const v: GoldenVerdict = { pass: true, reasons: [], native, sha };
  const b = o.baseline;
  // UI の経路(golden.mjs の runUiScenes)は、基準画像なしを環境違いより先に見る。3D の経路は逆。
  if (o.ui && !o.ref) { v.pass = false; v.reasons.push("基準画像が無い(--update で撮る)"); if (b) v.bitExact = b.nativeSha256 === sha; return v; }
  if (b && (b.nativeWidth !== o.native.width || b.nativeHeight !== o.native.height)) {
    v.pass = !o.strict; v.skipped = !o.strict; v.envMismatch = true;
    v.reasons.push(`環境違い: ネイティブ解像度が基準と違う(基準 ${b.nativeWidth}x${b.nativeHeight}、今回 ${native})`);
    return v;
  }
  if (!o.ref) { v.pass = false; v.reasons.push("基準画像が無い(--update で撮る)"); if (b) v.bitExact = b.nativeSha256 === sha; return v; }
  const small = lib.resampleArea(o.native, o.width, o.height);
  const cmp = lib.compareImages(o.ref, small, o.tol.pixelThreshold);
  v.diffRatio = cmp.diffRatio; v.meanAbs = cmp.meanAbs; v.maxDist = cmp.maxDist;
  const j = lib.judge(cmp, o.tol);
  v.pass = j.pass; v.reasons.push(...j.reasons);
  if (b) v.bitExact = b.nativeSha256 === sha;
  return v;
}

// ── 撮影(golden.mjs の captureScene / openScene / createNewScene と同じ手順) ─────────────────────────
export async function openSceneWait(call: EngineCall, scene: string, signal?: AbortSignal): Promise<void> {
  for (let i = 0; i < 600; i++) {
    if (signal?.aborted) throw new Error("aborted");
    const p = await call("ping");
    if (p.currentScene === scene) return;
    try { await call("open_scene", { path: scene }, { timeout: 300_000 }); }
    catch (e: any) { if (!/already in progress/.test(String(e?.message))) throw e; }
    await sleep(500);
  }
  fail("E_NOT_FOUND_SCENE", `シーンが開かない: ${scene}`);
}
export async function newSceneWait(call: EngineCall): Promise<void> {
  let before = (await call("ping")).sceneGeneration;
  for (let i = 0; i < 600; i++) { const p = await call("ping"); if (!p.sceneLoading) { before = p.sceneGeneration; break; } await sleep(500); }
  await call("new_scene", {}, { timeout: 300_000 });
  for (let i = 0; i < 600; i++) { const p = await call("ping"); if (!p.sceneLoading && p.sceneGeneration > before) return; await sleep(200); }
  fail("E_ENGINE_TIMEOUT", "new_scene が完了しない");
}
export type CaptureSpec = { camera?: { position: number[]; target: number[] }; warmupFrames: number; settleFrames: number };
/** 決定論スクショ。戻り値はネイティブ解像度の画像と保存先。 */
export async function captureFinal(call: EngineCall, outFile: string, s: CaptureSpec): Promise<{ img: Rgba; file: string }> {
  if (s.camera) await call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
  if (s.warmupFrames > 0) await call("step_frames", { frames: s.warmupFrames }, { timeout: 300_000 });
  const r = await call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: s.settleFrames, path: outFile }, { timeout: 300_000 });
  const file = String(r?.path ?? outFile);
  return { img: readPng(file), file };
}


// ── project suite: <project>/.dx12/baselines ──────────────────────────────────
export type BaselineKey = { adapter: string | null; driverVersion: string | null; engineVersion: string | null; renderSettingsHash: string | null; qualityPreset: string | null };
export type ProjectBaseline = {
  scene: string; name: string; key: BaselineKey; nativeSha256: string; width: number; height: number;
  camera?: { position: number[]; target: number[] }; warmupFrames?: number; settleFrames?: number;
  approvedAt: string; approvedBy: "ai" | "human"; reason: string; humanReviewed: boolean;
};
export type BaselinesFile = { version: 1; baselines: Record<string, ProjectBaseline> };
export const baselinesDir = (projectDir: string) => path.join(projectDir, ".dx12", "baselines");
export const baselineKeyOf = (scene: string, name: string) => `${scene.replace(/\\/g, "/")}/${name}`;
export function safeSeg(s: string): string {
  const t = String(s).replace(/\\/g, "/");
  if (!t || t.split("/").some((p) => p === ".." || p === "") || /[:*?"<>|]/.test(t)) fail("E_INVALID_PARAM", `名前 '${s}' は使えない(.. や空の区切り、記号は不可)`);
  return t;
}
export const baselinePng = (projectDir: string, scene: string, name: string) => path.join(baselinesDir(projectDir), safeSeg(scene), `${safeSeg(name)}.png`);
export function readBaselines(projectDir: string): BaselinesFile {
  return readJson<BaselinesFile>(path.join(baselinesDir(projectDir), "baselines.json")) ?? { version: 1, baselines: {} };
}
export function writeBaselines(projectDir: string, f: BaselinesFile) { writeJsonAtomic(path.join(baselinesDir(projectDir), "baselines.json"), f); }
export const historyPath = (projectDir: string) => path.join(baselinesDir(projectDir), "history.jsonl");

/** adapter が違えば比べない。ほかの差(ドライバ・エンジン版・設定ハッシュ・プリセット)は警告として返す。 */
export function keyCompat(base: BaselineKey, cur: BaselineKey): { comparable: boolean; reason?: string; changes: string[] } {
  if (base.adapter && cur.adapter && base.adapter !== cur.adapter) return { comparable: false, reason: `adapter が違う(基準 ${base.adapter} / 今回 ${cur.adapter})`, changes: [] };
  const changes: string[] = [];
  for (const k of ["driverVersion", "engineVersion", "renderSettingsHash", "qualityPreset"] as const) if (base[k] !== cur[k]) changes.push(`${k}: ${base[k] ?? "-"} → ${cur[k] ?? "-"}`);
  return { comparable: true, changes };
}

/** いまのエンジンの条件(基準のキー)。取れない項目は null。 */
export async function readBaselineKey(call: EngineCall): Promise<BaselineKey> {
  const ping = await call("ping", {}, { timeout: 8000 }).catch(() => ({} as any));
  let qualityPreset: string | null = null;
  try { const q = await call("get_quality_preset", {}, { timeout: 5000 }); qualityPreset = q?.preset ?? q?.name ?? null; } catch { /* まだ無いエンジン */ }
  const parts: unknown[] = [];
  for (const m of ["get_taa", "get_upscaler", "get_render_scale", "get_post_process", "get_ssao", "get_ssr", "get_ssgi"]) { try { parts.push([m, await call(m, {}, { timeout: 5000 })]); } catch { /* 無いものは飛ばす */ } }
  const hash = parts.length ? crypto.createHash("sha1").update(JSON.stringify(parts)).digest("hex").slice(0, 12) : null;
  return {
    adapter: ping?.gpu?.name ?? ping?.adapter ?? null, driverVersion: ping?.gpu?.driver ?? null,
    engineVersion: ping?.engineVersion ?? null, renderSettingsHash: hash, qualityPreset,
  };
}

export type ProjectItemResult = {
  key: string; scene: string; name: string; status: "pass" | "fail" | "skipped" | "missing";
  skippedReason?: string; keyChanges?: string[]; needsHumanReview?: boolean; flip?: ReturnType<typeof flipMean>;
} & Partial<CompareResult>;

export async function compareProjectItem(o: {
  lib: DiffLib; call: EngineCall; projectDir: string; rec: ProjectBaseline; curKey: BaselineKey; tol: Tolerance; outDir: string; repoDir: string; metric?: "flip";
}): Promise<ProjectItemResult> {
  const { rec } = o;
  const key = baselineKeyOf(rec.scene, rec.name);
  const compat = keyCompat(rec.key, o.curKey);
  const base: ProjectItemResult = { key, scene: rec.scene, name: rec.name, status: "pass", ...(compat.changes.length ? { keyChanges: compat.changes } : {}) };
  if (!compat.comparable) return { ...base, status: "skipped", skippedReason: compat.reason };
  const refFile = baselinePng(o.projectDir, rec.scene, rec.name);
  if (!fs.existsSync(refFile)) return { ...base, status: "missing", skippedReason: `基準画像が無い: ${path.relative(o.projectDir, refFile)}` };
  await openSceneWait(o.call, rec.scene);
  const cap = await captureFinal(o.call, path.join(o.outDir, `${safeSeg(rec.name).replace(/\//g, "__")}.native.png`), { camera: rec.camera, warmupFrames: rec.warmupFrames ?? 60, settleFrames: rec.settleFrames ?? 8 });
  const ref = readPng(refFile);
  if (ref.width !== cap.img.width || ref.height !== cap.img.height) {
    return { ...base, status: "skipped", skippedReason: `解像度が違う(基準 ${ref.width}x${ref.height} / 今回 ${cap.img.width}x${cap.img.height})。同じ表示サイズで撮り直す` };
  }
  const res = compareImagesPair(o.lib, ref, cap.img, o.tol, o.lib.sha256(cap.img) === rec.nativeSha256);
  const out: ProjectItemResult = { ...base, ...res, status: res.pass ? "pass" : "fail", needsHumanReview: rec.approvedBy === "ai" && !rec.humanReviewed };
  if (!res.pass || res.bitExact === false) Object.assign(out, writeEvidence(o.outDir, safeSeg(rec.name).replace(/\//g, "__"), ref, cap.img));
  if (o.metric === "flip") out.flip = flipMean(o.repoDir, refFile, cap.file);
  return out;
}

/** 基準を書く(approve)。reason 必須・履歴に追記・approvedBy は ai。 */
export async function approveProjectItem(o: {
  lib: DiffLib; call: EngineCall; projectDir: string; scene: string; name: string; reason: string | undefined;
  camera?: { position: number[]; target: number[] }; warmupFrames?: number; settleFrames?: number; approvedBy?: "ai" | "human";
}): Promise<ProjectBaseline> {
  if (!o.reason || !o.reason.trim()) fail("E_MISSING_PARAM", "approve には reason が要る(何が変わったから基準を更新するのか)", { fix: [{ tool: "dx12_visual", args: { op: "approve", scene: o.scene, name: o.name, reason: "<理由>" }, why: "理由を付けて撃ち直す" }] });
  const existing = readBaselines(o.projectDir).baselines[baselineKeyOf(o.scene, o.name)];
  const spec: CaptureSpec = { camera: o.camera ?? existing?.camera, warmupFrames: o.warmupFrames ?? existing?.warmupFrames ?? 60, settleFrames: o.settleFrames ?? existing?.settleFrames ?? 8 };
  await openSceneWait(o.call, o.scene);
  const tmp = path.join(baselinesDir(o.projectDir), "_tmp", `${crypto.randomBytes(4).toString("hex")}.png`);
  fs.mkdirSync(path.dirname(tmp), { recursive: true });
  const cap = await captureFinal(o.call, tmp, spec);
  const png = baselinePng(o.projectDir, o.scene, o.name);
  fs.mkdirSync(path.dirname(png), { recursive: true });
  fs.copyFileSync(cap.file, png);
  try { fs.rmSync(tmp, { force: true }); } catch { /* 無視 */ }
  const rec: ProjectBaseline = {
    scene: o.scene, name: o.name, key: await readBaselineKey(o.call), nativeSha256: o.lib.sha256(cap.img), width: cap.img.width, height: cap.img.height,
    ...(spec.camera ? { camera: spec.camera } : {}), warmupFrames: spec.warmupFrames, settleFrames: spec.settleFrames,
    approvedAt: new Date().toISOString(), approvedBy: o.approvedBy ?? "ai", reason: o.reason.trim(), humanReviewed: (o.approvedBy ?? "ai") === "human",
  };
  const f = readBaselines(o.projectDir);
  const k = baselineKeyOf(o.scene, o.name);
  const prevSha = f.baselines[k]?.nativeSha256 ?? null;
  f.baselines[k] = rec;
  writeBaselines(o.projectDir, f);
  appendJsonl(historyPath(o.projectDir), { at: rec.approvedAt, key: k, approvedBy: rec.approvedBy, reason: rec.reason, previousSha256: prevSha, nativeSha256: rec.nativeSha256, adapter: rec.key.adapter });
  return rec;
}

export const readHistoryLog = (projectDir: string, max = 50) => readJsonl(historyPath(projectDir), 5000).slice(-max);

/** approve 直後(AI が基準を更新して、人がまだ見ていない)の基準があるか。quality_gate が needsHumanReview にするのに使う。 */
export function pendingHumanReview(projectDir: string): string[] {
  return Object.entries(readBaselines(projectDir).baselines).filter(([, b]) => b.approvedBy === "ai" && !b.humanReviewed).map(([k]) => k);
}

export { StepError, errBody, round };
