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
//   node tools/bench/golden.mjs --no-strict     プロジェクトが見つからないシーンを不合格ではなく SKIP にする（既定は strict）
//   node tools/bench/golden.mjs --group repeat  group:"repeat"（DDGI/DXR など決定論が崩れるシーン）だけ回す（通常実行には含めない）
//   node tools/bench/golden.mjs --ui-only       kind:"ui"（エディタ UI の絵）だけ回す。--no-ui は逆に UI を回さない
//   node tools/bench/golden.mjs --force-theme b UI を指定のテーマ案で撮る（基準は変えない＝差分を検出できるかの確認用）
//   node tools/bench/golden.mjs --exe <path>    別の exe を撮る（既定: 環境変数 DX12_ENGINE_EXE → build/dev → build/release の順で最初にあるもの）
//
// ★kind:"ui"（W0-3）: エディタ UI（ImGui 込みの最終画面）のゴールデン。基準は tests/golden/ui/*.png。
//   --background の背景起動（tools/engine_instance.ps1 経由・exe のコピー・人の画面には何も出ない）＋
//   --ui-deterministic（UI の時計を「フレーム番号 × 1/60」に固定・キャレット点滅なし）＋
//   imgui_screenshot {hideCursor:true} で撮る。DPI（--dpi-scale）とテーマ案（--theme-variant）を組み合わせる。
//   プロジェクトは tests/golden/ui_project をコピーして使う（エディタが書き込んでも作業ツリーを汚さない）。
//   masks: [[x0,y0,x1,y1],...]（0..1 の割合）の矩形は比べる前に塗りつぶす（トースト・時刻などの揺れる領域）。
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
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, findDefaultExe, resolveProject, launchEngine, launchInstance, connect, sleep } from "./lib/engine.mjs";
import { parseArgs, selectScenes } from "./lib/sceneSelect.mjs";
import { sha256, resampleArea, compareImages, judge } from "./lib/imageDiff.mjs";

export { sha256, resampleArea, compareImages, judge };

const require = createRequire(import.meta.url);
const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));

const GOLDEN_DIR = path.join(REPO_ROOT, "tests", "golden");
const CONFIG = path.join(GOLDEN_DIR, "golden.json");
const BASELINE = path.join(GOLDEN_DIR, "baseline.json");
const OUT_DIR = path.join(REPO_ROOT, "build", "golden-out");
const UI_DIR = path.join(GOLDEN_DIR, "ui");                       // UI ゴールデンの基準 PNG
const UI_PROJECT_SRC = path.join(GOLDEN_DIR, "ui_project");       // UI 撮影用の最小プロジェクト（コピーして使う）

// ─── 引数 ─ parseArgs / selectScenes は lib/sceneSelect.mjs ────────────────
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
    ...(s.kind === "ui" ? { warmupFrames: s.warmupFrames ?? d.uiWarmupFrames ?? 120 } : {}),
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

/**
 * 実際の「新規シーン」コマンド（new_scene。エディタの「新規シーン」と同じ経路で DirectionalLight + MainCamera + gi::ApplyNew が走る）で
 * シーンを作る。保存先は指定しない（リポジトリのプロジェクトを汚さない）。作り直しの完了は sceneGeneration の増加で待つ。
 */
async function createNewScene(eng) {
  let before = (await eng.call("ping")).sceneGeneration;
  for (let i = 0; i < 600; i++) {
    const p = await eng.call("ping");
    if (!p.sceneLoading) { before = p.sceneGeneration; break; }
    await sleep(500);
  }
  await eng.call("new_scene", {}, 300_000);
  for (let i = 0; i < 600; i++) {
    const p = await eng.call("ping");
    if (!p.sceneLoading && p.sceneGeneration > before) return p;
    await sleep(200);
  }
  throw new Error("new_scene が完了しない");
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

// ─── UI ゴールデン（kind:"ui"）──────────────────────────────────────────
/** masks（0..1 の矩形）を単色で塗りつぶした複製を返す。基準にも今回にも同じ処理をする。 */
export function applyMasks(img, masks) {
  if (!masks?.length) return img;
  const data = Buffer.from(img.data);
  for (const [x0, y0, x1, y1] of masks) {
    const ax = Math.max(0, Math.floor(x0 * img.width)), bx = Math.min(img.width, Math.ceil(x1 * img.width));
    const ay = Math.max(0, Math.floor(y0 * img.height)), by = Math.min(img.height, Math.ceil(y1 * img.height));
    for (let y = ay; y < by; y++)
      for (let x = ax; x < bx; x++) {
        const o = (y * img.width + x) * 4;
        data[o] = 255; data[o + 1] = 0; data[o + 2] = 255; data[o + 3] = 255;
      }
  }
  return { width: img.width, height: img.height, data };
}

/** 撮影用プロジェクトを build/golden-out/ui_project へ作り直す（毎回まっさらにする）。 */
function prepareUiProject() {
  if (!fs.existsSync(UI_PROJECT_SRC)) throw new Error(`UI 撮影用プロジェクトが無い: ${path.relative(REPO_ROOT, UI_PROJECT_SRC)}`);
  const dst = path.join(OUT_DIR, "ui_project");
  fs.rmSync(dst, { recursive: true, force: true });
  fs.cpSync(UI_PROJECT_SRC, dst, { recursive: true });
  return dst;
}

/** 起動引数（決定論・DPI・テーマ案）。テーマ default は何も付けない＝現行の見た目そのまま。 */
export function uiExtraArgs(s, forceTheme = null) {
  const a = ["--ui-deterministic", "--dpi-scale", String(s.dpi ?? 1.0)];
  const theme = forceTheme ?? s.theme ?? "default";
  if (theme !== "default") a.push("--theme-variant", theme);
  return a;
}

/** 1 つの UI シーンを撮る（起動 → 落ち着くまで回す → imgui_screenshot を repeat 回）。 */
// 並行レーン（別 worktree）が同じインスタンスを奪い合わないよう、リポジトリのパスからインスタンス名を決める。
const UI_INSTANCE_NAME = `golden-ui-${createHash("sha1").update(REPO_ROOT).digest("hex").slice(0, 8)}`;

async function captureUi(s, args, project) {
  const buildDir = path.dirname(args.exe);
  const eng = await launchInstance({ name: UI_INSTANCE_NAME, project, buildDir, extraArgs: uiExtraArgs(s, args.forceTheme) });
  let conn;
  try {
    conn = await connect(eng.port);
    // 起動直後はプロジェクトのロード中。ping が通り、数フレーム回せるようになるまで待つ。
    for (let i = 0; i < 240; i++) {
      try { await conn.call("ping"); await conn.call("step_frames", { frames: 2 }, 60_000); break; }
      catch (e) { if (i === 239) throw e; await sleep(500); }
    }
    await conn.call("imgui_virtual_input", { enable: true });
    // レイアウト: default 以外はワークスペース切替コマンド（workspace.material / workspace.lighting）を流してから温める。
    const layout = s.layout ?? "default";
    if (layout !== "default") {
      await conn.call("editor_command_run", { id: `workspace.${layout}` }, 30_000);
      await conn.call("step_frames", { frames: 4, deterministic: true }, 60_000);
    }
    if (s.warmupFrames > 0) await conn.call("step_frames", { frames: s.warmupFrames, deterministic: true }, 300_000);
    const shots = [];
    const n = Math.max(2, args.repeat);   // UI は常に 2 枚以上撮って決定論を確かめる
    for (let k = 0; k < n; k++) {
      const file = path.join(OUT_DIR, `${s.name}.${k}.native.png`);
      await conn.call("step_frames", { frames: s.settleFrames, deterministic: true }, 120_000);
      const r = await conn.call("imgui_screenshot", { hideCursor: true, path: file }, 120_000);
      shots.push({ img: readPng(r.path), file: r.path });
    }
    return shots;
  } finally {
    try { conn?.close(); } catch { /* 既に切れている */ }
    eng.kill();
  }
}

async function runUiScenes(list, args, baseline, results) {
  fs.mkdirSync(UI_DIR, { recursive: true });
  const head = gitHead();
  for (const s of list) {
    const r = { name: s.name, pass: true, reasons: [], ui: true };
    results.push(r);
    const t0 = Date.now();
    try {
      const project = prepareUiProject();
      const shots = await captureUi(s, args, project);
      const first = shots[0];
      r.native = `${first.img.width}x${first.img.height}`;
      // ★マスク（3D ビューポート・FPS・トースト）を塗ってから比べる。ハッシュもマスク後の絵のもの。
      const masked = shots.map((x) => applyMasks(x.img, s.masks));
      r.sha = sha256(masked[0]);
      r.deterministic = masked.every((x) => sha256(x) === r.sha);
      if (!r.deterministic) {
        const c = compareImages(masked[0], masked.at(-1), s.tolerance.pixelThreshold);
        r.determinismDiff = { diffRatio: c.diffRatio, meanAbs: c.meanAbs, maxDist: c.maxDist };
        writePng(path.join(OUT_DIR, `${s.name}.repeat-diff.png`), c.diffImage);
        r.pass = false;
        r.reasons.push("同じセッションで撮り直した 2 枚が一致しない（UI に実時間・乱数が混ざっている）。masks を足すか、原因を直す");
      }
      const small = resampleArea(masked[0], s.width, s.height);
      const refFile = path.join(UI_DIR, `${s.name}.png`);
      if (args.update) {
        writePng(refFile, small);
        baseline.scenes[s.name] = {
          kind: "ui", nativeWidth: first.img.width, nativeHeight: first.img.height, nativeSha256: r.sha /* マスク後 */,
          compareSha256: sha256(small), width: s.width, height: s.height, dpi: s.dpi ?? 1.0, theme: s.theme ?? "default",
          commit: head, capturedAt: new Date().toISOString(), deterministicInSession: r.deterministic,
        };
        r.updated = true;
        continue;
      }
      writePng(path.join(OUT_DIR, `${s.name}.png`), small);
      const b = baseline.scenes[s.name];
      if (!fs.existsSync(refFile)) { r.pass = false; r.reasons.push("基準画像が無い（--update で撮る）"); continue; }
      if (b && (b.nativeWidth !== first.img.width || b.nativeHeight !== first.img.height)) {
        r.pass = args.strict ? false : r.pass; r.skipped = !args.strict; r.envMismatch = true;
        r.reasons.push(`環境違い: ネイティブ解像度が基準と違う（基準 ${b.nativeWidth}x${b.nativeHeight}、今回 ${first.img.width}x${first.img.height}）`);
        continue;
      }
      const ref = readPng(refFile);
      const cmp = compareImages(ref, small, s.tolerance.pixelThreshold);
      writePng(path.join(OUT_DIR, `${s.name}.diff.png`), cmp.diffImage);
      r.diffRatio = cmp.diffRatio; r.meanAbs = cmp.meanAbs; r.maxDist = cmp.maxDist;
      const j = judge(cmp, s.tolerance);
      r.pass = r.pass && j.pass; r.reasons.push(...j.reasons);
      if (b) r.bitExact = b.nativeSha256 === r.sha;
    } catch (e) {
      r.pass = false;
      r.reasons.push(`撮影失敗: ${e.message}`);
    }
    console.log(`  (ui ${s.name}: ${((Date.now() - t0) / 1000).toFixed(1)}s)`);
  }
}

// ─── 本体 ───────────────────────────────────────────────────────────────
async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) {
    console.log("node tools/bench/golden.mjs [--update] [--repeat N] [--only a,b] [--exe path]");
    return 0;
  }
  let scenes = loadConfig();
  // requiresProjectsRoot（手元のゲームが要る既存 4 件）/ group の振り分けは lib/sceneSelect.mjs（単体テストあり）。
  const sel = selectScenes(scenes, { hasRoot: !!process.env.DX12_GOLDEN_PROJECTS_ROOT, group: args.group, only: args.only });
  if (sel.gated.length) console.log(`対象外（DX12_GOLDEN_PROJECTS_ROOT 未設定）: ${sel.gated.join(", ")}`);
  scenes = sel.scenes;
  if (!scenes.length) { console.log("対象のシーンが無い"); return 1; }
  const uiScenes = scenes.filter((s) => s.kind === "ui");
  for (const s of scenes) if (s.kind === "ui" && !["default", "material", "lighting"].includes(s.layout ?? "default")) throw new Error(`${s.name}: layout "${s.layout}" は未対応（default / material / lighting）`);
  scenes = scenes.filter((s) => s.kind !== "ui");
  if (args.uiOnly) scenes = [];
  if (args.noUi) uiScenes.length = 0;
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
    const eng = await launchEngine({ exe: args.exe, project, scene: list[0].scene ?? list[0].launchScene });
    let conn;
    try {
      conn = await connect(eng.port);
      // requiresDxr:true のシーンは「ハードが DXR 非対応」のときだけ SKIP（不合格にしない）。
      // ハードは raytracingTier で判定する。supported は 6 段ゲート通過後の値なので、Tier があるのに false なら回帰として FAIL。
      let dxrHw = null;   // true=ハード対応, false=非対応, Error=get_dxr 失敗
      let dxrEnabled = false;
      if (list.some((x) => x.requiresDxr)) {
        try {
          const d = await conn.call("get_dxr");
          dxrHw = String(d.raytracingTier ?? "none") !== "none";
          dxrEnabled = !!d.supported;
        } catch (e) { dxrHw = e instanceof Error ? e : new Error(String(e)); }
      }
      for (const s of list) {
        const r = { name: s.name, pass: true, reasons: [] };
        results.push(r);
        if (s.requiresDxr) {
          if (dxrHw instanceof Error) {
            r.pass = false; r.reasons.push(`get_dxr が失敗した（通信/MCP の不具合）: ${dxrHw.message}`);
            continue;
          }
          if (!dxrHw) {
            r.skipped = true; r.reasons.push("DXR 非対応の PC なので SKIP（requiresDxr）");
            continue;
          }
          if (!dxrEnabled) {
            r.pass = false; r.reasons.push("ハードは DXR 対応（raytracingTier あり）なのに get_dxr.supported=false（DXR パイプライン/.cso の回帰）");
            continue;
          }
        }
        try {
          if (s.newScene) await createNewScene(conn); else await openScene(conn, s.scene);
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

  if (uiScenes.length) await runUiScenes(uiScenes, args, baseline, results);

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
