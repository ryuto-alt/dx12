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
//       "vfx": {                             VFX の予算。VfxSim / VfxDraw の行が VFX の ms の唯一の表現
//                                            （全アップスケーラ列の rows と totalMs に自動で加わる。rows に集約行 "Vfx" を置くと赤）
//         "rows": [ { "id": "VfxSim", "ms": 0.4, "leaves": ["vfxSim"], "leavesPending": true }, { "id": "VfxDraw", "leaves": ["vfxDraw"], ... } ],
//         "emissionMultiplier": 1.0, "maxParticles": 20000, "peakMs": 2.3
//       },
//       "vram": { "capGb": 6.0, "marginGb": 0.6, "precheckGb": 6.5, "totalGb": 4.8 },
//                                            capGb: 固定の上限。null なら固定上限なし（Cinematic = 予算 - marginGb）。
//                                            Epic の実効上限 = min(capGb, 予算 - marginGb)。marginGb: 予算から引く余白。
//                                            precheckGb: 計測の前提確認のしきい値（予算 - 他プロセスの使用量 がこれ未満なら計測を中止）。
//                                            この 3 つは JSON と .inc の両方へ出す（TS / C++ に 0.6 / 6.5 を書かない）
//       "cpu": { "gameThreadP95Ms": 6.0, "renderRecordP95Ms": 4.0, "editorUiP95Ms": 1.5 },
//       "expectedTotalMs": { "dlss": 15.4 }  任意。列の合計（共通の行 + VFX の行 + 列の行）がこの値と 0.05ms 以内で一致しなければ赤
//     }
//   }
// }
// 必須の条件: プリセットは epic_1080p60 / high_1080p60 / cinematic_1080p30 の 3 つ。Epic は dlss / taau / taaNative の 3 列。
//   gate の列はちょうど 1 つで、dlss であること（W1 の合否は DLSS 列）。全ての行に degrade（超えたときの縮退）が要る。
//   degrade / owner は JSON の行と .inc（UNO_QP_DEGRADE）へ出す。
// 行の作り方: MainScene の「重い群」(V2 の重い群 0.3 など) は MainScene の 1 行にまとめる（同じ葉 mainScene を指す行を 2 つ置くと赤。
//   同じ葉の実測値が 2 つの上限で別々に判定されてしまうため）。1 つの葉は、同じ列の中で 1 行からだけ指す。
// leaves は GpuTimer の「葉」（子を持たないスコープ）だけ。親（total / shadows / postFx / particles / rtScreen 等）を指すと --check-leaves が赤。
// leavesPending は vfx.rows の行だけに true を付けられる（W1-BD4 の前の仮。JSON の vfx.rows と列の rows の両方に出る）。
// 同じ id の degrade は、プリセット内で全列・全行を通して 1 つ（.inc の UNO_QP_DEGRADE は行ごとに 1 つのため。食い違うと赤）。
// gate の意味: gate = perf_gate の合否に使う（W1 の合否は DLSS 列）、gateAfterW2BD = W2-BD（TAA v2）の後のゲートから合否に使う、
//              record = 記録だけ（合否にしない）。
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, mkdtempSync, rmSync, realpathSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const GATES = ["gate", "gateAfterW2BD", "record"];
const COLUMNS = ["dlss", "taau", "taaNative"];
const REQUIRED_PRESETS = ["epic_1080p60", "high_1080p60", "cinematic_1080p30"];
const PRESET_ID_RE = /^[a-z][a-z0-9_]*$/;
const ROW_ID_RE = /^[A-Za-z][A-Za-z0-9_]*$/;

const round4 = (x) => Math.round(x * 10000) / 10000;
const num = (x) => {
  const s = String(round4(x));
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
  for (const need of REQUIRED_PRESETS) if (!presets[need]) e(`presets.${need} が必要（perf_gate が testing/budgets/ から読む）`);
  const checkRows = (rows, where, ids, allowPending = false) => {
    if (!Array.isArray(rows)) { e(`${where}: rows が配列でない`); return; }
    for (const [i, r] of rows.entries()) {
      const w = `${where}.rows[${i}]`;
      if (!r || typeof r.id !== "string" || !ROW_ID_RE.test(r.id)) { e(`${w}: id が不正`); continue; }
      if (ids.has(r.id)) e(`${w}: id ${r.id} が重複`);
      ids.add(r.id);
      if (!isNum(r.ms) || r.ms < 0) e(`${w}(${r.id}): ms は 0 以上の数`);
      if (r.leaves !== undefined && (!Array.isArray(r.leaves) || !r.leaves.length || r.leaves.some((l) => typeof l !== "string" || !l)))
        e(`${w}(${r.id}): leaves は空でない文字列の配列`);
      if (typeof r.degrade !== "string" || !r.degrade.trim()) e(`${w}(${r.id}): degrade（超えたときの縮退）が必要`);
      if (r.leavesPending !== undefined && (typeof r.leavesPending !== "boolean" || (r.leavesPending && !allowPending)))
        e(`${w}(${r.id}): leavesPending は vfx.rows の行だけに true を付けられる（葉の検査を外すため）`);
      if (r.owner !== undefined && typeof r.owner !== "string") e(`${w}(${r.id}): owner は文字列`);
    }
  };
  for (const [pid, p] of Object.entries(presets)) {
    const w = `presets.${pid}`;
    if (!PRESET_ID_RE.test(pid)) e(`${w}: プリセット名が不正`);
    if (!p || typeof p !== "object") { e(`${w}: オブジェクトでない`); continue; }
    const errsBefore = errs.length;
    if (p.label !== undefined && typeof p.label !== "string") e(`${w}: label は文字列`);
    if (!isNum(p.targetFps) || p.targetFps <= 0) e(`${w}: targetFps`);
    if (!Array.isArray(p.output) || p.output.length !== 2 || !p.output.every((v) => Number.isInteger(v) && v > 0)) e(`${w}: output は [幅, 高さ]`);
    if (!isNum(p.p95Ms) || !isNum(p.p99Ms) || p.p99Ms < p.p95Ms) e(`${w}: p95Ms / p99Ms（p99 >= p95）`);
    const ids = new Set();
    checkRows(p.rows, w, ids);
    if (Array.isArray(p.rows) && p.rows.some((r) => r?.id === "Vfx")) e(`${w}: rows に集約行 Vfx を置かない（VfxSim / VfxDraw が唯一の表現）`);
    if (p.vfx) {
      const vids = new Set(ids);
      checkRows(p.vfx.rows, `${w}.vfx`, vids, true);
      for (const id of vids) ids.add(id);
    }
    const cols = p.upscalerColumns;
    if (!cols || typeof cols !== "object" || !Object.keys(cols).length) e(`${w}: upscalerColumns が空`);
    else {
      for (const [cn, c] of Object.entries(cols)) {
        const cw = `${w}.upscalerColumns.${cn}`;
        if (!COLUMNS.includes(cn)) e(`${cw}: 列名は ${COLUMNS.join(" / ")}`);
        if (!GATES.includes(c?.gate)) e(`${cw}: gate は ${GATES.join(" / ")}`);
        if (!isNum(c?.renderScale) || c.renderScale <= 0 || c.renderScale > 1) e(`${cw}: renderScale は (0,1]`);
        checkRows(c?.rows, cw, new Set(ids));
        if (Array.isArray(p.rows) && (c?.rows === undefined || Array.isArray(c.rows)) && (!p.vfx || Array.isArray(p.vfx.rows))) {
          const owner = new Map();
          for (const r of columnRows(p, c ?? {})) {
            if (!r || typeof r.id !== "string") continue;
            for (const l of rowLeaves(r)) {
              if (owner.has(l) && owner.get(l) !== r.id)
                e(`${cw}: 葉 "${l}" を行 ${owner.get(l)} と ${r.id} が重複して指す（同じ実測値が 2 つの上限で判定され、合計も二重になる。1 行にまとめる）`);
              else owner.set(l, r.id);
            }
          }
        }
      }
      const gateCols = Object.entries(cols).filter(([, c]) => c?.gate === "gate").map(([n]) => n);
      if (gateCols.length > 1) e(`${w}: gate の列は 1 つまで`);
      if (gateCols.length !== 1) e(`${w}: gate の列がちょうど 1 つ要る（実際: ${gateCols.length}）`);
      else if (gateCols[0] !== "dlss") e(`${w}: gate の列は dlss であること（実際: ${gateCols[0]}）`);
      if (pid === "epic_1080p60")
        for (const need of COLUMNS) if (!cols[need]) e(`${w}: Epic は ${need} 列が必要`);
    }
    if (cols && typeof cols === "object") {
      const dg = new Map();
      const all = [...(Array.isArray(p.rows) ? p.rows : []), ...(Array.isArray(p.vfx?.rows) ? p.vfx.rows : []),
        ...Object.values(cols).flatMap((c) => (Array.isArray(c?.rows) ? c.rows : []))];
      for (const r of all) {
        if (!r || typeof r.id !== "string" || typeof r.degrade !== "string") continue;
        if (dg.has(r.id) && dg.get(r.id) !== r.degrade) e(`${w}: 行 ${r.id} の degrade が列・行で食い違う（.inc の UNO_QP_DEGRADE は行ごとに 1 つ）`);
        else dg.set(r.id, r.degrade);
      }
    }
    if (p.vfx) {
      if (!isNum(p.vfx.emissionMultiplier) || p.vfx.emissionMultiplier <= 0) e(`${w}.vfx: emissionMultiplier`);
      if (!Number.isInteger(p.vfx.maxParticles) || p.vfx.maxParticles <= 0) e(`${w}.vfx: maxParticles`);
      if (!isNum(p.vfx.peakMs)) e(`${w}.vfx: peakMs`);
      for (const need of ["VfxSim", "VfxDraw"])
        if (!(Array.isArray(p.vfx.rows) ? p.vfx.rows : []).some((r) => r?.id === need)) e(`${w}.vfx: ${need} の行が必要`);
    } else e(`${w}: vfx が無い（VFX の行とアップスケーラの列は両方出力する）`);
    if (!p.vram) e(`${w}.vram`);
    else {
      if (p.vram.capGb !== null && (!isNum(p.vram.capGb) || p.vram.capGb <= 0)) e(`${w}.vram.capGb は正の数か null（固定上限なし）`);
      if (!isNum(p.vram.marginGb) || p.vram.marginGb < 0) e(`${w}.vram.marginGb は 0 以上の数`);
      if (!isNum(p.vram.precheckGb) || p.vram.precheckGb <= 0) e(`${w}.vram.precheckGb は正の数`);
    }
    if (!p.cpu || !isNum(p.cpu.gameThreadP95Ms)) e(`${w}.cpu.gameThreadP95Ms`);
    const structOk = errs.length === errsBefore && Array.isArray(p.rows) && Array.isArray(p.vfx?.rows) && cols && typeof cols === "object";
    if (structOk) {
      for (const [cn, c] of Object.entries(cols)) {
        const got = columnTotal(p, c);
        if (c.gate === "gate" && got > p.p95Ms + 1e-9) e(`${w}: gate の列 ${cn} の合計 ${num(got)} が p95Ms ${p.p95Ms} を超える（入力の打ち間違い）`);
      }
    }
    if (p.expectedTotalMs !== undefined && structOk) {
      if (!p.expectedTotalMs || typeof p.expectedTotalMs !== "object") e(`${w}.expectedTotalMs はオブジェクト`);
      else for (const [cn, want] of Object.entries(p.expectedTotalMs)) {
        if (!cols[cn]) { e(`${w}.expectedTotalMs.${cn}: その列が無い`); continue; }
        const got = columnTotal(p, cols[cn]);
        if (Math.abs(got - want) > 0.05) e(`${w}: 列 ${cn} の合計 ${num(got)} が expectedTotalMs ${want} と 0.05ms 以上ずれる`);
      }
    }
  }
  return errs;
}

function columnRows(p, col) {
  return [...p.rows, ...(p.vfx?.rows ?? []), ...(col.rows ?? [])];
}

function columnTotal(p, col) {
  return columnRows(p, col).reduce((s, r) => s + r.ms, 0);
}

// ---- 生成 ----
export function buildBudgetJson(pid, p) {
  const columns = {};
  for (const [cn, c] of Object.entries(p.upscalerColumns)) {
    const rows = columnRows(p, c);
    columns[cn] = {
      gate: c.gate,
      renderScale: c.renderScale,
      totalMs: round4(columnTotal(p, c)),
      rows: rows.map((r) => ({ id: r.id, ms: round4(r.ms), leaves: rowLeaves(r), degrade: r.degrade, owner: r.owner ?? "", leavesPending: !!r.leavesPending })),
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
      rows: p.vfx.rows.map((r) => ({ id: r.id, ms: round4(r.ms), leaves: rowLeaves(r), leavesPending: !!r.leavesPending })),
    },
    vram: { capGb: p.vram.capGb, marginGb: p.vram.marginGb, precheckGb: p.vram.precheckGb, ...(p.vram.totalGb !== undefined ? { totalGb: p.vram.totalGb } : {}) },
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
  L.push("//   UNO_QP_ROW(preset, column, row, ms)        column: dlss / taau / taaNative。row は予算の行名で、GpuTimer::Scope の識別子ではない");
  L.push("//                                              （複数の葉をまとめた行や、スコープの無い行がある。GpuTimer の実測との対応は UNO_QP_LEAF）");
  L.push("//   UNO_QP_LEAF(preset, row, leaf)             行が指す GpuTimer の葉の名前（文字列。1 行に複数ありうる）");
  L.push("// このファイルの用途は VFX とアップスケーラの値の読み出しに限る。予算超過の判定には使わない（判定は perf_gate が JSON で行う）。");
  L.push("//   UNO_QP_UPSCALER(preset, column, gate, renderScale, totalMs)   gate: Gate / GateAfterW2BD / Record");
  L.push("//   UNO_QP_VFX(preset, emissionMultiplier, maxParticles, vfxSimMs, vfxDrawMs, peakMs)");
  L.push("//   UNO_QP_VRAM(preset, hasCap, capGb, marginGb, precheckGb)   hasCap=0 のとき capGb は無視（固定上限なし）。実効上限 = min(capGb, 予算 - marginGb)");
  L.push("//   UNO_QP_DEGRADE(preset, row, text)          行が予算を超えたときの縮退（行ごとに 1 つ。列をまたいで共通）");
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
        for (const r of columnRows(p, c))
          L.push(`UNO_QP_ROW(${cident(pid)}, ${cn}, ${cident(r.id)}, ${num(r.ms)})`);
  });
  ifdef("UNO_QP_LEAF", () => {
    for (const [pid, p] of entries) {
      const seen = new Set();
      for (const r of [...p.rows, ...(p.vfx?.rows ?? []), ...Object.values(p.upscalerColumns).flatMap((c) => c.rows ?? [])]) {
        if (seen.has(r.id)) continue;
        seen.add(r.id);
        for (const l of rowLeaves(r)) L.push(`UNO_QP_LEAF(${cident(pid)}, ${cident(r.id)}, ${cstr(l)})`);
      }
    }
  });
  ifdef("UNO_QP_VFX", () => {
    for (const [pid, p] of entries) {
      const ms = (id) => p.vfx.rows.find((r) => r.id === id).ms;
      L.push(`UNO_QP_VFX(${cident(pid)}, ${num(p.vfx.emissionMultiplier)}, ${p.vfx.maxParticles}, ${num(ms("VfxSim"))}, ${num(ms("VfxDraw"))}, ${num(p.vfx.peakMs)})`);
    }
  });
  ifdef("UNO_QP_VRAM", () => {
    for (const [pid, p] of entries) {
      const v = p.vram;
      L.push(`UNO_QP_VRAM(${cident(pid)}, ${v.capGb === null ? 0 : 1}, ${num(v.capGb ?? 0)}, ${num(v.marginGb)}, ${num(v.precheckGb)})`);
    }
  });
  ifdef("UNO_QP_DEGRADE", () => {
    for (const [pid, p] of entries) {
      const seen = new Set();
      for (const r of [...p.rows, ...(p.vfx?.rows ?? []), ...Object.values(p.upscalerColumns).flatMap((c) => c.rows ?? [])]) {
        if (seen.has(r.id)) continue;
        seen.add(r.id);
        L.push(`UNO_QP_DEGRADE(${cident(pid)}, ${cident(r.id)}, ${cstr(r.degrade)})`);
      }
    }
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
// GpuTimer.cpp から (葉, 親) を読む。2 つの書き方に対応する。
//   旧（T-00 より前）: switch の  return "name";  を全部集める。入れ子の情報が無いので、既知の親（LEGACY_PARENTS）を除いたものを葉とする。
//   新（W05-26 の ScopeInfo 表）: 表の 1 行 { "name", Scope::Parent, "alias" } を読み、parent に名前を持つ行を親とする。子を持たないものが葉。
//     alias（RtReflect=RtReflection など）は、葉の別名として葉の集合へ足す。Scope の識別子から名前への対応は小文字化で取る（PostFX → postFx）。
// 実際の表の書き方が違えば、ここの正規表現を W05-26 の後に合わせる（self-test の断片も直す）。
const LEGACY_PARENTS = ["total", "shadows", "postFx", "prepassSsao", "particles", "rtScreen"];
export function loadGpuTimerScopes(root) {
  const f = join(root, "src", "graphics", "GpuTimer.cpp");
  if (!existsSync(f)) return null;
  const src = readFileSync(f, "utf8");
  const table = [...src.matchAll(/\{\s*"([A-Za-z0-9_]+)"\s*,\s*(?:Scope::)?([A-Za-z0-9_]+)\s*(?:,\s*(?:"([A-Za-z0-9_]*)"|nullptr))?\s*\}/g)];
  const all = new Set(), parents = new Set(), aliases = new Map();
  if (table.length) {
    const byLower = new Map(table.map((m) => [m[1].toLowerCase(), m[1]]));
    for (const m of table) {
      all.add(m[1]);
      if (m[3]) aliases.set(m[3], m[1]);
      const par = byLower.get(m[2].toLowerCase());
      if (par && par !== m[1]) parents.add(par);
    }
  } else {
    for (const m of src.matchAll(/return\s+"([A-Za-z0-9_]+)"/g)) all.add(m[1]);
    for (const n of LEGACY_PARENTS) if (all.has(n)) parents.add(n);
  }
  const leaves = new Set([...all].filter((n) => !parents.has(n)));
  for (const [a, n] of aliases) if (leaves.has(n)) leaves.add(a);
  return { all, parents, leaves };
}

export function loadGpuTimerLeaves(root) {
  const s = loadGpuTimerScopes(root);
  return s ? s.leaves : null;
}

// parents: 親のスコープ名の集合（指定されたら、親を指す行は赤。省略時は「存在するか」だけ）
export function checkLeaves(input, leaves, parents = new Set()) {
  const errs = [];
  for (const [pid, p] of Object.entries(input.presets)) {
    const rows = [...p.rows, ...Object.values(p.upscalerColumns).flatMap((c) => c.rows ?? []), ...p.vfx.rows];
    for (const r of rows) {
      if (r.leavesPending) continue; // VFX の行は W1-BD4 の後に検査を有効にする
      for (const l of rowLeaves(r)) {
        if (parents.has(l)) errs.push(`${pid}: 行 ${r.id} の "${l}" は親のスコープ（葉ではない。二重計上になる）`);
        else if (!leaves.has(l)) errs.push(`${pid}: 行 ${r.id} の葉 "${l}" が GpuTimer に無い`);
      }
    }
  }
  return [...new Set(errs)];
}

// ---- 見本（スキーマの形を示すためのもの。★数値はすべて仮置きで、実際の予算ではない。実値は PROGRAM_PLAN §3 からメインが書く）----
export function exampleInput() {
  const R = (id, ms, extra = {}) => ({ id, ms, degrade: "（仮置き）縮退の内容", ...extra });
  const preset = (label, fps, p95, p99, vram) => ({
    label, targetFps: fps, output: [1920, 1080], p95Ms: p95, p99Ms: p99,
    rows: [
      R("DepthPrepass", 0.5),
      R("ClusterCull", 0.2, { leaves: ["hiZ", "clusterCull", "vgCull"] }),
      R("Raytracing", 0.25),
      R("MainScene", 3.3),
      R("Ddgi", 0.7),
    ],
    upscalerColumns: {
      dlss: { gate: "gate", renderScale: 0.6667, rows: [R("Upscale", 1.1, { leaves: ["upscale"] })] },
      taau: { gate: "gateAfterW2BD", renderScale: 0.6667, rows: [R("Upscale", 0.65, { leaves: ["upscale"] })] },
      taaNative: { gate: "record", renderScale: 1.0, rows: [R("Upscale", 0.6, { leaves: ["upscale"] })] },
    },
    vfx: {
      rows: [R("VfxSim", 0.5, { leaves: ["vfxSim"], leavesPending: true }), R("VfxDraw", 0.8, { leaves: ["vfxDraw"], leavesPending: true })],
      emissionMultiplier: 1.0, maxParticles: 20000, peakMs: 2.3,
    },
    vram,
    cpu: { gameThreadP95Ms: 6.0, renderRecordP95Ms: 4.0, editorUiP95Ms: 1.5 },
  });
  return {
    schema: 1,
    presets: {
      epic_1080p60: preset("Epic", 60, 15.5, 16.6, { capGb: 6.0, marginGb: 0.6, precheckGb: 6.5, totalGb: 4.8 }),
      high_1080p60: preset("High", 60, 15.5, 16.6, { capGb: 5.0, marginGb: 0.6, precheckGb: 6.5 }),
      cinematic_1080p30: preset("Cinematic", 30, 31.0, 33.3, { capGb: null, marginGb: 0.6, precheckGb: 6.5 }),
    },
  };
}

// ---- CLI ----
const VALUE_OPTS = ["--presets", "--root", "--out-inc", "--out-dir"];
function parseArgs(argv) {
  const o = { mode: "generate", checkLeaves: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--check") o.mode = "check";
    else if (a === "--check-leaves") { o.mode = "check"; o.checkLeaves = true; }
    else if (a === "--example") o.mode = "example";
    else if (a === "--self-test") o.mode = "selftest";
    else if (VALUE_OPTS.includes(a)) {
      const v = argv[i + 1];
      if (v === undefined || v.startsWith("--")) { console.error(`${a} に値が要る`); process.exit(2); }
      o[a.slice(2).replace(/-(\w)/g, (_, c) => c.toUpperCase())] = v;
      i++;
    } else { console.error(`不明な引数: ${a}`); process.exit(2); }
  }
  return o;
}

function selfTest() {
  let fail = 0;
  const t = (name, cond) => { if (!cond) { fail++; console.error(`NG: ${name}`); } };
  const ex = exampleInput();
  const mut = (fn) => { const c = structuredClone(ex); fn(c.presets.epic_1080p60, c); return validate(c); };
  const has = (errs, sub) => errs.some((m) => m.includes(sub));
  t("見本が検証を通る", validate(ex).length === 0);
  const files = generate(ex);
  t("生成が決定的", [...files.values()].join() === [...generate(ex).values()].join());
  const b = JSON.parse(files.get("json:epic_1080p60"));
  // 共通 4.95 + VFX 1.3 + Upscale 1.1
  t("DLSS 列の合計（VFX を含む）", Math.abs(b.columns.dlss.totalMs - 7.35) < 0.0011);
  t("VFX の行が列の rows にも入る", b.columns.dlss.rows.some((r) => r.id === "VfxSim") && b.columns.dlss.rows.some((r) => r.id === "VfxDraw"));
  t("3 列とも出る", Object.keys(b.columns).join() === "dlss,taau,taaNative");
  t("VFX の行が出る", b.vfx.rows.map((r) => r.id).join() === "VfxSim,VfxDraw");
  t("degrade が JSON の行に出る", b.columns.dlss.rows.every((r) => r.degrade));
  const inc = files.get("inc");
  t("inc に VFX とアップスケーラ", /UNO_QP_VFX\(epic_1080p60/.test(inc) && /UNO_QP_UPSCALER\(epic_1080p60, taaNative, Record/.test(inc));
  t("inc に VFX の行", /UNO_QP_ROW\(epic_1080p60, dlss, VfxSim,/.test(inc));
  t("inc に葉の対応", /UNO_QP_LEAF\(epic_1080p60, ClusterCull, "vgCull"\)/.test(inc));
  t("JSON の列の rows に leavesPending", b.columns.dlss.rows.find((r) => r.id === "VfxSim").leavesPending === true && b.columns.dlss.rows[0].leavesPending === false);
  t("inc に degrade", /UNO_QP_DEGRADE\(epic_1080p60, MainScene,/.test(inc));
  t("inc の VRAM（上限あり）", /UNO_QP_VRAM\(epic_1080p60, 1, 6\.0, 0\.6, 6\.5\)/.test(inc));
  t("inc の VRAM（上限なし）", /UNO_QP_VRAM\(cinematic_1080p30, 0, 0\.0, 0\.6, 6\.5\)/.test(inc));
  const cj = JSON.parse(files.get("json:cinematic_1080p30"));
  t("JSON の VRAM（capGb null / margin / precheck）", cj.vram.capGb === null && cj.vram.marginGb === 0.6 && cj.vram.precheckGb === 6.5);
  t("vfx 欠落を検出", mut((p) => { delete p.vfx; }).length > 0);
  t("gate 列の重複を検出", has(mut((p) => { p.upscalerColumns.taau.gate = "gate"; }), "gate の列は 1 つまで"));
  t("gate 列が 0 個を検出", has(mut((p) => { p.upscalerColumns.dlss.gate = "record"; }), "ちょうど 1 つ"));
  t("gate 列が dlss 以外を検出", has(mut((p) => { p.upscalerColumns.dlss.gate = "record"; p.upscalerColumns.taau.gate = "gate"; }), "dlss であること"));
  t("expectedTotalMs のずれを検出", mut((p) => { p.expectedTotalMs = { dlss: 99 }; }).length > 0);
  t("必須プリセットの欠落を検出", validate({ schema: 1, presets: { epic_1080p60: ex.presets.epic_1080p60 } }).filter((m) => m.includes("が必要（perf_gate")).length === 2);
  t("Epic の列欠落を検出", has(mut((p) => { delete p.upscalerColumns.taaNative; }), "Epic は taaNative 列が必要"));
  t("集約 Vfx 行を検出", has(mut((p) => { p.rows.push({ id: "Vfx", ms: 1.3, degrade: "x" }); }), "集約行 Vfx"));
  t("VfxSim / VfxDraw が同じ葉を指すのを検出", has(mut((p) => { p.vfx.rows[1].leaves = ["vfxSim"]; }), "重複して指す"));
  t("別々の行が同じ葉（MainScene の重い群）を検出", has(mut((p) => { p.rows.push({ id: "MainSceneV2Heavy", ms: 0.3, leaves: ["mainScene"], degrade: "x" }); }), "重複して指す"));
  t("列の行が共通の行と同じ葉を指すのを検出", has(mut((p) => { p.upscalerColumns.taau.rows.push({ id: "Extra", ms: 0.1, leaves: ["mainScene"], degrade: "x" }); }), "重複して指す"));
  t("rows が無く expectedTotalMs があっても落ちない", has(mut((p) => { delete p.rows; p.expectedTotalMs = { dlss: 1 }; }), "rows が配列でない"));
  t("vfx.rows に null があっても落ちない", has(mut((p) => { p.vfx.rows.push(null); }), "id が不正"));
  t("label が文字列でないのを検出", has(mut((p) => { p.label = 5; }), "label は文字列"));
  t("leavesPending を VFX 以外に付けるのを検出", has(mut((p) => { p.rows[0].leavesPending = true; }), "leavesPending"));
  t("列の合計が p95Ms を超えるのを検出", has(mut((p) => { p.rows[3].ms = 30; }), "p95Ms"));
  t("列ごとの degrade の食い違いを検出", has(mut((p) => { p.upscalerColumns.taau.rows[0].degrade = "別の縮退"; }), "食い違う"));
  t("VFX の行と列の行の id 重複を検出", has(mut((p) => { p.upscalerColumns.dlss.rows.push({ id: "VfxSim", ms: 1, degrade: "x" }); }), "重複"));
  t("degrade 欠落を検出", has(mut((p) => { delete p.rows[0].degrade; }), "degrade"));
  t("vram.marginGb 欠落を検出", has(mut((p) => { delete p.vram.marginGb; }), "marginGb"));
  t("vram.precheckGb 欠落を検出", has(mut((p) => { delete p.vram.precheckGb; }), "precheckGb"));
  t("vram.capGb が undefined を検出", has(mut((p) => { delete p.vram.capGb; }), "capGb"));
  const lv = new Set(["depthPrepass", "hiZ", "clusterCull", "vgCull", "raytracing", "mainScene", "ddgi", "upscale"]);
  t("葉検査（全部ある）", checkLeaves(ex, lv).length === 0);
  t("親のスコープを指す行は赤", checkLeaves(ex, lv, new Set(["mainScene"])).some((m) => m.includes("親のスコープ") && m.includes("mainScene")));
  lv.delete("ddgi");
  t("葉検査（欠落を検出）", checkLeaves(ex, lv).length === 3 && checkLeaves(ex, lv).every((m) => m.includes("ddgi")));
  // GpuTimer.cpp の Name() 形式の読み取り（実ファイルの形を模した断片）
  const tmp = mkdtempSync(join(tmpdir(), "gen_budgets_"));
  try {
    mkdirSync(join(tmp, "src", "graphics"), { recursive: true });
    writeFileSync(join(tmp, "src", "graphics", "GpuTimer.cpp"),
      'const char* GpuTimer::Name(Pass p) {\n  switch (p) {\n    case Pass::A: return "depthPrepass";\n    case Pass::B: return "hiZ";\n    default: return "unknown";\n  }\n}\n');
    const got = loadGpuTimerLeaves(tmp);
    t("GpuTimer.cpp の葉を読む", got && got.has("depthPrepass") && got.has("hiZ") && !got.has("ddgi"));
    t("旧形式: 親は葉に入らない", (() => {
      writeFileSync(join(tmp, "src", "graphics", "GpuTimer.cpp"), 'case A: return "total";\ncase B: return "postFx";\ncase C: return "upscale";\ncase D: return "mainScene";\n');
      const sc = loadGpuTimerScopes(tmp);
      return sc.leaves.has("upscale") && sc.leaves.has("mainScene") && !sc.leaves.has("total") && !sc.leaves.has("postFx") && sc.parents.has("postFx");
    })());
    t("新形式（ScopeInfo 表）: parent と alias を読む", (() => {
      writeFileSync(join(tmp, "src", "graphics", "GpuTimer.cpp"),
        'static const ScopeInfo kInfo[] = {\n {"total", Scope::Total, nullptr},\n {"postFx", Scope::Total, nullptr},\n {"upscale", Scope::PostFX, "TaaUp"},\n {"mainScene", Scope::Total, nullptr},\n};\n');
      const sc = loadGpuTimerScopes(tmp);
      return sc.leaves.has("upscale") && sc.leaves.has("TaaUp") && sc.leaves.has("mainScene") && !sc.leaves.has("postFx") && sc.parents.has("postFx") && sc.parents.has("total");
    })());
    t("GpuTimer.cpp が無ければ null", loadGpuTimerLeaves(join(tmp, "none")) === null);
  } finally { rmSync(tmp, { recursive: true, force: true }); }
  if (fail) { console.error(`self-test 失敗: ${fail} 件`); return 1; }
  console.log("self-test OK");
  return 0;
}

function listJson(dir) {
  return existsSync(dir) ? readdirSync(dir).filter((f) => f.endsWith(".json")) : [];
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
    // 生成物が既にあるのに入力が無い（消した / 名前を変えた）なら、SKIP にせず失敗させる
    const orphans = [existsSync(incPath) ? incPath : null, ...listJson(outDir).map((f) => join(outDir, f))].filter(Boolean);
    if (o.mode === "check" && orphans.length) {
      console.error(`GenBudgetsCheck 失敗: 入力 ${presetsPath} が無いのに生成物がある:\n` + orphans.map((m) => "  - " + m).join("\n"));
      return 1;
    }
    console.log(`${o.mode === "check" ? "SKIP" : "エラー"}: 入力 ${presetsPath} がまだ無い（メインが M5 で置く）`);
    return o.mode === "check" ? 77 : 2;
  }
  let input;
  try { input = JSON.parse(readFileSync(presetsPath, "utf8").replace(/^﻿/, "")); }
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
    const expected = new Set(Object.keys(input.presets).map((pid) => `${pid}.json`));
    for (const f of listJson(outDir)) if (!expected.has(f)) bad.push(`${join(outDir, f)} は入力に無いプリセットの古い生成物（消す）`);
    if (o.checkLeaves) {
      const sc = loadGpuTimerScopes(root);
      if (!sc) bad.push("src/graphics/GpuTimer.cpp が読めない");
      else bad.push(...checkLeaves(input, sc.leaves, sc.parents));
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
  for (const f of listJson(outDir))
    if (!Object.keys(input.presets).some((pid) => `${pid}.json` === f)) console.log(`注意: ${join(outDir, f)} は入力に無いプリセットの古い生成物（--check は失敗する。消す）`);
  return 0;
}

// ジャンクション / シンボリックリンク経由の起動でも直接実行と判定する（実体のパスへ正規化して比べる）
const realOf = (p) => { try { return realpathSync(p); } catch { return resolve(p); } };
if (process.argv[1] && realOf(process.argv[1]) === realOf(fileURLToPath(import.meta.url))) process.exit(main());
