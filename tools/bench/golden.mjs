// 描画の回帰テスト（ゴールデン画像）。
//
// なぜ要るか:
//   Render() の分割・View 化・知覚の ID パスなど、描画まわりの大きな改修をこれから入れる。
//   「絵が 1 ビットも変わっていない」「変わったのは狙った所だけ」を人の目ではなく機械で言えないと、
//   改修のたびにエディタを開いて見比べることになる。決定論スクショ（screenshot_final の
//   deterministic:true）は既にあるので、固定シーン × 固定カメラで撮って基準と比べるだけ。
//
// 使い方:
//   node tools/bench/golden.mjs                 基準と比べる（終了コード 0 = 全部合格 / 1 = どれか不合格）
//   node tools/bench/golden.mjs --update        基準を撮り直す（tests/golden/*.png と baseline.json）
//   node tools/bench/golden.mjs --repeat 2      同じシーンを 2 回撮って決定論も確かめる
//   node tools/bench/golden.mjs --only a,b      名前で絞る
//   node tools/bench/golden.mjs --strict        プロジェクトが見つからないシーンを SKIP ではなく不合格にする
//   node tools/bench/golden.mjs --exe <path>    別の exe を撮る（既定: 環境変数 DX12_ENGINE_EXE → build/dev → build/release の順で最初にあるもの）
//
// 設定は tests/golden/golden.json。出力（撮った絵・差分画像）は build/golden-out/。
//
// ★比べ方は 2 段:
//   1) ネイティブ解像度（ヘッドレスの表示矩形そのもの）の画素 SHA-256 が基準と同じか → bitExact
//   2) width x height へ面積平均で縮めた絵を基準 PNG と画素ごとに比べる → 許容差で合否
//   合否は 2) で決める（ドライバ更新などで 1 LSB 揺れても落とさないため）。1) は
//   「本当に 1 ビットも変わっていないか」を見たいとき（知覚パスを足した前後など）の証拠。
// ★シーンの project はリポジトリ外（開発者の手元のゲーム）を指してよい。見つからないシーンは SKIP にする
//   （--strict で不合格）。別の PC では環境変数 DX12_GOLDEN_PROJECTS_ROOT に「PerfTest/Nocturne/Junction の親フォルダ」を
//   渡すと、project の末尾のフォルダ名をそこから探す。リポジトリ内の相対パス（tests/... 等）も使える。
// ★エンジンはヘッドレスで build/dev の exe を別ポート（8850〜）で起動する。人のエディタ（8787〜）には繋がない。
//   止めるときは自分の PID だけを落とす。
import fs from "node:fs";
import path from "node:path";
import { execSync } from "node:child_process";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, findDefaultExe, resolveProject, launchEngine, connect, sleep } from "./lib/engine.mjs";
import { sha256, resampleArea, compareImages, judge } from "./lib/imageDiff.mjs";

export { sha256, resampleArea, compareImages, judge };

const require = createRequire(import.meta.url);
const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));

const GOLDEN_DIR = path.join(REPO_ROOT, "tests", "golden");
const CONFIG = path.join(GOLDEN_DIR, "golden.json");
const BASELINE = path.join(GOLDEN_DIR, "baseline.json");
const OUT_DIR = path.join(REPO_ROOT, "build", "golden-out");

// ─── 引数 ───────────────────────────────────────────────────────────────
function parseArgs(argv) {
  const a = { update: false, repeat: 1, only: null, exe: findDefaultExe(), strict: false };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === "--update") a.update = true;
    else if (k === "--repeat") a.repeat = Math.max(1, Number(argv[++i]) || 1);
    else if (k === "--only") a.only = new Set(String(argv[++i]).split(",").map((s) => s.trim()).filter(Boolean));
    else if (k === "--exe") a.exe = path.resolve(argv[++i]);
    else if (k === "--strict") a.strict = true;
    else if (k === "--help" || k === "-h") { a.help = true; }
    else throw new Error(`不明な引数: ${k}`);
  }
  return a;
}

// ─── 画像ユーティリティ（純関数） ───────────────────────────────────────
export function readPng(file) {
  const png = PNG.sync.read(fs.readFileSync(file));
  return { width: png.width, height: png.height, data: png.data };   // RGBA
}
export function writePng(file, img) {
  const png = new PNG({ width: img.width, height: img.height });
  img.data.copy ? img.data.copy(png.data) : png.data.set(img.data);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, PNG.sync.write(png));
}

// ─── 設定 ───────────────────────────────────────────────────────────────
function loadConfig() {
  const cfg = JSON.parse(fs.readFileSync(CONFIG, "utf8"));
  const d = cfg.defaults ?? {};
  return cfg.scenes.map((s) => ({
    width: d.width ?? 640, height: d.height ?? 360,
    settleFrames: d.settleFrames ?? 8, warmupFrames: d.warmupFrames ?? 60,
    ...s,
    tolerance: { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25, ...(d.tolerance ?? {}), ...(s.tolerance ?? {}) },
  }));
}

function gitHead() {
  try { return execSync("git rev-parse --short HEAD", { cwd: REPO_ROOT }).toString().trim(); }
  catch { return "unknown"; }
}

// ─── 撮影 ───────────────────────────────────────────────────────────────
async function openScene(eng, scene) {
  // ★起動直後は --scene のロードが走っている最中で、そこへ open_scene を撃つと
  //   「a scene load is already in progress」で弾かれる。一致するまで待ち、弾かれたら待って撃ち直す。
  for (let i = 0; i < 600; i++) {
    const p = await eng.call("ping");
    if (p.currentScene === scene) return p;
    try {
      await eng.call("open_scene", { path: scene }, 300_000);
    } catch (e) {
      if (!/already in progress/.test(e.message)) throw e;
    }
    await sleep(500);
  }
  throw new Error(`シーンが開かない: ${scene}`);
}

/** 1 シーンを決定論モードで撮る。戻り値はネイティブ解像度の画像と保存先。 */
async function captureScene(eng, s, tag) {
  await eng.call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
  // ロード直後はテクスチャの非同期読み込みや自動露出が落ち着いていないので、先に数十フレーム回す
  if (s.warmupFrames > 0) await eng.call("step_frames", { frames: s.warmupFrames }, 300_000);
  const file = path.join(OUT_DIR, `${s.name}${tag}.native.png`);
  const r = await eng.call("screenshot_final",
    { deterministic: true, gizmos: false, settleFrames: s.settleFrames, path: file }, 300_000);
  return { img: readPng(r.path), file: r.path, meta: r };
}

// ─── 本体 ───────────────────────────────────────────────────────────────
async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) {
    console.log("node tools/bench/golden.mjs [--update] [--repeat N] [--only a,b] [--exe path]");
    return 0;
  }
  let scenes = loadConfig();
  if (args.only) scenes = scenes.filter((s) => args.only.has(s.name));
  if (!scenes.length) { console.log("対象のシーンが無い"); return 1; }
  fs.mkdirSync(OUT_DIR, { recursive: true });

  const baseline = fs.existsSync(BASELINE) ? JSON.parse(fs.readFileSync(BASELINE, "utf8")) : { scenes: {} };
  const head = gitHead();
  console.log(`${args.update ? "基準を撮り直す" : "基準と比べる"}: ${scenes.length} シーン / exe=${path.relative(REPO_ROOT, args.exe)} / HEAD=${head}`);

  // プロジェクトごとに 1 回だけ起動する（シーンは open_scene で切り替える）
  const byProject = new Map();
  for (const s of scenes) {
    if (!byProject.has(s.project)) byProject.set(s.project, []);
    byProject.get(s.project).push(s);
  }

  const results = [];
  for (const [projectCfg, list] of byProject) {
    const project = resolveProject(projectCfg);
    if (!project) {
      for (const s of list) {
        results.push({ name: s.name, pass: args.strict ? false : true, skipped: !args.strict,
          reasons: [`プロジェクトが見つからない: ${projectCfg}（DX12_GOLDEN_PROJECTS_ROOT で親フォルダを指定できる）`] });
      }
      continue;
    }
    const t0 = Date.now();
    const eng = await launchEngine({ exe: args.exe, project, scene: list[0].scene });
    let conn;
    try {
      conn = await connect(eng.port);
      for (const s of list) {
        const r = { name: s.name, pass: true, reasons: [] };
        results.push(r);
        try {
          await openScene(conn, s.scene);
          const shots = [];
          for (let k = 0; k < args.repeat; k++)
            shots.push(await captureScene(conn, s, args.repeat > 1 ? `.${k}` : ""));
          const first = shots[0];
          r.native = `${first.img.width}x${first.img.height}`;
          r.sha = sha256(first.img);

          // 決定論: 同じセッションで撮り直しても 1 ビットも変わらないか
          if (shots.length > 1) {
            r.deterministic = shots.every((x) => sha256(x.img) === r.sha);
            if (!r.deterministic) {
              const c = compareImages(first.img, shots.at(-1).img, s.tolerance.pixelThreshold);
              r.determinismDiff = { diffRatio: c.diffRatio, meanAbs: c.meanAbs, maxDist: c.maxDist };
              writePng(path.join(OUT_DIR, `${s.name}.repeat-diff.png`), c.diffImage);
            }
          }

          const small = resampleArea(first.img, s.width, s.height);
          const refFile = path.join(GOLDEN_DIR, `${s.name}.png`);
          if (args.update) {
            writePng(refFile, small);
            baseline.scenes[s.name] = {
              nativeWidth: first.img.width, nativeHeight: first.img.height, nativeSha256: r.sha,
              compareSha256: sha256(small), width: s.width, height: s.height,
              commit: head, capturedAt: new Date().toISOString(),
              deterministicInSession: r.deterministic ?? null,
            };
            r.updated = true;
          } else {
            writePng(path.join(OUT_DIR, `${s.name}.png`), small);
            const b0 = baseline.scenes[s.name];
            if (b0 && (b0.nativeWidth !== first.img.width || b0.nativeHeight !== first.img.height)) {
              // ★ネイティブ解像度が違う絵は比べても意味がない（AO/影などが解像度に依存する）。
              //   画の回帰ではなく撮影環境の違いなので、FAIL にせず「環境違い」で打ち切る（--strict では不合格）。
              r.pass = !args.strict; r.skipped = !args.strict; r.envMismatch = true;
              r.reasons.push(`環境違い: ネイティブ解像度が基準と違う（基準 ${b0.nativeWidth}x${b0.nativeHeight}、今回 ${first.img.width}x${first.img.height}）。` +
                             `基準を撮った環境（build/dev と imgui.ini）に揃えるか、この環境で --update して基準を撮り直す`);
            } else if (!fs.existsSync(refFile)) {
              r.pass = false;
              r.reasons.push("基準画像が無い（--update で撮る）");
            } else {
              const ref = readPng(refFile);
              const cmp = compareImages(ref, small, s.tolerance.pixelThreshold);
              writePng(path.join(OUT_DIR, `${s.name}.diff.png`), cmp.diffImage);
              r.diffRatio = cmp.diffRatio; r.meanAbs = cmp.meanAbs; r.maxDist = cmp.maxDist;
              const j = judge(cmp, s.tolerance);
              r.pass = j.pass; r.reasons.push(...j.reasons);
            }
            const b = baseline.scenes[s.name];
            if (b) {
              r.bitExact = b.nativeSha256 === r.sha;
              if (b.nativeWidth !== first.img.width || b.nativeHeight !== first.img.height)
                r.reasons.push(`注意: ネイティブ解像度が基準と違う（基準 ${b.nativeWidth}x${b.nativeHeight}）。` +
                               `build/dev/imgui.ini やウィンドウの DPI が変わっていないか`);
            }
          }
        } catch (e) {
          r.pass = false;
          r.reasons.push(`撮影失敗: ${e.message}`);
        }
      }
    } finally {
      try { conn?.close(); } catch { /* 既に切れている */ }
      eng.kill();
    }
    console.log(`  (${path.basename(project)}: ${((Date.now() - t0) / 1000).toFixed(1)}s)`);
  }

  if (args.update) {
    fs.writeFileSync(BASELINE, JSON.stringify(baseline, null, 2) + "\n");
  }

  console.log("");
  for (const r of results) {
    const head = r.skipped ? "SKIP" : r.updated ? "UPD " : r.pass ? "ok  " : "FAIL";
    const parts = [`${head} ${r.name.padEnd(24)} native=${r.native ?? "?"}`];
    if (r.diffRatio != null) parts.push(`差画素=${(r.diffRatio * 100).toFixed(3)}% 平均差=${r.meanAbs.toFixed(3)} 最大距離=${r.maxDist.toFixed(1)}`);
    if (r.bitExact != null) parts.push(`bitExact=${r.bitExact}`);
    if (r.deterministic != null) parts.push(`決定論=${r.deterministic ? "一致" : `不一致(差画素 ${(r.determinismDiff.diffRatio * 100).toFixed(3)}% 平均差 ${r.determinismDiff.meanAbs.toFixed(3)})`}`);
    console.log(parts.join("  "));
    for (const x of r.reasons) console.log(`       ${x}`);
  }
  const failed = results.filter((r) => !r.pass).length;
  const skipped = results.filter((r) => r.skipped).length;
  if (skipped === results.length) console.log("\n全シーンが SKIP（撮れたものが 0 件）。合格とは言えないので注意。");
  console.log(`\n${results.length} シーン中 ${failed} 件が不合格${skipped ? `（${skipped} 件 SKIP）` : ""}${args.update ? "（--update なので基準を書き換えた）" : ""}。出力: ${path.relative(REPO_ROOT, OUT_DIR)}`);
  return failed ? 1 : 0;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  main().then((code) => process.exit(code), (e) => { console.error(e); process.exit(1); });
}
