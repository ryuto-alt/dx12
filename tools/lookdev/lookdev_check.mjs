// MH-1 ルックデブ検証ハーネスの本体（tools/lookdev_check.ps1 から呼ぶ）。
//
// 何をするか:
//   tests/golden/entries/lookdev_*.json の各シーンを、shadingRevision = 1（V1。既定）と 2（UNO_SHADING_V2）の両方で
//   決定論スクショ（screenshot_final deterministic:true）に撮り、次を 1 つの JSON にして出す。
//     - 各リビジョンの画素ハッシュ・決定論（同じセッションで複数回撮って 1 ビットも違わないか）・輝度の統計
//     - V1 と V2 の差（差画素の割合・平均絶対差・最大距離・bitExact）。全機能 OFF の V2 は V1 と同じ絵のはず（docs/SHADING_V2.md）
//     - GPU の時間（perf_stats の gpuPassMs.mainScene。窓を取って平均）。★合否には使わない（性能の合否は夜間の計測窓。P.0）
//     - 任意: FLIP（tools/parity。venv があるときだけ。V1 を基準に V2 を比べる、または --reference-dir の画像を基準にする）
//   終了コード: 0 = 合格 / 1 = 不合格（決定論の崩れ・V2 の逸脱・撮影失敗）/ 2 = 起動や引数のエラー。
//
// 窓もカーソルも使わない: エンジンは tools/engine_instance.ps1 経由の --headless。止めるのは自分の PID だけ。
//
// 使い方:
//   node tools/lookdev/lookdev_check.mjs [--only a,b] [--out DIR] [--exe PATH] [--port N] [--revisions 1,2]
//        [--perf-window 120] [--repeat 2] [--v2-max-mean-abs 0.25] [--v2-max-diff-ratio 0.001] [--v2-record-only]
//        [--reference-dir DIR] [--flip-python PATH] [--no-save-images]
//        [--pt-reference] [--pt-spp 1024] [--pt-flip-max 0.08] [--pt-max-seconds 120]
//   --pt-reference: 各シーンをエンジンのパストレーサー（render_reference）で描いて <out>/<シーン>.pt.png / .pt.pfm を作り、
//     最後のリビジョン（既定 V2）と FLIP（flip_ldr_mean）を比べる。1 シーン 20〜40 秒の GPU を食うので、離席中に回す（既定は切）。
//     FLIP が --pt-flip-max を超えたら不合格（FLIP を計算できないときは不合格にせず skipped と出す）。
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, findDefaultExe, resolveProject, launchEngine, connect, sleep } from "../bench/lib/engine.mjs";
import { sha256, compareImages } from "../bench/lib/imageDiff.mjs";
import { loadConfig, readPng, writePng } from "../bench/golden.mjs";

export const SCHEMA = "lookdev-check/1";

export function parseLookdevArgs(argv, env = process.env) {
  const a = {
    only: null, exe: findDefaultExe(), port: Number(env.LOOKDEV_PORT) || 0,
    out: path.join(env.USERPROFILE ?? env.HOME ?? ".", "dx12-scorecard", "lookdev"),
    revisions: [1, 2], perfWindow: 120, repeat: 2,
    v2MaxMeanAbs: 0.25, v2MaxDiffRatio: 0.001, v2RecordOnly: false,
    referenceDir: null, flipPython: null, saveImages: true, help: false,
    ptReference: false, ptSpp: 1024, ptFlipMax: 0.08, ptMaxSeconds: 120,
  };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const val = () => { if (i + 1 >= argv.length) throw new Error(`${k} に値が要る`); return argv[++i]; };
    if (k === "--only") a.only = new Set(val().split(",").map((s) => s.trim()).filter(Boolean));
    else if (k === "--exe") a.exe = path.resolve(val());
    else if (k === "--port") a.port = Number(val());
    else if (k === "--out") a.out = path.resolve(val());
    else if (k === "--revisions") a.revisions = val().split(",").map(Number);
    else if (k === "--perf-window") a.perfWindow = Math.max(1, Number(val()) || 120);
    else if (k === "--repeat") a.repeat = Math.max(1, Number(val()) || 2);
    else if (k === "--v2-max-mean-abs") a.v2MaxMeanAbs = Number(val());
    else if (k === "--v2-max-diff-ratio") a.v2MaxDiffRatio = Number(val());
    else if (k === "--v2-record-only") a.v2RecordOnly = true;
    else if (k === "--reference-dir") a.referenceDir = path.resolve(val());
    else if (k === "--flip-python") a.flipPython = path.resolve(val());
    else if (k === "--no-save-images") a.saveImages = false;
    else if (k === "--pt-reference") a.ptReference = true;
    else if (k === "--pt-spp") a.ptSpp = Math.max(1, Number(val()) || 1024);
    else if (k === "--pt-flip-max") a.ptFlipMax = Number(val());
    else if (k === "--pt-max-seconds") a.ptMaxSeconds = Math.max(1, Number(val()) || 120);
    else if (k === "--help" || k === "-h") a.help = true;
    else throw new Error(`不明な引数: ${k}`);
  }
  if (a.ptReference && !a.saveImages) throw new Error("--pt-reference には画像の保存が要る（--no-save-images と併用できない）");
  if (!a.revisions.length || a.revisions.some((r) => r !== 1 && r !== 2)) throw new Error("--revisions は 1 と 2 だけ（例 1,2）");
  return a;
}

/** 輝度の統計（sRGB のまま Rec.709 の係数。ここでの値は「V1 と V2 を比べる物差し」で、絶対値の測光ではない）。 */
export function lumaStats(img) {
  const n = img.width * img.height;
  let sum = 0, max = 0, clipped = 0, dark = 0;
  for (let p = 0; p < n; p++) {
    const i = p * 4;
    const l = (img.data[i] * 0.2126 + img.data[i + 1] * 0.7152 + img.data[i + 2] * 0.0722) / 255;
    sum += l; if (l > max) max = l;
    if (img.data[i] >= 254 && img.data[i + 1] >= 254 && img.data[i + 2] >= 254) clipped++;
    if (l < 0.02) dark++;
  }
  return { mean: sum / n, max, clippedRatio: clipped / n, darkRatio: dark / n };
}

/** V2 の判定（純関数）。cmp は compareImages の結果。 */
export function judgeV2(cmp, a) {
  if (a.v2RecordOnly) return { pass: true, reasons: [] };
  const reasons = [];
  if (cmp.diffRatio > a.v2MaxDiffRatio) reasons.push(`V2 が V1 と違う画素 ${(cmp.diffRatio * 100).toFixed(3)}% > 上限 ${(a.v2MaxDiffRatio * 100).toFixed(3)}%`);
  if (cmp.meanAbs > a.v2MaxMeanAbs) reasons.push(`V2 と V1 の平均絶対差 ${cmp.meanAbs.toFixed(3)} > 上限 ${a.v2MaxMeanAbs}`);
  return { pass: reasons.length === 0, reasons };
}

function gitHead() {
  const r = spawnSync("git", ["rev-parse", "--short", "HEAD"], { cwd: REPO_ROOT, encoding: "utf8" });
  return r.status === 0 ? r.stdout.trim() : "unknown";
}

async function openScene(eng, scene) {
  for (let i = 0; i < 600; i++) {
    const p = await eng.call("ping");
    if (p.currentScene === scene && !p.sceneLoading) return p;
    try { await eng.call("open_scene", { path: scene }, 300_000); }
    catch (e) { if (!/already in progress/.test(e.message)) throw e; }
    await sleep(500);
  }
  throw new Error(`シーンが開かない: ${scene}`);
}

function findFlipPython(a) {
  const cands = [a.flipPython, process.env.LOOKDEV_FLIP_PYTHON,
    path.join(REPO_ROOT, "tools", "parity", ".venv", "Scripts", "python.exe")].filter(Boolean);
  return cands.find((c) => fs.existsSync(c)) ?? null;
}

/** FLIP の結果から flip_ldr_mean を取り出す（純関数）。無ければ null。 */
export function flipMean(f) {
  const v = f?.metrics?.flip_ldr_mean;
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

/** PT 基準の判定（純関数）。FLIP が計算できていないときは合否に使わない。 */
export function judgePtFlip(f, max) {
  const v = flipMean(f);
  if (v === null) return { pass: true, reasons: [] };
  return v <= max ? { pass: true, reasons: [] } : { pass: false, reasons: [`PT 基準との FLIP ${v.toFixed(4)} > 上限 ${max}`] };
}

/** エンジンのパストレーサーで基準画像を作る。出力は <base>.pfm / .png。 */
async function renderPtReference(conn, a, base, size) {
  await conn.call("render_reference", {
    spp: a.ptSpp, bounces: 8, size, seed: 1, output: base, formats: ["pfm", "png"], frameBudgetMs: 40, maxSeconds: a.ptMaxSeconds,
  }, 60_000);
  const t0 = Date.now();
  for (;;) {
    const st = await conn.call("render_reference_status", {}, 30_000);
    if (st.state === "done") return st;
    if (st.state === "failed" || st.state === "cancelled") throw new Error(`PT 基準が ${st.state}: ${st.error ?? ""}`);
    if (Date.now() - t0 > (a.ptMaxSeconds + 120) * 1000) throw new Error("PT 基準が時間切れ");
    await sleep(1000);
  }
}

/** tools/parity の compare で FLIP を出す。python / flip-evaluator が無ければ null（合否には使わない）。 */
function runFlip(py, refPng, testPng, outDir) {
  if (!py) return null;
  const parityDir = path.join(REPO_ROOT, "tools", "parity");
  const r = spawnSync(py, ["-m", "parity", "compare", "--metrics", "flip", "--json", "--out", outDir, refPng, testPng],
    { cwd: parityDir, encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
  if (r.status !== 0 && r.status !== 1) return { error: (r.stderr || r.stdout || "").trim().slice(0, 300) };
  try { const j = JSON.parse(r.stdout); return { metrics: j.metrics ?? j }; } catch { return { error: "JSON を読めない" }; }
}

export async function runLookdev(a) {
  const entries = loadConfig().filter((s) => s.name.startsWith("lookdev_") && (!a.only || a.only.has(s.name)));
  if (!entries.length) throw new Error("lookdev_* のゴールデンの登録が無い（tests/golden/entries/）");
  const project = resolveProject(entries[0].project);
  if (!project) throw new Error(`プロジェクトが見つからない: ${entries[0].project}`);
  fs.mkdirSync(a.out, { recursive: true });
  const instName = `lookdev-${createHash("sha1").update(REPO_ROOT).digest("hex").slice(0, 8)}`;
  const report = {
    schema: SCHEMA, commit: gitHead(), exe: path.relative(REPO_ROOT, a.exe), capturedAt: new Date().toISOString(),
    thresholds: { v2MaxMeanAbs: a.v2MaxMeanAbs, v2MaxDiffRatio: a.v2MaxDiffRatio, v2RecordOnly: a.v2RecordOnly, ptReference: a.ptReference, ptFlipMax: a.ptFlipMax },
    // 白い炉（furnace）の統計はここには無い。MH-2 の受け入れ（CPU テストの方向アルベド）で見る。docs/GOLDEN_TESTING.md の「ルックデブ」参照
    scenes: [], pass: true, reasons: [],
  };
  const eng = await launchEngine({ exe: a.exe, project, port: a.port || undefined, name: instName });
  let conn;
  const py = findFlipPython(a);
  try {
    conn = await connect(eng.port);
    for (let i = 0; i < 240; i++) {
      try { await conn.call("ping"); break; } catch (e) { if (i === 239) throw e; await sleep(500); }
    }
    for (const s of entries) {
      const rec = { name: s.name, scene: s.scene, revisions: {}, pass: true, reasons: [] };
      report.scenes.push(rec);
      try {
        await openScene(conn, s.scene);
        const imgs = {};
        for (const rev of a.revisions) {
          await conn.call("set_shading", { shadingRevision: rev });
          await conn.call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
          await conn.call("step_frames", { frames: Math.max(s.warmupFrames ?? 60, 60) }, 300_000);   // PSO の切替は次フレーム先頭。温めてから撮る
          const shots = [];
          for (let k = 0; k < a.repeat; k++) {
            const file = path.join(a.out, `${s.name}.v${rev}.${k}.native.png`);
            const r = await conn.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: s.settleFrames ?? 8, path: file }, 300_000);
            shots.push(readPng(r.path));
            if (!a.saveImages || k > 0) fs.rmSync(r.path, { force: true });
          }
          const sha = sha256(shots[0]);
          const deterministic = shots.every((x) => sha256(x) === sha);
          // 性能: 撮影で止まっていた分を流してから窓を取る（合否には使わない）
          await conn.call("step_frames", { frames: a.perfWindow + 10 }, 300_000);
          let perf = null;
          try {
            const p = await conn.call("perf_stats", { window: a.perfWindow });
            perf = { mainSceneMs: p.gpuPassMs?.mainScene ?? null, totalGpuMs: p.gpuPassMs?.total ?? null, frameMs: p.frameMs ?? null, window: a.perfWindow };
          } catch (e) { perf = { error: e.message }; }
          imgs[rev] = shots[0];
          rec.revisions[rev] = { native: `${shots[0].width}x${shots[0].height}`, sha256: sha, deterministic, luma: lumaStats(shots[0]), perf };
          if (!deterministic) {
            rec.pass = false;
            rec.reasons.push(`V${rev}: 同じセッションで撮り直した ${a.repeat} 枚が一致しない（決定論の崩れ）`);
          }
        }
        if (imgs[1] && imgs[2]) {
          const cmp = compareImages(imgs[1], imgs[2], 12);
          rec.v1vs2 = { diffRatio: cmp.diffRatio, meanAbs: cmp.meanAbs, maxDist: cmp.maxDist, bitExact: rec.revisions[1].sha256 === rec.revisions[2].sha256 };
          if (a.saveImages) writePng(path.join(a.out, `${s.name}.v1vs2.diff.png`), cmp.diffImage);
          const j = judgeV2(cmp, a);
          if (!j.pass) { rec.pass = false; rec.reasons.push(...j.reasons); }
          if (a.saveImages) {
            const f = runFlip(py, path.join(a.out, `${s.name}.v1.0.native.png`), path.join(a.out, `${s.name}.v2.0.native.png`), path.join(a.out, "flip", `${s.name}.v1vs2`));
            rec.flipV1vsV2 = f ?? { skipped: "tools/parity の venv が無い（pwsh tools/parity/setup.ps1）" };
          }
        }
        // 基準画像との FLIP は、計画のしきい値（FLIP <= 0.08）が掛かる最後のリビジョン（既定 V2）で比べる
        const lastRev = a.revisions[a.revisions.length - 1];
        const testFile = path.join(a.out, `${s.name}.v${lastRev}.0.native.png`);
        let refFile = null;
        if (a.ptReference && imgs[lastRev]) {
          const base = path.join(a.out, `${s.name}.pt`);
          await conn.call("set_shading", { shadingRevision: lastRev });
          await conn.call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
          await conn.call("step_frames", { frames: 4 }, 60_000);
          const st = await renderPtReference(conn, a, base, [imgs[lastRev].width, imgs[lastRev].height]);
          rec.ptReference = { spp: st.output?.sppDone ?? a.ptSpp, files: st.output?.files ?? [], truncated: st.output?.truncated ?? false };
          refFile = `${base}.png`;
        } else if (a.referenceDir) {
          refFile = path.join(a.referenceDir, `${s.name}.png`);
        }
        if (refFile && a.saveImages) {
          const ptFlip = fs.existsSync(refFile)
            ? (runFlip(py, refFile, testFile, path.join(a.out, "flip", `${s.name}.vs_ref`)) ?? { skipped: "tools/parity の venv が無い（pwsh tools/parity/setup.ps1）" })
            : { skipped: `基準画像が無い: ${refFile}` };
          rec.flipReference = { revision: lastRev, reference: path.basename(refFile), ...ptFlip };
          if (a.ptReference) {
            const j = judgePtFlip(ptFlip, a.ptFlipMax);
            if (!j.pass) { rec.pass = false; rec.reasons.push(...j.reasons); }
          }
        }
      } catch (e) {
        rec.pass = false;
        rec.reasons.push(`撮影失敗: ${e.message}`);
      }
      if (!rec.pass) report.pass = false;
    }
  } finally {
    try { conn?.close(); } catch { /* 既に切れている */ }
    eng.kill();
  }
  report.reasons = report.scenes.flatMap((s) => s.reasons.map((r) => `${s.name}: ${r}`));
  const file = path.join(a.out, "lookdev_check.json");
  fs.writeFileSync(file, JSON.stringify(report, null, 2) + "\n");
  report.file = file;
  return report;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  let a;
  try { a = parseLookdevArgs(process.argv.slice(2)); }
  catch (e) { console.error(e.message); process.exit(2); }
  if (!a.help && !a.port) {
    console.error("ポートが要る（--port か環境変数 LOOKDEV_PORT。worktree_setup.ps1 が表示した自分のレーンのもの）。他レーンのポートを取らないため、空きの自動選択はしない");
    process.exit(2);
  }
  if (a.help) { console.log("node tools/lookdev/lookdev_check.mjs [--only a,b] [--out DIR] [--exe PATH] [--port N] [--revisions 1,2] [--v2-record-only] [--pt-reference] ..."); process.exit(0); }
  runLookdev(a).then((r) => {
    for (const s of r.scenes) {
      const v = s.revisions;
      const bits = [`${s.pass ? "ok  " : "FAIL"} ${s.name.padEnd(24)}`];
      for (const k of Object.keys(v)) bits.push(`V${k}: mainScene=${v[k].perf?.mainSceneMs ?? "?"}ms det=${v[k].deterministic}`);
      if (s.v1vs2) bits.push(`V1/V2: 差画素=${(s.v1vs2.diffRatio * 100).toFixed(3)}% 平均差=${s.v1vs2.meanAbs.toFixed(3)} bitExact=${s.v1vs2.bitExact}`);
      console.log(bits.join("  "));
      for (const x of s.reasons) console.log(`       ${x}`);
    }
    console.log(`\n結果 JSON: ${r.file}\n${r.pass ? "合格" : "不合格"}`);
    process.exit(r.pass ? 0 : 1);
  }, (e) => { console.error(e.stack ?? e.message); process.exit(2); });
}
