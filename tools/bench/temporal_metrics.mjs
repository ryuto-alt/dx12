// 時間方向の画質指標（PI-01b / W1-NB4）。静止カメラ（または決まった動き）の N フレームを MCP で連写し、
// ちらつき・残光（ゴースト）・カット直後の収束（マスク内 SSIM）・DoF / ブルーム / モーションブラーのエネルギー比を JSON で出す。
// PI-03（TAA v2）、PI-05（Bloom v2）ほかの「受け入れの物差し」。数値の計算は Python（tools/bench/temporal_metrics.py と energy_check.py。numpy。-I で実行）。
//
//   node tools/bench/temporal_metrics.mjs --scene taa_torture [--phase flicker,ghost,ssim,energy] [--repeat 2]
//        [--out <dir>] [--json <file>] [--exe <DX12Engine.exe>] [--port N] [--settings '{"taa":{"feedbackMax":0.9},"post":{"bloomOn":false}}']
//        [--keep-frames] [--python <python.exe>]
//   --scene all で 4 シーン（taa_torture / dof_chart / mb_wheel / hdr_range）すべて
//   --repeat 2 で同じ手順を 2 回走らせ、指標の差（maxRelDiff）が 1% 以内か（within1pct）も出す（決定論の確認）
//
// 撮り方:
//   * シーンは tests/golden/entries/<scene>.json（golden.mjs と同じ入口）。プロジェクトは一時フォルダへコピーしてから開く
//     （MCP の編集で自動保存が走っても、リポジトリのシーン JSON を書き換えない）。
//   * 1 枚 = step_frames{1, deterministic, hold} と screenshot_final{pfm} を**続けて送る**（応答を待たずに 2 本を同じ読み取りに載せる）。
//     こうすると撮影がステップしたそのフレームに載り、MCP の往復で余計に描かれるフレームが入らない（TAA の蓄積枚数が毎回同じになる）。
//   * PFM は線形 HDR（トーンマップ前・露出前。ポスト = TAA・ブルーム・DoF・モーションブラーは適用済み）。
//   * 手順（フレーム数・領域・動かす対象）は tools/bench/temporal_metrics.scenarios.json。判定の数値はここにも JSON にも置かない。
// ★窓は出さない・実マウスは触らない: tools/engine_instance.ps1 --headless（lib/engine.mjs）。自分が起動した PID だけを止める。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { REPO_ROOT, findDefaultExe, resolveProject, launchEngine, connect, sleep, portOpen } from "./lib/engine.mjs";
import { loadGoldenEntries } from "./lib/goldenStore.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const SCENARIOS_FILE = path.join(HERE, "temporal_metrics.scenarios.json");
export const METRICS_PY = path.join(HERE, "temporal_metrics.py");
export const ENERGY_PY = path.join(HERE, "energy_check.py");
const GOLDEN_DIR = path.join(REPO_ROOT, "tests", "golden");

export function loadScenarios(file = SCENARIOS_FILE) {
  return JSON.parse(fs.readFileSync(file, "utf8"));
}

// ─── 引数 ─────────────────────────────────────────────────────────────
export function parseArgs(argv) {
  const a = { scenes: [], phases: null, repeat: 1, out: null, json: null, exe: null, port: null, settings: null, keepFrames: false, python: null, help: false };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const v = () => { if (i + 1 >= argv.length) throw new Error(`${k} に値が要る`); return argv[++i]; };
    if (k === "--scene") a.scenes = v().split(",").filter(Boolean);
    else if (k === "--phase") a.phases = v().split(",").filter(Boolean);
    else if (k === "--repeat") { a.repeat = Number(v()); if (!(a.repeat >= 1 && a.repeat <= 5)) throw new Error("--repeat は 1〜5"); }
    else if (k === "--out") a.out = v();
    else if (k === "--json") a.json = v();
    else if (k === "--exe") a.exe = v();
    else if (k === "--port") a.port = Number(v());
    else if (k === "--settings") a.settings = JSON.parse(v());
    else if (k === "--python") a.python = v();
    else if (k === "--keep-frames") a.keepFrames = true;
    else if (k === "--help" || k === "-h") a.help = true;
    else throw new Error(`知らない引数: ${k}`);
  }
  return a;
}

// ─── Python（numpy の入った）を探す ───────────────────────────────────────
export function findPython(explicit, env = process.env, exists = fs.existsSync) {
  if (explicit) return explicit;
  if (env.UNO_BENCH_PYTHON) return env.UNO_BENCH_PYTHON;
  for (const c of [path.join(REPO_ROOT, "tools", "parity", ".venv", "Scripts", "python.exe"), path.join(REPO_ROOT, "tools", "parity", ".venv", "bin", "python")])
    if (exists(c)) return c;
  return "python";
}

function runPy(python, script, args) {
  const r = spawnSync(python, ["-I", script, ...args], { encoding: "utf8", env: { ...process.env, PYTHONUTF8: "1" }, maxBuffer: 1 << 26 });
  if (r.error) throw new Error(`Python を起動できない（${python}）: ${r.error.message}`);
  if (r.status !== 0) throw new Error(`${path.basename(script)} が失敗（${r.status}）: ${(r.stderr || r.stdout || "").trim().slice(0, 800)}`);
  return r;
}

/** manifest（撮った PFM の一覧と領域）→ 指標 JSON。 */
export function analyzeManifest(python, manifest, tmpDir) {
  const f = path.join(tmpDir, `manifest_${process.pid}_${Math.random().toString(36).slice(2, 8)}.json`);
  fs.writeFileSync(f, JSON.stringify(manifest, null, 2));
  try { return JSON.parse(runPy(python, METRICS_PY, ["--manifest", f]).stdout); }
  finally { fs.rmSync(f, { force: true }); }
}

export function compareResults(python, a, b, tmpDir) {
  const fa = path.join(tmpDir, "cmp_a.json"), fb = path.join(tmpDir, "cmp_b.json");
  fs.writeFileSync(fa, JSON.stringify(a)); fs.writeFileSync(fb, JSON.stringify(b));
  try { return JSON.parse(runPy(python, METRICS_PY, ["--compare", fa, fb]).stdout); }
  finally { fs.rmSync(fa, { force: true }); fs.rmSync(fb, { force: true }); }
}

export function energyCheck(python, aPfm, bPfm, { region, center, rmax, bins, exposure } = {}) {
  const args = [aPfm, bPfm];
  if (region) args.push("--region", region.join(","));
  if (center) args.push("--center", center.join(","));
  if (rmax) args.push("--rmax", String(rmax));
  if (bins) args.push("--bins", String(bins));
  if (exposure) args.push("--exposure", String(exposure));
  return JSON.parse(runPy(python, ENERGY_PY, args).stdout);
}

// ─── manifest の組み立て（純関数。単体テストあり）─────────────────────────
/** phase 名 + 撮れたファイル → manifest の phases の 1 要素。 */
export function phaseManifest(name, spec, files, extra = {}) {
  const base = { files, region: spec.region ?? null };
  if (name === "ghost") return { ...base, peakIndex: spec.visibleFrames - 1, bgCount: spec.bgCount ?? 4, ...extra };
  if (name === "ssim") return { ...base, refCount: spec.refCount ?? 4, ...extra };
  return { ...base, ...extra };
}

/** drive（entity の回転・カメラのパン）の k フレーム目の姿勢。 */
export function driveState(drive, k, cam) {
  const r = (drive.rotationPerFrame ?? [0, 0, 0]).map((d) => d * k);
  const pan = (drive.cameraPanPerFrame ?? [0, 0, 0]).map((d) => d * k);
  return {
    rotation: r,
    camera: cam ? { position: cam.position.map((v, i) => v + pan[i]), target: cam.target.map((v, i) => v + pan[i]) } : null,
  };
}

// ─── エンジンの操作 ───────────────────────────────────────────────────────
async function openScene(eng, scene) {
  for (let i = 0; i < 600; i++) {
    const p = await eng.call("ping");
    if (p.currentScene === scene && !p.sceneLoading) return p;
    try { await eng.call("open_scene", { path: scene }, 300_000); } catch (e) { if (!/already in progress/.test(e.message)) throw e; }
    await sleep(500);
  }
  throw new Error(`シーンが開かない: ${scene}`);
}

async function applySettings(eng, settings) {
  if (!settings) return;
  if (settings.taa) await eng.call("set_taa", settings.taa);
  if (settings.post && Object.keys(settings.post).length) await eng.call("set_post_process", settings.post);
  if (settings.upscaler) await eng.call("set_upscaler", settings.upscaler);
}

class Rig {
  constructor(eng, cam, common, outDir) { this.eng = eng; this.cam = cam; this.dt = common.dt; this.outDir = outDir; this.n = 0; }
  setCamera(cam) { this.cam = cam; return this.eng.call("set_editor_camera", { position: cam.position, target: cam.target }); }
  async hide(entity, pos) { await this.eng.call("set_transform", { name: entity, position: pos }); }
  async rotate(entity, rotation) { await this.eng.call("set_transform", { name: entity, rotation }); }
  /** 時間を進めて（固定 dt・止める）N フレーム。 */
  async advance(n) {
    while (n > 0) {
      const c = Math.min(n, 600);
      await this.eng.call("step_frames", { frames: c, deterministic: true, dt: this.dt, hold: true }, 300_000);
      n -= c;
    }
  }
  /** 1 フレーム進めたそのフレームを PFM で撮る（2 本を続けて送る）。 */
  async capture(tag) {
    const file = path.join(this.outDir, `${tag}_${String(this.n++).padStart(3, "0")}.png`);
    const p1 = this.eng.call("step_frames", { frames: 1, deterministic: true, dt: this.dt, hold: true }, 300_000);
    const p2 = this.eng.call("screenshot_final", { format: "pfm", gizmos: false, path: file }, 300_000);
    const [, r] = await Promise.all([p1, p2]);
    const pfm = r.files?.pfm ?? r.path;
    if (!pfm || !fs.existsSync(pfm)) throw new Error(`PFM が書かれていない: ${JSON.stringify(r).slice(0, 200)}`);
    return pfm;
  }
}

async function runPhases(rig, sc, entry, common, want, python, tmpDir, settings) {
  const out = {};
  const phases = sc.phases ?? {};
  const camOf = () => ({ position: entry.camera.position, target: entry.camera.target });
  const reset = async () => { await rig.setCamera(camOf()); };

  if (phases.flicker && want("flicker")) {
    const p = phases.flicker;
    await reset();
    for (const e of p.hide ?? []) await rig.hide(e, [0, -50, 0]);
    if (p.drive) await rig.rotate(p.drive.entity, [0, 0, 0]);
    await rig.advance(common.warmupFrames);
    const files = [];
    // 収束のための助走。drive があれば回し続ける（遮蔽解除の負荷をかけた状態で測る）
    for (let k = 0; k < common.settleFrames; k++) {
      if (p.drive) await rig.rotate(p.drive.entity, driveState(p.drive, k).rotation);
      await rig.advance(1);
    }
    for (let k = 0; k < p.frames; k++) {
      if (p.drive) await rig.rotate(p.drive.entity, driveState(p.drive, common.settleFrames + k).rotation);
      files.push(await rig.capture("flicker"));
    }
    out.flicker = phaseManifest("flicker", p, files);
  }

  if (phases.ghost && want("ghost")) {
    const p = phases.ghost;
    await reset();
    await rig.hide(p.entity, p.visiblePosition);
    if (phases.flicker?.drive) await rig.rotate(phases.flicker.drive.entity, [0, 0, 0]);
    await rig.advance(common.warmupFrames + common.settleFrames);
    const files = [];
    for (let k = 0; k < p.visibleFrames; k++) files.push(await rig.capture("ghost"));
    await rig.hide(p.entity, p.hiddenPosition);
    for (let k = 0; k < p.afterFrames; k++) files.push(await rig.capture("ghost"));
    out.ghost = phaseManifest("ghost", p, files);
  }

  if (phases.ssim && want("ssim")) {
    const p = phases.ssim;
    await reset();
    for (const e of phases.flicker?.hide ?? []) await rig.hide(e, [0, -50, 0]);
    await rig.advance(common.warmupFrames + common.settleFrames);
    await rig.setCamera(p.cut);          // カット
    const files = [];
    for (let k = 0; k < p.frames; k++) files.push(await rig.capture("ssim"));
    out.ssim = phaseManifest("ssim", p, files);
  }
  return out;
}

async function runEnergy(rig, sc, entry, common, python, eng) {
  const en = sc.energy;
  const lead = en.leadFrames ?? 0;
  const cam0 = { position: entry.camera.position, target: entry.camera.target };
  const one = async (side) => {
    await rig.setCamera(cam0);
    if (en.drive?.entity) await rig.rotate(en.drive.entity, [0, 0, 0]);
    await applySettings(eng, { taa: side.taa, post: side.post ?? {} });
    await rig.advance(common.warmupFrames);
    for (let k = 0; k < lead; k++) {
      const st = driveState(en.drive ?? {}, k + 1, cam0);
      if (en.drive?.entity) await rig.rotate(en.drive.entity, st.rotation);
      if (st.camera) await rig.setCamera(st.camera);
      await rig.advance(1);
    }
    const st = driveState(en.drive ?? {}, lead + 1, cam0);
    if (en.drive?.entity) await rig.rotate(en.drive.entity, st.rotation);
    if (st.camera) await rig.setCamera(st.camera);
    return rig.capture(side === en.a ? "energy_a" : "energy_b");
  };
  const a = await one(en.a);
  const b = await one(en.b);
  let center = en.center ?? null;
  if (!center && en.centerEntity) {
    await rig.setCamera(cam0);
    await rig.advance(2);
    const pr = await eng.call("project_world_to_screen", { name: en.centerEntity });
    center = [pr.x / pr.width, pr.y / pr.height];
  }
  // 元に戻す（同じセッションで続けて走らせても影響しないように、既定の ON / OFF はシーンの値）
  return { a, b, center, args: { region: en.region, center, rmax: en.rmax, bins: en.bins, exposure: common.exposure } };
}

// ─── 1 シーン ─────────────────────────────────────────────────────────────
async function runScene(name, args, cfg, entries, python, outRoot, tmpDir) {
  const sc = cfg.scenes[name];
  if (!sc) throw new Error(`手順が無いシーン: ${name}（${Object.keys(cfg.scenes).join(", ")}）`);
  const entry = entries.find((e) => e.name === name);
  if (!entry) throw new Error(`tests/golden/entries/${name}.json が無い`);
  const common = cfg.common;
  const wantSet = args.phases ? new Set(args.phases) : null;
  const want = (p) => !wantSet || wantSet.has(p);

  const srcProject = resolveProject(entry.project);
  if (!srcProject) throw new Error(`プロジェクトが見つからない: ${entry.project}`);
  const projCopy = path.join(tmpDir, `proj_${name}`);
  fs.rmSync(projCopy, { recursive: true, force: true });
  fs.cpSync(srcProject, projCopy, { recursive: true });

  const results = [];
  for (let rep = 0; rep < args.repeat; rep++) {
    const outDir = path.join(outRoot, name, `run${rep}`);
    fs.mkdirSync(outDir, { recursive: true });
    const exe = args.exe ?? findDefaultExe();
    const eng = await launchEngine({ exe, project: projCopy, scene: entry.scene, port: args.port ?? undefined });
    let conn;
    try {
      conn = await connect(eng.port);
      await openScene(conn, entry.scene);
      await applySettings(conn, args.settings);
      const rig = new Rig(conn, { position: entry.camera.position, target: entry.camera.target }, common, outDir);
      const t0 = Date.now();
      const manifestPhases = await runPhases(rig, sc, entry, common, want, python, tmpDir, args.settings);
      const res = { scene: name, run: rep, exe: path.relative(REPO_ROOT, exe).replace(/\\/g, "/"), settings: args.settings ?? null };
      if (Object.keys(manifestPhases).length) {
        const m = analyzeManifest(python, { exposure: common.exposure, phases: manifestPhases }, tmpDir);
        Object.assign(res, { flicker: m.flicker, ghost: m.ghost, ssim: m.ssim });
      }
      if (sc.energy && want("energy")) {
        const e = await runEnergy(rig, sc, entry, common, python, conn);
        res.energy = energyCheck(python, e.a, e.b, e.args);
      }
      res.seconds = Number(((Date.now() - t0) / 1000).toFixed(1));
      results.push(res);
    } finally {
      try { conn?.close(); } catch { /* 既に切れている */ }
      eng.kill();
      // 止めたエンジンのポートが閉じるのを待つ（次の起動が「空きポートが無い」で落ちない）
      for (let i = 0; i < 100 && (await portOpen(eng.port, 150)); i++) await sleep(300);
    }
  }
  const out = { scene: name, runs: results };
  if (results.length > 1) {
    out.determinism = [];
    for (let i = 1; i < results.length; i++) {
      const strip = (r) => ({ flicker: r.flicker, ghost: r.ghost, ssim: r.ssim, energy: r.energy ? { total: r.energy.total } : undefined });
      out.determinism.push(compareResults(python, strip(results[0]), strip(results[i]), tmpDir));
    }
  }
  return out;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || !args.scenes.length) {
    console.log("node tools/bench/temporal_metrics.mjs --scene taa_torture|dof_chart|mb_wheel|hdr_range|all [--phase flicker,ghost,ssim,energy] [--repeat 2] [--out dir] [--json file] [--exe path] [--port N] [--settings JSON] [--keep-frames]");
    return args.help ? 0 : 1;
  }
  const cfg = loadScenarios();
  const names = args.scenes.includes("all") ? Object.keys(cfg.scenes) : args.scenes;
  const entries = loadGoldenEntries(GOLDEN_DIR).scenes;
  const python = findPython(args.python);
  const outRoot = path.resolve(args.out ?? path.join(REPO_ROOT, "build", "temporal-out"));
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "uno-temporal-"));
  const all = { version: 1, generatedAt: new Date().toISOString(), scenes: [] };
  let fail = 0;
  try {
    for (const n of names) {
      console.log(`== ${n}`);
      try {
        const r = await runScene(n, args, cfg, entries, python, outRoot, tmpDir);
        all.scenes.push(r);
        const r0 = r.runs[0];
        if (r0.flicker) console.log(`  flicker  meanStd=${r0.flicker.meanStd.toFixed(5)} p95=${r0.flicker.p95Std.toFixed(5)} level=${r0.flicker.meanLevel.toFixed(3)}`);
        if (r0.ghost) console.log(`  ghost    meanRatio=${r0.ghost.meanRatio.toFixed(4)} halfLife=${r0.ghost.halfLifeFrames ?? ">" + r0.ghost.ratio.length}f`);
        if (r0.ssim) console.log(`  ssim     first=${r0.ssim.firstFrame.toFixed(4)} min=${r0.ssim.min.toFixed(4)} mean=${r0.ssim.mean.toFixed(4)}`);
        if (r0.energy) console.log(`  energy   ratio=${r0.energy.total.ratio?.toFixed(4)}${r0.energy.profile ? ` halfR a/b=${r0.energy.profile.halfRadius.a?.toFixed(4)}/${r0.energy.profile.halfRadius.b?.toFixed(4)}` : ""}`);
        for (const d of r.determinism ?? []) console.log(`  決定論   maxRelDiff=${(d.maxRelDiff * 100).toFixed(3)}%（${d.worst}）${d.within1pct ? "1% 以内" : "1% 超"}`);
      } catch (e) {
        fail++;
        console.error(`  失敗: ${e.message}`);
        all.scenes.push({ scene: n, error: e.message });
      }
    }
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
  if (!args.keepFrames) {
    for (const n of names) for (let i = 0; i < args.repeat; i++) {
      const d = path.join(outRoot, n, `run${i}`);
      if (fs.existsSync(d)) for (const f of fs.readdirSync(d)) if (f.endsWith(".pfm") || f.endsWith(".png")) fs.rmSync(path.join(d, f), { force: true });
    }
  }
  const jsonPath = path.resolve(args.json ?? path.join(outRoot, "temporal_metrics.json"));
  fs.mkdirSync(path.dirname(jsonPath), { recursive: true });
  fs.writeFileSync(jsonPath, JSON.stringify(all, null, 2) + "\n");
  console.log(`\nJSON: ${jsonPath}`);
  return fail ? 1 : 0;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main().then((c) => process.exit(c), (e) => { console.error(e); process.exit(1); });
