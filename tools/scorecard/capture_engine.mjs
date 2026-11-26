// 品質スコアカード（W05-35）: エンジンを動かして撮る側。tools/scorecard/capture.ps1 が段（--stage）ごとに呼ぶ。
//
// 規則（HARD RULE）:
//   ・窓を出さない／実マウスとフォーカスに触れない。エンジンは tools/engine_instance.ps1 経由の --headless / --background だけ
//     （tools/bench/lib/engine.mjs の launchInstance）。--splash-preview は窓なしのオフスクリーン描画。
//   ・ビルド出力を握らない: 起動する exe は常に「インスタンス専用フォルダのコピー」（--tool-dir も同様）。
//   ・撮影は決定論モード（step_frames deterministic / screenshot_final deterministic / --ui-deterministic）。
//   ・API キーは使わない。
//
// 使い方（通常は capture.ps1 から）:
//   node tools/scorecard/capture_engine.mjs --stage <splash|ui|golden|templates|vfx|startup>
//        --engine-dir <exe のあるフォルダ> --run-dir <出力> --tool-dir <exe のコピー置き場> [--port 8855] [--no-dred]
//        [--startup-runs 6]
// 出力: <run-dir>/stage_<stage>.json（{stage, ok, items[], notes[], errors[]}）と各画像。
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { spawn, spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import { fileURLToPath, pathToFileURL } from "node:url";
import { REPO_ROOT, launchInstance, connect, portOpen, sleep } from "../bench/lib/engine.mjs";
import { loadGoldenEntries } from "../bench/lib/goldenStore.mjs";
import { sha256, compareImages } from "../bench/lib/imageDiff.mjs";

const require = createRequire(import.meta.url);
const { PNG } = require(path.join(REPO_ROOT, "tools/mcp-server/node_modules/pngjs"));

export const STAGES = ["splash", "ui", "golden", "templates", "vfx", "startup"];
export const TEMPLATES = ["empty", "fps", "tps", "2d"];
// golden_core の 5 シーン（リポジトリ内で完結するもの）。新規シーン（new_scene の結果）は別枠で撮る。
export const CORE_SCENES = ["core_pbr_spheres", "core_csm_shadows", "core_transparency", "core_post_stack", "core_load_defaults"];
export const NEW_SCENE_ID = "core_new_scene_gi";

// 絶対パス・ユーザー名を要約に残さない
export function sanitizeMsg(m) {
  return String(m ?? "").replace(/[A-Za-z]:[\\/][^\s"'`<>|,;)]*/g, "<path>").replace(/\/(?:Users|home)\/[^\s"']*/g, "<path>");
}

// ─── 引数 ──────────────────────────────────────────────────────────────
export function parseArgs(argv) {
  const a = { stage: null, engineDir: null, runDir: null, toolDir: null, port: 8855, noDred: false, startupRuns: 6, instance: null };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const v = () => { if (i + 1 >= argv.length) throw new Error(`${k} に値が要る`); return argv[++i]; };
    if (k === "--stage") a.stage = v();
    else if (k === "--engine-dir") a.engineDir = path.resolve(v());
    else if (k === "--run-dir") a.runDir = path.resolve(v());
    else if (k === "--tool-dir") a.toolDir = path.resolve(v());
    else if (k === "--port") a.port = Number(v());
    else if (k === "--startup-runs") a.startupRuns = Math.max(2, Number(v()) || 6);
    else if (k === "--instance") a.instance = v();
    else if (k === "--no-dred") a.noDred = true;
    else throw new Error(`不明な引数: ${k}`);
  }
  if (!STAGES.includes(a.stage)) throw new Error(`--stage は ${STAGES.join("|")} のどれか`);
  for (const [n, x] of [["--engine-dir", a.engineDir], ["--run-dir", a.runDir], ["--tool-dir", a.toolDir]])
    if (!x) throw new Error(`${n} が要る`);
  if (!Number.isInteger(a.port) || a.port < 1024) throw new Error("--port が不正");
  a.instance ??= `scorecard-w0-${createHash("sha1").update(REPO_ROOT).digest("hex").slice(0, 8)}`;
  return a;
}

// ─── 画像ユーティリティ ───────────────────────────────────────────────
export function readPng(file) {
  const png = PNG.sync.read(fs.readFileSync(file));
  return { width: png.width, height: png.height, data: png.data };
}
function writePngBuf(file, png) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, PNG.sync.write(png));
}
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
const rel = (runDir, f) => path.relative(runDir, f).split(path.sep).join("/");
const lumaOf = (img) => {
  const n = img.width * img.height;
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) out[i] = (img.data[i * 4] * 0.2126 + img.data[i * 4 + 1] * 0.7152 + img.data[i * 4 + 2] * 0.0722) / 255;
  return out;
};

// ─── エンジン起動の共通部 ───────────────────────────────────────────────
function engineArgs(args, extra = []) {
  return [...(args.noDred ? ["--no-dred"] : []), ...extra];
}
async function waitReady(conn, tries = 480) {
  // 起動直後はプロジェクトのロード中。ping が通り、sceneLoading が落ち、数フレーム回せるまで待つ。
  for (let i = 0; i < tries; i++) {
    try {
      const p = await conn.call("ping");
      if (!p.sceneLoading) { await conn.call("step_frames", { frames: 2 }, 60_000); return p; }
    } catch (e) { if (i === tries - 1) throw e; }
    await sleep(500);
  }
  throw new Error("エンジンが ready にならない");
}
async function withEngine(args, { project, mode = "headless", extra = [] }, fn) {
  const eng = await launchInstance({
    name: args.instance, project, port: args.port, buildDir: args.engineDir, mode, extraArgs: engineArgs(args, extra),
  });
  let conn;
  try {
    conn = await connect(eng.port);
    return await fn(conn, eng);
  } finally {
    try { conn?.close(); } catch { /* 既に切れている */ }
    eng.kill();
  }
}

/** exe のコピー（--tool-dir）で --new-project を走らせて、テンプレートからプロジェクトを作る（GUI は起動しない）。 */
export function createTemplateProject(toolDir, dir, template) {
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(path.dirname(dir), { recursive: true });
  const r = spawnSync(path.join(toolDir, "DX12Engine.exe"), ["--new-project", dir, "--template", template],
    { cwd: toolDir, timeout: 120_000, windowsHide: true, encoding: "utf8" });
  if (r.status !== 0) throw new Error(`--new-project(${template}) が失敗 status=${r.status} ${r.error?.message ?? ""}`);
  return dir;
}

/** 同じセッションで撮った 2 枚の差（一致なら null）。決定論の範囲を実測で決めるために items へ残す。 */
export function selfDiff(a, b) {
  if (sha256(a) === sha256(b)) return null;
  const c = compareImages(a, b, 12);
  return { diffRatio: c.diffRatio, meanAbs: c.meanAbs };
}
function stageResult(stage) { return { stage, ok: true, items: [], notes: [], errors: [] }; }
function fail(res, id, e) { res.ok = false; res.errors.push({ id, message: sanitizeMsg(e?.message ?? e).slice(0, 400) }); }

// ─── 段 1: 旧スプラッシュ（--splash-preview の書き出し） ──────────────────
async function stageSplash(args) {
  const res = stageResult("splash");
  const exe = path.join(args.toolDir, "DX12Engine.exe");
  for (const [id, extra] of [["startup", ["--splash-mode", "startup"]], ["project", ["--splash-mode", "project", "--splash-project", "Junction"]]]) {
    const dir = path.join(args.runDir, "splash", id);
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    const r = spawnSync(exe, ["--splash-preview", dir, "--dpi-scale", "1", "--splash-fps", "30", ...extra],
      { cwd: args.toolDir, timeout: 180_000, windowsHide: true, encoding: "utf8" });
    fs.writeFileSync(path.join(dir, "splash_preview.log"), `${r.stdout ?? ""}\n${r.stderr ?? ""}`);
    if (r.status !== 0) { fail(res, id, `--splash-preview が失敗 status=${r.status}`); continue; }
    const pngs = fs.readdirSync(dir).filter((f) => f.toLowerCase().endsWith(".png")).sort();
    if (!pngs.length) { fail(res, id, "PNG が 0 枚"); continue; }
    // フレームごとの画素ハッシュ（決定論の比較用）と、全体を 1 つにまとめたハッシュ。
    const frames = pngs.map((f) => ({ file: rel(args.runDir, path.join(dir, f)), sha256: sha256(readPng(path.join(dir, f))) }));
    const all = createHash("sha256").update(frames.map((x) => x.sha256).join("\n")).digest("hex");
    const first = readPng(path.join(dir, pngs[0]));
    res.items.push({ id: `splash_${id}`, kind: "image-seq", count: frames.length, width: first.width, height: first.height, sha256: all, frames });
    // コンタクトシート（python と PIL があるときだけ。無ければ飛ばす）
    const csv = path.join(dir, "frames.csv");
    if (fs.existsSync(csv)) {
      const sheet = path.join(args.runDir, "splash", `${id}_contact.png`);
      const py = spawnSync("python", ["-I", path.join(REPO_ROOT, "tools", "splash_contact_sheet.py"), dir, sheet], { encoding: "utf8", timeout: 120_000, windowsHide: true });
      if (py.status !== 0) res.notes.push(`コンタクトシートは作らなかった(${id}): status=${py.status} ${sanitizeMsg(py.error?.message ?? py.stderr ?? "").slice(0, 200)}`);
    }
  }
  return res;
}

// ─── 段 2: エディタ UI（旧テーマ。UI ゴールデンの撮り方そのまま） ──────────
async function stageUi(args) {
  const res = stageResult("ui");
  const cfg = loadGoldenEntries(path.join(REPO_ROOT, "tests", "golden"));
  const scenes = cfg.scenes.filter((s) => s.kind === "ui" && (s.theme ?? "default") === "default");
  const uiProjectSrc = path.join(REPO_ROOT, "tests", "golden", "ui_project");
  for (const s of scenes) {
    const project = path.join(args.runDir, "ui", "_project");
    fs.rmSync(project, { recursive: true, force: true });
    fs.cpSync(uiProjectSrc, project, { recursive: true });
    const extra = ["--ui-deterministic", "--dpi-scale", String(s.dpi ?? 1.0)];
    const warm = s.warmupFrames ?? cfg.defaults.uiWarmupFrames ?? 120;
    const settle = s.settleFrames ?? cfg.defaults.settleFrames ?? 8;
    try {
      const shots = await withEngine(args, { project, mode: "background", extra }, async (conn) => {
        await waitReady(conn);
        await conn.call("imgui_virtual_input", { enable: true });
        const layout = s.layout ?? "default";
        if (layout !== "default") {
          await conn.call("editor_command_run", { id: `workspace.${layout}` }, 30_000);
          await conn.call("step_frames", { frames: 4, deterministic: true }, 60_000);
        }
        if (warm > 0) await conn.call("step_frames", { frames: warm, deterministic: true }, 300_000);
        const out = [];
        for (let k = 0; k < 2; k++) {
          const file = path.join(args.runDir, "ui", `${s.name}.${k}.png`);
          await conn.call("step_frames", { frames: settle, deterministic: true }, 120_000);
          const r = await conn.call("imgui_screenshot", { hideCursor: true, path: file }, 120_000);
          out.push(readPng(r.path ?? file));
        }
        return out;
      });
      const masked = shots.map((x) => applyMasks(x, s.masks));
      const sha = sha256(masked[0]);
      res.items.push({
        id: s.name, kind: "image", file: `ui/${s.name}.0.png`, width: shots[0].width, height: shots[0].height,
        sha256: sha256(shots[0]), maskedSha256: sha, dpi: s.dpi ?? 1, layout: s.layout ?? "default", theme: "default",
        deterministicInSession: masked.every((x) => sha256(x) === sha), selfDiff: selfDiff(masked[0], masked[1]), masks: s.masks ?? [],
      });
    } catch (e) { fail(res, s.name, e); }
  }
  fs.rmSync(path.join(args.runDir, "ui", "_project"), { recursive: true, force: true });
  if (!scenes.length) fail(res, "ui", "default テーマの UI シーンが無い");
  return res;
}

// ─── 段 3: golden_core と新規シーン（3D） ─────────────────────────────────
async function openScene(conn, scene) {
  for (let i = 0; i < 600; i++) {
    const p = await conn.call("ping");
    if (p.currentScene === scene) return p;
    try { await conn.call("open_scene", { path: scene }, 300_000); }
    catch (e) { if (!/already in progress/.test(e.message)) throw e; }
    await sleep(500);
  }
  throw new Error(`シーンが開かない: ${scene}`);
}
async function createNewScene(conn) {
  let before = (await conn.call("ping")).sceneGeneration;
  for (let i = 0; i < 600; i++) {
    const p = await conn.call("ping");
    if (!p.sceneLoading) { before = p.sceneGeneration; break; }
    await sleep(500);
  }
  await conn.call("new_scene", {}, 300_000);
  for (let i = 0; i < 600; i++) {
    const p = await conn.call("ping");
    if (!p.sceneLoading && p.sceneGeneration > before) return p;
    await sleep(200);
  }
  throw new Error("new_scene が完了しない");
}
async function stageGolden(args) {
  const res = stageResult("golden");
  const cfg = loadGoldenEntries(path.join(REPO_ROOT, "tests", "golden"));
  const byName = new Map(cfg.scenes.map((s) => [s.name, s]));
  const list = [...CORE_SCENES, NEW_SCENE_ID].map((n) => byName.get(n)).filter(Boolean);
  const missing = [...CORE_SCENES, NEW_SCENE_ID].filter((n) => !byName.has(n));
  for (const m of missing) fail(res, m, "tests/golden/entries に無い");
  if (!list.length) return res;
  const project = path.resolve(REPO_ROOT, list[0].project);
  const d = cfg.defaults;
  await withEngine(args, { project, mode: "headless", extra: ["--scene", list[0].scene ?? list[0].launchScene] }, async (conn) => {
    await waitReady(conn);
    let dxr = null;
    for (const s of list) {
      const warm = s.warmupFrames ?? d.warmupFrames ?? 60, settle = s.settleFrames ?? d.settleFrames ?? 8;
      try {
        if (s.requiresDxr && dxr === null) { try { dxr = await conn.call("get_dxr"); } catch (e) { dxr = { error: String(e.message) }; } }
        if (s.newScene) await createNewScene(conn); else await openScene(conn, s.scene);
        const shots = [];
        for (let k = 0; k < 2; k++) {
          await conn.call("set_editor_camera", { position: s.camera.position, target: s.camera.target });
          if (warm > 0) await conn.call("step_frames", { frames: warm }, 300_000);
          const file = path.join(args.runDir, "golden", `${s.name}.${k}.png`);
          const r = await conn.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: settle, path: file }, 300_000);
          shots.push(readPng(r.path ?? file));
        }
        const sha = sha256(shots[0]);
        res.items.push({
          id: s.name, kind: "image", file: `golden/${s.name}.0.png`, width: shots[0].width, height: shots[0].height,
          sha256: sha, deterministicInSession: shots.every((x) => sha256(x) === sha), selfDiff: selfDiff(shots[0], shots[1]),
          ...(s.newScene ? { newScene: true } : {}),
        });
      } catch (e) { fail(res, s.name, e); }
    }
  });
  return res;
}

// ─── 段 4: テンプレート 4 種 ─────────────────────────────────────────────
async function stageTemplates(args) {
  const res = stageResult("templates");
  for (const t of TEMPLATES) {
    const proj = path.join(args.runDir, "templates", "_proj", t);
    try {
      createTemplateProject(args.toolDir, proj, t);
      const item = await withEngine(args, { project: proj, mode: "headless" }, async (conn) => {
        const p = await waitReady(conn);
        const shots = [];
        for (let k = 0; k < 2; k++) {
          await conn.call("step_frames", { frames: 120, deterministic: true }, 300_000);
          const file = path.join(args.runDir, "templates", `${t}.${k}.png`);
          const r = await conn.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: 8, path: file }, 300_000);
          shots.push(readPng(r.path ?? file));
        }
        const ping = await conn.call("ping");
        return {
          id: `template_${t}`, kind: "image", file: `templates/${t}.0.png`, width: shots[0].width, height: shots[0].height,
          sha256: sha256(shots[0]), deterministicInSession: sha256(shots[0]) === sha256(shots[1]), selfDiff: selfDiff(shots[0], shots[1]), scene: ping.currentScene ?? p.currentScene ?? null,
        };
      });
      res.items.push(item);
    } catch (e) { fail(res, `template_${t}`, e); }
  }
  fs.rmSync(path.join(args.runDir, "templates", "_proj"), { recursive: true, force: true });
  return res;
}

// ─── 段 5: 旧 33 VFX のコンタクトシート ───────────────────────────────────
const VFX_FRAMES = 4, VFX_STEP = 20;   // 20 フレーム（1/3 秒）ずつ 4 枚
async function stageVfx(args) {
  const res = stageResult("vfx");
  const vfxMod = await import(pathToFileURL(path.join(REPO_ROOT, "tools/mcp-server/vfx.ts")).href);
  const sheetMod = await import(pathToFileURL(path.join(REPO_ROOT, "tools/mcp-server/contactSheet.ts")).href);
  const ids = vfxMod.VFX_IDS;
  res.notes.push(`レシピ ${ids.length} 件（tools/mcp-server/vfx.ts）`);
  const proj = path.join(args.runDir, "vfx", "_proj", "empty");
  createTemplateProject(args.toolDir, proj, "empty");
  const pos = [0, 0, 0], dist = 3.5, height = 1;   // 粒子は原点の上 1m、カメラは 3.5m 手前
  await withEngine(args, { project: proj, mode: "headless" }, async (conn) => {
    await waitReady(conn);
    const shoot = async (tag) => {
      const file = path.join(args.runDir, "vfx", "_tmp", `${tag}.png`);
      const r = await conn.call("screenshot_final", { deterministic: true, gizmos: false, settleFrames: 2, path: file }, 120_000);
      return fs.readFileSync(r.path ?? file);
    };
    const step = (n) => conn.call("step_frames", { frames: n, deterministic: true, hold: true }, 120_000);
    // テンプレートの既定エンティティ（原点の立方体など）が粒子を隠すので、カメラ・ライト以外を全部消す。
    const listed = await conn.call("list_entities", { limit: 0 });
    const ents = listed.entities ?? listed.items ?? (Array.isArray(listed) ? listed : []);
    let removed = 0;
    for (const en of ents) {
      const comps = JSON.stringify(en.components ?? en.componentTypes ?? "");
      if (/camera|light|Camera|Light/.test(comps) || /camera|light/i.test(String(en.name ?? ""))) continue;
      const id = en.entityId ?? en.entity ?? en.id;
      if (id === undefined) continue;
      await conn.call("delete_entity", { entity: id }).then(() => { removed++; }).catch(() => {});
    }
    res.notes.push(`撮影前に既定エンティティを ${removed} 件消した（全 ${ents.length} 件）`);
    await conn.call("set_editor_camera", { position: [pos[0], pos[1] + height, pos[2] - dist], target: [pos[0], pos[1] + height, pos[2]] });
    await step(30);
    const baseImg = PNG.sync.read(await shoot("base"));
    const baseLuma = lumaOf(baseImg);
    const tiles = [];
    for (const id of ids) {
      let entity;
      try {
        const r = vfxMod.resolveVfx(id);
        const made = await conn.call("create_entity", { type: r.layers.length ? "particle_emitter" : "empty", name: `FX_${id}`, position: [pos[0], pos[1] + height - 0.5, pos[2]] });
        entity = made.entityId;
        const ref = { entity };
        for (let i = 0; i < r.layers.length; i++) {
          if (i === 0) await conn.call("set_component", { ...ref, component: "particleEmitter", data: { ...r.layers[0] } });
          else {
            await conn.call("add_particle_layer", { ...ref, layerName: r.layers[i].name });
            await conn.call("set_component", { ...ref, component: "particleEmitter", layer: i, data: { ...r.layers[i] } });
          }
        }
        if (r.trail) await conn.call("set_component", { ...ref, component: "trailRenderer", data: { ...r.trail } });
        const oneShot = r.layers.length > 0 && r.layers.every((l) => l.looping === false);
        if (oneShot) await conn.call("eval_lua", { code: `return tostring(fx:play(${JSON.stringify(`FX_${id}`)}))` }).catch(() => null);
        // トレイルは動く実体が要るので、撮影中は 5 フレームごとに X 方向へ往復させる。
        let tick = 0;
        const advance = async (n) => {
          if (!r.trail) { await step(n); return; }
          for (let done = 0; done < n; done += 5) {
            tick++;
            await conn.call("set_transform", { entity, position: [Math.sin(tick * 0.35) * 1.2, pos[1] + height - 0.5, pos[2]] }).catch(() => {});
            await step(Math.min(5, n - done));
          }
        };
        await advance(oneShot ? 6 : 90);   // ループ系は 1.5 秒ならして定常へ。ワンショットは鳴らした直後から撮る
        const bufs = [];
        for (let f = 0; f < VFX_FRAMES; f++) { await advance(VFX_STEP); bufs.push(await shoot(`${id}_${f}`)); }
        const imgs = bufs.map((b) => PNG.sync.read(b));
        // 指標: 被覆（基準画から 12/441 以上離れた画素の割合）、明るさの増分、ちらつき（フレーム平均輝度の標準偏差）
        const lumas = imgs.map(lumaOf);
        let cover = 0;
        const gains = [];
        for (let f = 0; f < imgs.length; f++) {
          const c = compareImages({ width: baseImg.width, height: baseImg.height, data: baseImg.data }, { width: imgs[f].width, height: imgs[f].height, data: imgs[f].data }, 12);
          cover += c.diffRatio;
          let g = 0; for (let i = 0; i < lumas[f].length; i++) g += lumas[f][i] - baseLuma[i];
          gains.push(g / lumas[f].length);
        }
        const mean = gains.reduce((a, b) => a + b, 0) / gains.length;
        const flicker = Math.sqrt(gains.reduce((a, b) => a + (b - mean) ** 2, 0) / gains.length);
        const sheet = sheetMod.buildContactSheet(bufs, { columns: VFX_FRAMES, tileWidth: 320 });
        const file = path.join(args.runDir, "vfx", `${id}.png`);
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, sheet.sheetPng);
        tiles.push(bufs.at(-1));
        res.items.push({
          id: `vfx_${id}`, kind: "image", file: `vfx/${id}.png`, layers: r.layers.length, oneShot,
          sha256: createHash("sha256").update(imgs.map((x) => sha256(x)).join("\n")).digest("hex"),
          metrics: { coverage: cover / imgs.length, lumaGain: mean, flicker },
        });
      } catch (e) { fail(res, `vfx_${id}`, e); }
      finally { if (entity !== undefined) await conn.call("delete_entity", { entity }).catch(() => {}); await step(2).catch(() => {}); }
    }
    await conn.call("step_frames", { frames: 1, deterministic: true, hold: false }).catch(() => {});
    if (tiles.length) {
      const ov = sheetMod.buildContactSheet(tiles, { columns: 6, tileWidth: 300, label: true });
      fs.writeFileSync(path.join(args.runDir, "vfx", "_overview.png"), ov.sheetPng);
      res.notes.push(`全体の一覧: vfx/_overview.png（コマの番号 n は次の並びの n 番目: ${ids.join(",")} の順）`);
    }
  });
  fs.rmSync(path.join(args.runDir, "vfx", "_tmp"), { recursive: true, force: true });
  fs.rmSync(path.join(args.runDir, "vfx", "_proj"), { recursive: true, force: true });
  return res;
}

// ─── 段 6: 起動秒数（ウォームスタートの EMA） ─────────────────────────────
export function ema(values, alpha = 0.3) {
  if (!values.length) return null;
  let e = values[0];
  for (let i = 1; i < values.length; i++) e = alpha * values[i] + (1 - alpha) * e;
  return e;
}
function killPid(pid) { spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], { stdio: "ignore", windowsHide: true }); }

async function stageStartup(args) {
  const res = stageResult("startup");
  const proj = path.join(args.runDir, "startup", "_proj", "empty");
  createTemplateProject(args.toolDir, proj, "empty");
  // 1 回目: engine_instance.ps1 経由でインスタンスのフォルダへコピーさせる（ここではコピー込みなので測定に使わない）。
  const eng = await launchInstance({ name: args.instance, project: proj, port: args.port, buildDir: args.engineDir, mode: "background", extraArgs: engineArgs(args) });
  const instDir = eng.dir;
  eng.kill();
  await sleep(1500);
  const exe = path.join(instDir, "DX12Engine.exe");
  const runs = [];
  // 先頭の 1 回はディスクキャッシュが冷えている可能性があるので cold として分け、warm 回数ぶん続けて測る。
  const total = args.startupRuns + 1;
  for (let i = 0; i < total; i++) {
    const argv = ["--background", "--mcp-port", String(args.port), "--idle-exit", "20", "--project", proj, ...engineArgs(args)];
    const env = { ...process.env, DX12E_DATA_DIR: path.join(instDir, "data"), DX12E_NO_SPLASH: "1" };
    const t0 = performance.now();
    const child = spawn(exe, argv, { cwd: instDir, env, windowsHide: true, stdio: "ignore", detached: false });
    let tPort = null, tReady = null, err = null;
    try {
      while (performance.now() - t0 < 120_000) {
        if (tPort === null && await portOpen(args.port, 100)) tPort = performance.now() - t0;
        if (tPort !== null) {
          let conn;
          try {
            conn = await connect(args.port);
            const p = await conn.call("ping", {}, 10_000);
            if (!p.sceneLoading) { tReady = performance.now() - t0; conn.close(); break; }
            conn.close();
          } catch { try { conn?.close(); } catch { /* 無視 */ } }
        }
        await sleep(40);
      }
      if (tReady === null) err = "120 秒で ready にならない";
    } finally { killPid(child.pid); await sleep(1500); }
    runs.push({ run: i, kind: i === 0 ? "first" : "warm", portOpenSec: tPort === null ? null : tPort / 1000, readySec: tReady === null ? null : tReady / 1000, ...(err ? { error: err } : {}) });
    if (err) fail(res, `run${i}`, err);
  }
  const warm = runs.filter((r) => r.kind === "warm" && r.readySec !== null).map((r) => r.readySec);
  const sorted = [...warm].sort((a, b) => a - b);
  res.items.push({
    id: "startup_ready_ema", kind: "metric", unit: "s", alpha: 0.3,
    value: ema(warm), median: sorted.length ? sorted[Math.floor(sorted.length / 2)] : null,
    min: sorted[0] ?? null, max: sorted.at(-1) ?? null, runs,
  });
  // エンジン自身の startup.json（SplashProgress.h の EMA）が残っていれば併記する（スプラッシュ経路の値。--background では更新されないことがある）
  try {
    const sj = path.join(instDir, "data", "startup.json");
    if (fs.existsSync(sj)) res.items.at(-1).engineStartupJson = JSON.parse(fs.readFileSync(sj, "utf8"));
  } catch { /* 無視 */ }
  res.notes.push("startup_ready_ema = プロセス起動から ping が通り sceneLoading が false になるまで（--background、スプラッシュ無し、空のテンプレート、alpha 0.3、warm のみ）。★ST-1 の「ウォームスタートの EMA」（SplashProgress.h の startup.json、スプラッシュ経路・1500〜6000ms にクランプ）とは別の量。目標の判定はスプラッシュ経路の値で行うこと。first は直前に 1 回起動した後の先頭 1 回で、本物のコールドではない");
  fs.rmSync(path.join(args.runDir, "startup", "_proj"), { recursive: true, force: true });
  return res;
}

// ─── 本体 ───────────────────────────────────────────────────────────────
const RUNNERS = { splash: stageSplash, ui: stageUi, golden: stageGolden, templates: stageTemplates, vfx: stageVfx, startup: stageStartup };

async function main() {
  const args = parseArgs(process.argv.slice(2));
  fs.mkdirSync(args.runDir, { recursive: true });
  let res;
  try { res = await RUNNERS[args.stage](args); }
  catch (e) { res = stageResult(args.stage); fail(res, args.stage, e); }
  fs.writeFileSync(path.join(args.runDir, `stage_${args.stage}.json`), JSON.stringify(res, null, 2));
  console.log(`[scorecard] ${args.stage}: ${res.ok ? "ok" : "ERRORS"} items=${res.items.length} errors=${res.errors.length}`);
  for (const e of res.errors) console.log(`  - ${e.id}: ${e.message}`);
  return res.ok ? 0 : 1;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main().then((c) => process.exit(c), (e) => { console.error(e); process.exit(1); });
