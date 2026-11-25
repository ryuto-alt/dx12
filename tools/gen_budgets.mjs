// 性能・VRAM 予算の生成器（W05-34 γ1）。
//
// 入力（数値の唯一の出どころ。メインが PROGRAM_PLAN §3 の表から書く）:
//   budgets/quality_presets.json
// 出力（手で編集しない。生成物のコミットはメインが M5 の直後に行う）:
//   src/renderer/QualityPresetTable.inc                       C++ 用（X マクロ。W1-C7 の QualityPreset が読む）
//   tools/mcp-server/testing/budgets/<preset>.json            perf_gate（NB1）が読む
//
// 使い方:
//   node tools/gen_budgets.mjs                  生成して書き込む
//   node tools/gen_budgets.mjs --check          入力の検証と、生成物が入力と一致するかの検査（ctest GenBudgetsCheck）
//   node tools/gen_budgets.mjs --check-leaves   上に加えて、各行の leaves が GpuTimer の葉に存在するかを検査（W05-26 の後に有効化）
//   node tools/gen_budgets.mjs --example        入力 JSON の見本を標準出力へ（スキーマの正。メインはこれを土台にする）
//   node tools/gen_budgets.mjs --self-test      見本で生成・検証の自己テスト（ファイルは書かない）
//   --presets <path>  入力の場所を変える（既定 budgets/quality_presets.json）
//   --root <dir>      リポジトリのルートを変える
//   --out-inc <path> / --out-dir <dir>   出力先を変える
//
// 入力が無いとき: --check は終了コード 77（ctest の SKIP）、生成は 2。
// 数値を TS / C++ に手で書かない。値を変えるときは入力 JSON を直して再生成する。
//
// ---- 入力 JSON のスキーマ（schema: 1）----
// {
//   "schema": 1,
//   "presets": {
//     "<preset>": {                          epic_1080p60 | high_1080p60 | cinematic_1080p30
//       "label": "Epic", "targetFps": 60,
//       "output": [1920, 1080], "p95Ms": 15.5, "p99Ms": 16.6,
//       "rows": [                            全アップスケーラ列で共通の GPU の行（ms。葉のスコープ）
//         { "id": "DepthPrepass", "ms": 0.5, "leaves": ["depthPrepass"],   // leaves 省略時は id の先頭を小文字にした 1 本
//           "degrade": "PDO を OFF にする", "owner": "既存、MH-13", "leavesPending": false }
//       ],
//       "upscalerColumns": {                 列ごとの差分。dlss / taau / taaNative のうち必要なもの
//         "dlss":      { "gate": "gate",          "renderScale": 0.6667, "rows": [ { "id": "Upscale", "ms": 1.1, "leaves": ["upscale"] } ] },
//         "taau":      { "gate": "gateAfterW2BD", "renderScale": 0.6667, "rows": [ ... ] },
//         "taaNative": { "gate": "record",        "renderScale": 1.0,    "rows": [ ... ] }
//       },
//       "vfx": {                             VFX の行（W1-C1 が使う）。rows の中の Vfx 系の葉と同じ ms を VfxSim / VfxDraw に分ける
//         "rows": [ { "id": "VfxSim", "ms": 0.4, "leaves": ["vfxSim"], "leavesPending": true }, { "id": "VfxDraw", ... } ],
//         "emissionMultiplier": 1.0, "maxParticles": 20000, "peakMs": 2.3
//       },
//       "vram": { "capGb": 6.0, "totalGb": 4.8 },
//       "cpu": { "gameThreadP95Ms": 6.0, "renderRecordP95Ms": 4.0, "editorUiP95Ms": 1.5 },
//       "expectedTotalMs": { "dlss": 15.4 }  任意。列の合計がこの値と 0.05ms 以内で一致しなければ赤
//     }
//   }
// }
// gate の意味: gate = perf_gate の合否に使う（W1 の合否は DLSS 列）、gateAfterW2BD = W2-BD（TAA v2）の後のゲートから合否に使う、
//              record = 記録だけ（合否にしない）。
import { readFileSync, writeFileSync, existsSync, mkdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const GATES = ["gate", "gateAfterW2BD", "record"];
const COLUMNS = ["dlss", "taau", "taaNative"];
const PRESET_ID_RE = /^[a-z][a-z0-9_]*$/;
const ROW_ID_RE = /^[A-Za-z][A-Za-z0-9_]*$/;

const round3 = (x) => Math.round(x * 10000) / 10000;
const num = (x) => {
  const s = String(round3(x));
  return /^-?\d+$/.test(s) ? `${s}.0` : s;
};
const lowerFirst = (s) => s[0].toLowerCase() + s.slice(1);
const rowLeaves = (r) => (Array.isArray(r.leaves) && r.leaves.length ? r.leaves : [lowerFirst(r.id)]);

// ---- 検証 ----
export function validate(input) {
  const errs = [];
  const e = (m) => errs.push(m);
  const isNum = (v) => typeof v === "number" && Number.isFinite(v);
  if (!input || typeof input !== "object") return ["入力が JSON のオブジェクトではない"];
  if (input.schema !== 1) e(`schema は 1 であること（実際: ${input.schema}）`);
  const presets = input.presets;
  if (!presets || typeof presets !== "object" || !Object.keys(presets).length) {
    e("presets が空");
    return errs;
  }
  const checkRows = (rows, where, ids) => {
    if (!Array.isArray(rows)) { e(`${where}: rows が配列でない`); return; }
    for (const [i, r] of rows.entries()) {
      const w = `${where}.rows[${i}]`;
      if (!r || typeof r.id !== "string" || !ROW_ID_RE.test(r.id)) { e(`${w}: id が不正`); continue; }
      if (ids.has(r.id)) e(`${w}: id ${r.id} が重複`);
      ids.add(r.id);
      if (!isNum(r.ms) || r.ms < 0) e(`${w}(${r.id}): ms は 0 以上の数`);
      if (r.leaves !== undefined && (!Array.isArray(r.leaves) || !r.leaves.length || r.leaves.some((l) => typeof l !== "string" || !l)))
        e(`${w}(${r.id}): leaves は空でない文字列の配列`);
    }
  };
  for (const [pid, p] of Object.entries(presets)) {
    const w = `presets.${pid}`;
    if (!PRESET_ID_RE.test(pid)) e(`${w}: プリセット名が不正`);
    if (!p || typeof p !== "object") { e(`${w}: オブジェクトでない`); continue; }
    if (!isNum(p.targetFps) || p.targetFps <= 0) e(`${w}: targetFps`);
    if (!Array.isArray(p.output) || p.output.length !== 2 || !p.output.every((v) => Number.isInteger(v) && v > 0)) e(`${w}: output は [幅, 高さ]`);
    if (!isNum(p.p95Ms) || !isNum(p.p99Ms) || p.p99Ms < p.p95Ms) e(`${w}: p95Ms / p99Ms（p99 >= p95）`);
    const ids = new Set();
    checkRows(p.rows, w, ids);
    const cols = p.upscalerColumns;
    if (!cols || typeof cols !== "object" || !Object.keys(cols).length) e(`${w}: upscalerColumns が空`);
    else {
      for (const [cn, c] of Object.entries(cols)) {
        const cw = `${w}.upscalerColumns.${cn}`;
        if (!COLUMNS.includes(cn)) e(`${cw}: 列名は ${COLUMNS.join(" / ")}`);
        if (!GATES.includes(c?.gate)) e(`${cw}: gate は ${GATES.join(" / ")}`);
        if (!isNum(c?.renderScale) || c.renderScale <= 0 || c.renderScale > 1) e(`${cw}: renderScale は (0,1]`);
        checkRows(c?.rows, cw, new Set(ids));
      }
      const gates = Object.values(cols).filter((c) => c?.gate === "gate").length;
      if (gates > 1) e(`${w}: gate の列は 1 つまで`);
    }
    if (p.vfx) {
      if (!isNum(p.vfx.emissionMultiplier) || p.vfx.emissionMultiplier <= 0) e(`${w}.vfx: emissionMultiplier`);
      if (!Number.isInteger(p.vfx.maxParticles) || p.vfx.maxParticles <= 0) e(`${w}.vfx: maxParticles`);
      if (!isNum(p.vfx.peakMs)) e(`${w}.vfx: peakMs`);
      const vids = new Set(ids);
      checkRows(p.vfx.rows, `${w}.vfx`, vids);
      for (const need of ["VfxSim", "VfxDraw"])
        if (!(p.vfx.rows ?? []).some((r) => r.id === need)) e(`${w}.vfx: ${need} の行が必要`);
    } else e(`${w}: vfx が無い（VFX の行とアップスケーラの列は両方出力する）`);
    if (!p.vram || !isNum(p.vram.capGb)) e(`${w}.vram.capGb`);
    if (!p.cpu || !isNum(p.cpu.gameThreadP95Ms)) e(`${w}.cpu.gameThreadP95Ms`);
    if (p.expectedTotalMs && cols) {
      for (const [cn, want] of Object.entries(p.expectedTotalMs)) {
        if (!cols[cn]) { e(`${w}.expectedTotalMs.${cn}: その列が無い`); continue; }
        const got = columnTotal(p, cols[cn]);
        if (Math.abs(got - want) > 0.05) e(`${w}: 列 ${cn} の合計 ${num(got)} が expectedTotalMs ${want} と 0.05ms 以上ずれる`);
      }
    }
  }
  return errs;
}

function columnTotal(p, col) {
  return [...p.rows, ...(col.rows ?? [])].reduce((s, r) => s + r.ms, 0);
}

// ---- 生成 ----
export function buildBudgetJson(pid, p) {
  const columns = {};
  for (const [cn, c] of Object.entries(p.upscalerColumns)) {
    const rows = [...p.rows, ...(c.rows ?? [])];
    columns[cn] = {
      gate: c.gate,
      renderScale: c.renderScale,
      totalMs: round3(columnTotal(p, c)),
      rows: rows.map((r) => ({ id: r.id, ms: round3(r.ms), leaves: rowLeaves(r) })),
    };
  }
  return {
    _generated: "tools/gen_budgets.mjs が budgets/quality_presets.json から生成。手で編集しない",
    preset: pid,
    label: p.label ?? pid,
    targetFps: p.targetFps,
    output: p.output,
    p95Ms: p.p95Ms,
    p99Ms: p.p99Ms,
    columns,
    vfx: {
      emissionMultiplier: p.vfx.emissionMultiplier,
      maxParticles: p.vfx.maxParticles,
      peakMs: p.vfx.peakMs,
      rows: p.vfx.rows.map((r) => ({ id: r.id, ms: round3(r.ms), leaves: rowLeaves(r), leavesPending: !!r.leavesPending })),
    },
    vram: p.vram,
    cpu: p.cpu,
  };
}

const cstr = (s) => JSON.stringify(s);
const cident = (s) => s.replace(/[^A-Za-z0-9_]/g, "_");

export function buildInc(input) {
  const L = [];
  L.push("// 自動生成: tools/gen_budgets.mjs（入力 budgets/quality_presets.json）。手で編集しない。");
  L.push("// 使う側が次のマクロを定義してから include する。使わないものは定義しなくてよい。");
  L.push("//   UNO_QP_PRESET(id, label, targetFps, outW, outH, p95Ms, p99Ms)");
  L.push("//   UNO_QP_ROW(preset, column, row, ms)        column: dlss / taau / taaNative。row は GpuTimer の葉に対応する行名");
  L.push("//   UNO_QP_UPSCALER(preset, column, gate, renderScale, totalMs)   gate: Gate / GateAfterW2BD / Record");
  L.push("//   UNO_QP_VFX(preset, emissionMultiplier, maxParticles, vfxSimMs, vfxDrawMs, peakMs)");
  L.push("//   UNO_QP_VRAM(preset, capGb)");
  const ifdef = (name, body) => { L.push(`#ifdef ${name}`); body(); L.push("#endif"); };
  const gateId = { gate: "Gate", gateAfterW2BD: "GateAfterW2BD", record: "Record" };
  const entries = Object.entries(input.presets);
  ifdef("UNO_QP_PRESET", () => {
    for (const [pid, p] of entries)
      L.push(`UNO_QP_PRESET(${cident(pid)}, ${cstr(p.label ?? pid)}, ${num(p.targetFps)}, ${p.output[0]}, ${p.output[1]}, ${num(p.p95Ms)}, ${num(p.p99Ms)})`);
  });
  ifdef("UNO_QP_UPSCALER", () => {
    for (const [pid, p] of entries)
      for (const [cn, c] of Object.entries(p.upscalerColumns))
        L.push(`UNO_QP_UPSCALER(${cident(pid)}, ${cn}, ${gateId[c.gate]}, ${num(c.renderScale)}, ${num(columnTotal(p, c))})`);
  });
  ifdef("UNO_QP_ROW", () => {
    for (const [pid, p] of entries)
      for (const [cn, c] of Object.entries(p.upscalerColumns))
        for (const r of [...p.rows, ...(c.rows ?? [])])
          L.push(`UNO_QP_ROW(${cident(pid)}, ${cn}, ${cident(r.id)}, ${num(r.ms)})`);
  });
  ifdef("UNO_QP_VFX", () => {
    for (const [pid, p] of entries) {
      const ms = (id) => p.vfx.rows.find((r) => r.id === id).ms;
      L.push(`UNO_QP_VFX(${cident(pid)}, ${num(p.vfx.emissionMultiplier)}, ${p.vfx.maxParticles}, ${num(ms("VfxSim"))}, ${num(ms("VfxDraw"))}, ${num(p.vfx.peakMs)})`);
    }
  });
  ifdef("UNO_QP_VRAM", () => {
    for (const [pid, p] of entries) L.push(`UNO_QP_VRAM(${cident(pid)}, ${num(p.vram.capGb)})`);
  });
  return L.join("\n") + "\n";
}

export function generate(input) {
  const files = new Map(); // 相対パス（root 基準 or out 指定）ではなく論理名 → 内容
  files.set("inc", buildInc(input));
  for (const [pid, p] of Object.entries(input.presets))
    files.set(`json:${pid}`, JSON.stringify(buildBudgetJson(pid, p), null, 2) + "\n");
  return files;
}

// ---- 葉の存在検査 ----
export function loadGpuTimerLeaves(root) {
  const f = join(root, "src", "graphics", "GpuTimer.cpp");
  if (!existsSync(f)) return null;
  const src = readFileSync(f, "utf8");
  return new Set([...src.matchAll(/return\s+"([A-Za-z0-9_]+)"/g)].map((m) => m[1]));
}

export function checkLeaves(input, leaves) {
  const errs = [];
  for (const [pid, p] of Object.entries(input.presets)) {
    const rows = [...p.rows, ...Object.values(p.upscalerColumns).flatMap((c) => c.rows ?? []), ...p.vfx.rows];
    for (const r of rows) {
      if (r.leavesPending) continue; // VFX の行は W1-BD4 の後に検査を有効にする
      for (const l of rowLeaves(r)) if (!leaves.has(l)) errs.push(`${pid}: 行 ${r.id} の葉 "${l}" が GpuTimer に無い`);
    }
  }
  return [...new Set(errs)];
}

// ---- 見本（スキーマの正。数値は PROGRAM_PLAN §3.1 の Epic の表）----
export function exampleInput() {
  const R = (id, ms, extra = {}) => ({ id, ms, ...extra });
  return {
    schema: 1,
    presets: {
      epic_1080p60: {
        label: "Epic", targetFps: 60, output: [1920, 1080], p95Ms: 15.5, p99Ms: 16.6,
        rows: [
          R("DepthPrepass", 0.5, { degrade: "PDO を OFF にする" }),
          R("ClusterCull", 0.2, { leaves: ["hiZ", "clusterCull", "vgCull"] }),
          R("Raytracing", 0.25),
          R("MainScene", 3.3, { degrade: "ディテールを距離で切る" }),
          R("Ddgi", 0.7),
        ],
        upscalerColumns: {
          dlss: { gate: "gate", renderScale: 0.6667, rows: [R("Upscale", 1.1, { leaves: ["upscale"] })] },
          taau: { gate: "gateAfterW2BD", renderScale: 0.6667, rows: [R("Upscale", 0.65, { leaves: ["upscale"] })] },
          taaNative: { gate: "record", renderScale: 1.0, rows: [R("Upscale", 0.6, { leaves: ["upscale"] })] },
        },
        vfx: {
          rows: [R("VfxSim", 0.5, { leaves: ["particles"], leavesPending: true }), R("VfxDraw", 0.8, { leaves: ["particles"], leavesPending: true })],
          emissionMultiplier: 1.0, maxParticles: 20000, peakMs: 2.3,
        },
        vram: { capGb: 6.0, totalGb: 4.8 },
        cpu: { gameThreadP95Ms: 6.0, renderRecordP95Ms: 4.0, editorUiP95Ms: 1.5 },
      },
    },
  };
}

// ---- CLI ----
function parseArgs(argv) {
  const o = { mode: "generate", checkLeaves: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--check") o.mode = "check";
    else if (a === "--check-leaves") { o.mode = "check"; o.checkLeaves = true; }
    else if (a === "--example") o.mode = "example";
    else if (a === "--self-test") o.mode = "selftest";
    else if (["--presets", "--root", "--out-inc", "--out-dir"].includes(a)) o[a.slice(2).replace(/-(\w)/g, (_, c) => c.toUpperCase())] = argv[++i];
    else { console.error(`不明な引数: ${a}`); process.exit(2); }
  }
  return o;
}

function selfTest() {
  let fail = 0;
  const t = (name, cond) => { if (!cond) { fail++; console.error(`NG: ${name}`); } };
  const ex = exampleInput();
  t("見本が検証を通る", validate(ex).length === 0);
  const files = generate(ex);
  t("生成が決定的", [...files.values()].join() === [...generate(ex).values()].join());
  const b = JSON.parse(files.get("json:epic_1080p60"));
  t("DLSS 列の合計", Math.abs(b.columns.dlss.totalMs - 6.05) < 1e-9 || Math.abs(b.columns.dlss.totalMs - 6.05) < 0.0011);
  t("3 列とも出る", Object.keys(b.columns).join() === "dlss,taau,taaNative");
  t("VFX の行が出る", b.vfx.rows.map((r) => r.id).join() === "VfxSim,VfxDraw");
  t("inc に VFX とアップスケーラ", /UNO_QP_VFX\(epic_1080p60/.test(files.get("inc")) && /UNO_QP_UPSCALER\(epic_1080p60, taaNative, Record/.test(files.get("inc")));
  const bad = structuredClone(ex);
  delete bad.presets.epic_1080p60.vfx;
  t("vfx 欠落を検出", validate(bad).length > 0);
  const bad2 = structuredClone(ex);
  bad2.presets.epic_1080p60.upscalerColumns.taau.gate = "gate";
  t("gate 列の重複を検出", validate(bad2).some((m) => m.includes("gate の列は 1 つまで")));
  const bad3 = structuredClone(ex);
  bad3.presets.epic_1080p60.expectedTotalMs = { dlss: 99 };
  t("expectedTotalMs のずれを検出", validate(bad3).length > 0);
  const lv = new Set(["depthPrepass", "hiZ", "clusterCull", "vgCull", "raytracing", "mainScene", "ddgi", "upscale"]);
  t("葉検査（全部ある）", checkLeaves(ex, lv).length === 0);
  lv.delete("ddgi");
  t("葉検査（欠落を検出）", checkLeaves(ex, lv).length === 1);
  if (fail) { console.error(`self-test 失敗: ${fail} 件`); return 1; }
  console.log("self-test OK");
  return 0;
}

function main() {
  const o = parseArgs(process.argv.slice(2));
  if (o.mode === "example") { process.stdout.write(JSON.stringify(exampleInput(), null, 2) + "\n"); return 0; }
  if (o.mode === "selftest") return selfTest();

  const root = resolve(o.root ?? join(HERE, ".."));
  const presetsPath = resolve(o.presets ?? join(root, "budgets", "quality_presets.json"));
  const incPath = resolve(o.outInc ?? join(root, "src", "renderer", "QualityPresetTable.inc"));
  const outDir = resolve(o.outDir ?? join(root, "tools", "mcp-server", "testing", "budgets"));

  if (!existsSync(presetsPath)) {
    console.log(`${o.mode === "check" ? "SKIP" : "エラー"}: 入力 ${presetsPath} がまだ無い（メインが M5 で置く）`);
    return o.mode === "check" ? 77 : 2;
  }
  let input;
  try { input = JSON.parse(readFileSync(presetsPath, "utf8")); }
  catch (err) { console.error(`入力の JSON が読めない: ${err.message}`); return 1; }
  const errs = validate(input);
  if (errs.length) { console.error("入力の検証に失敗:\n" + errs.map((m) => "  - " + m).join("\n")); return 1; }

  const files = generate(input);
  const targets = new Map([["inc", incPath], ...Object.keys(input.presets).map((pid) => [`json:${pid}`, join(outDir, `${pid}.json`)])]);

  if (o.mode === "check") {
    const bad = [];
    for (const [k, path] of targets) {
      if (!existsSync(path)) bad.push(`${path} が無い（node tools/gen_budgets.mjs で生成する）`);
      else if (readFileSync(path, "utf8").replace(/\r\n/g, "\n") !== files.get(k)) bad.push(`${path} が入力と食い違う（再生成が必要）`);
    }
    if (o.checkLeaves) {
      const leaves = loadGpuTimerLeaves(root);
      if (!leaves) bad.push("src/graphics/GpuTimer.cpp が読めない");
      else bad.push(...checkLeaves(input, leaves));
    }
    if (bad.length) { console.error("GenBudgetsCheck 失敗:\n" + bad.map((m) => "  - " + m).join("\n")); return 1; }
    console.log(`GenBudgetsCheck OK（${targets.size} ファイル${o.checkLeaves ? "、葉の検査あり" : ""}）`);
    return 0;
  }

  mkdirSync(dirname(incPath), { recursive: true });
  mkdirSync(outDir, { recursive: true });
  for (const [k, path] of targets) {
    writeFileSync(path, files.get(k), "utf8");
    console.log(`書き込み: ${path}`);
  }
  return 0;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) process.exit(main());
