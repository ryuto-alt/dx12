// 性能・VRAM 予算の読み込みと判定(MCP-01 / W1-NB1)。
//
// ★数値をここに書かない。予算の値は testing/budgets/<preset>.json(tools/gen_budgets.mjs が budgets/quality_presets.json から生成)だけが持つ。
//   この TS は「列(dlss / taau / taaNative)の選び方」と「測った値を予算に当てる式」だけを持つ。
// 列の取り違えに注意: 合否の列は gate:"gate" の列(W1 は dlss)だけ。gateAfterW2BD / record の列は記録するだけで、合否にしない。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { readJson, round } from "./common.ts";

export type ColumnId = "dlss" | "taau" | "taaNative";
export const COLUMN_IDS: readonly ColumnId[] = ["dlss", "taau", "taaNative"];
export type GateKind = "gate" | "gateAfterW2BD" | "record";

export type BudgetRow = { id: string; ms: number; leaves: string[]; degrade?: string; owner?: string; leavesPending?: boolean };
export type BudgetColumn = { gate: GateKind; renderScale: number; totalMs: number; rows: BudgetRow[] };
export type Budget = {
  preset: string; label: string; targetFps: number; output: [number, number];
  p95Ms: number; p99Ms: number;
  vfx?: { emissionMultiplier?: number; maxParticles?: number; peakMs?: number; rows?: BudgetRow[] };
  vram: { capGb: number | null; marginGb: number; precheckGb: number; totalGb?: number };
  cpu: { gameThreadP95Ms: number; renderRecordP95Ms: number; editorUiP95Ms: number };
  columns: Partial<Record<ColumnId, BudgetColumn>>;
  /** 上書きが適用されたか(プロジェクトの .dx12/budgets.json)。 */
  overridden?: string[];
};

const here = path.dirname(fileURLToPath(import.meta.url));
export function budgetsDir(env: NodeJS.ProcessEnv = process.env): string {
  return env.DX12_BUDGETS_DIR && env.DX12_BUDGETS_DIR.trim() ? path.resolve(env.DX12_BUDGETS_DIR) : path.join(here, "budgets");
}

export function listBudgetPresets(dir = budgetsDir()): string[] {
  try { return fs.readdirSync(dir).filter((f) => f.endsWith(".json")).map((f) => f.replace(/\.json$/, "")).sort(); } catch { return []; }
}

export class BudgetError extends Error {
  readonly code: "E_NOT_FOUND" | "E_INVALID_PARAM";
  readonly validValues?: string[];
  constructor(code: "E_NOT_FOUND" | "E_INVALID_PARAM", message: string, validValues?: string[]) { super(message); this.name = "BudgetError"; this.code = code; this.validValues = validValues; }
}

function validate(b: any, file: string): Budget {
  const bad = (m: string) => { throw new BudgetError("E_INVALID_PARAM", `${path.basename(file)}: ${m}`); };
  if (!b || typeof b !== "object") bad("JSON ではない");
  for (const k of ["p95Ms", "p99Ms"]) if (typeof b[k] !== "number") bad(`${k} が無い`);
  if (!b.columns || typeof b.columns !== "object") bad("columns が無い");
  if (!b.vram || typeof b.vram.marginGb !== "number" || typeof b.vram.precheckGb !== "number") bad("vram.marginGb / precheckGb が無い");
  if (!b.cpu) bad("cpu が無い");
  for (const [id, c] of Object.entries<any>(b.columns)) {
    if (!(COLUMN_IDS as readonly string[]).includes(id)) bad(`未知の列 ${id}`);
    if (!["gate", "gateAfterW2BD", "record"].includes(c?.gate)) bad(`列 ${id} の gate が不正`);
    if (!Array.isArray(c.rows)) bad(`列 ${id} の rows が無い`);
  }
  return b as Budget;
}

export type ProjectOverride = {
  p95Ms?: number; p99Ms?: number;
  vram?: { capGb?: number | null; marginGb?: number; precheckGb?: number };
  cpu?: Partial<Budget["cpu"]>;
  /** 行 id → 上限 ms(全列の同じ id に効く)。 */
  rows?: Record<string, number>;
};

/** <project>/.dx12/budgets.json。無ければ null。形が違えば BudgetError。 */
export function readProjectOverride(projectDir: string | null | undefined): ProjectOverride | null {
  if (!projectDir) return null;
  const f = path.join(projectDir, ".dx12", "budgets.json");
  if (!fs.existsSync(f)) return null;
  const j = readJson<any>(f);
  if (!j || typeof j !== "object" || Array.isArray(j)) throw new BudgetError("E_INVALID_PARAM", `${f}: JSON のオブジェクトではない`);
  const num = (v: unknown, k: string) => { if (v !== undefined && (typeof v !== "number" || !Number.isFinite(v) || v <= 0)) throw new BudgetError("E_INVALID_PARAM", `${f}: ${k} は正の数`); };
  num(j.p95Ms, "p95Ms"); num(j.p99Ms, "p99Ms");
  for (const [k, v] of Object.entries<any>(j.rows ?? {})) num(v, `rows.${k}`);
  return j as ProjectOverride;
}

export function applyOverride(b: Budget, o: ProjectOverride | null): Budget {
  if (!o) return b;
  const out: Budget = JSON.parse(JSON.stringify(b));
  const changed: string[] = [];
  if (o.p95Ms !== undefined) { out.p95Ms = o.p95Ms; changed.push("p95Ms"); }
  if (o.p99Ms !== undefined) { out.p99Ms = o.p99Ms; changed.push("p99Ms"); }
  if (o.vram) { out.vram = { ...out.vram, ...o.vram } as Budget["vram"]; changed.push("vram"); }
  if (o.cpu) { out.cpu = { ...out.cpu, ...o.cpu }; changed.push("cpu"); }
  if (o.rows) {
    for (const col of Object.values(out.columns)) for (const r of col?.rows ?? []) if (o.rows[r.id] !== undefined) { r.ms = o.rows[r.id]; changed.push(`rows.${r.id}`); }
  }
  out.overridden = [...new Set(changed)];
  return out;
}

export function loadBudget(preset: string, opts: { dir?: string; projectDir?: string | null } = {}): Budget {
  const dir = opts.dir ?? budgetsDir();
  const names = listBudgetPresets(dir);
  if (!/^[A-Za-z0-9_-]+$/.test(preset) || !names.includes(preset)) throw new BudgetError("E_NOT_FOUND", `予算 '${preset}' が無い(${dir})`, names);
  const file = path.join(dir, `${preset}.json`);
  const b = validate(readJson(file), file);
  return applyOverride(b, readProjectOverride(opts.projectDir));
}

/** 合否に使う列(gate:"gate")の id。ちょうど 1 つが前提。無ければ null。 */
export function gateColumnOf(b: Budget): ColumnId | null {
  const ids = COLUMN_IDS.filter((c) => b.columns[c]?.gate === "gate");
  return ids.length === 1 ? ids[0] : null;
}

/** VRAM の実効上限(MB)。capGb が null なら「予算 - 余白」だけ。budgetMB は ping.vramBudgetMB(OS が他プロセスの使用量を引いた後の値)。 */
export function vramLimitMB(b: Budget, budgetMB: number | null): { limitMB: number | null; basis: "cap" | "budget-margin" | "cap(budget unknown)" } {
  const cap = b.vram.capGb === null || b.vram.capGb === undefined ? null : b.vram.capGb * 1024;
  const byBudget = budgetMB !== null && Number.isFinite(budgetMB) && budgetMB > 0 ? budgetMB - b.vram.marginGb * 1024 : null;
  if (cap !== null && byBudget !== null) return { limitMB: Math.min(cap, byBudget), basis: cap <= byBudget ? "cap" : "budget-margin" };
  if (cap !== null) return { limitMB: cap, basis: "cap(budget unknown)" };
  return { limitMB: byBudget, basis: "budget-margin" };
}

/** 計測の前提確認のしきい値(MB)。 */
export const precheckMB = (b: Budget): number => b.vram.precheckGb * 1024;

// ── 測った値を予算に当てる ─────────────────────────────────────────────────────
export type Measured = {
  frameMsP95: number | null;
  frameMsP99?: number | null;
  gpuTotalMs: number | null;
  /** perf_stats.gpuLeafMs(葉の名前 → ms)。 */
  leafMs: Record<string, number>;
  cpu: { gameThreadP95Ms: number | null; renderRecordP95Ms: number | null; editorUiP95Ms: number | null };
  vramUsedMB: number | null;
  vramBudgetMB: number | null;
};

export type Check = {
  id: string; kind: "frame" | "gpu" | "row" | "cpu" | "vram";
  actual: number | null; limit: number | null; margin: number | null;
  status: "pass" | "fail" | "unmeasured" | "pending";
  note?: string;
};

function mk(id: string, kind: Check["kind"], actual: number | null, limit: number | null, extra: Partial<Check> = {}): Check {
  if (actual === null || limit === null) return { id, kind, actual, limit, margin: null, status: "unmeasured", ...extra };
  const margin = round(limit - actual, 3);
  return { id, kind, actual: round(actual, 3), limit: round(limit, 3), margin, status: actual <= limit ? "pass" : "fail", ...extra };
}

export function evaluateColumn(b: Budget, col: ColumnId, m: Measured): { checks: Check[]; pass: boolean; failed: Check[]; unmeasured: Check[]; gate: GateKind } {
  const c = b.columns[col];
  if (!c) throw new BudgetError("E_NOT_FOUND", `予算 ${b.preset} に列 ${col} が無い`, Object.keys(b.columns));
  const checks: Check[] = [];
  checks.push(mk("frameMsP95", "frame", m.frameMsP95, b.p95Ms));
  if (m.frameMsP99 !== undefined) checks.push(mk("frameMsP99", "frame", m.frameMsP99, b.p99Ms));
  checks.push(mk("gpuTotalMs", "gpu", m.gpuTotalMs, c.totalMs));
  const rows: BudgetRow[] = [...c.rows];
  for (const r of rows) {
    const found = r.leaves.filter((l) => typeof m.leafMs[l] === "number");
    if (found.length === 0) { checks.push({ id: r.id, kind: "row", actual: null, limit: round(r.ms, 3), margin: null, status: r.leavesPending ? "pending" : "unmeasured", note: r.leavesPending ? "葉が未計装(leavesPending)" : `葉 ${r.leaves.join(" / ")} が perf_stats に無い` }); continue; }
    const sum = found.reduce((a, l) => a + m.leafMs[l], 0);
    checks.push(mk(r.id, "row", sum, r.ms, found.length < r.leaves.length ? { note: `葉の一部だけ計測(${found.join(", ")})` } : {}));
  }
  checks.push(mk("gameThreadP95Ms", "cpu", m.cpu.gameThreadP95Ms, b.cpu.gameThreadP95Ms));
  checks.push(mk("renderRecordP95Ms", "cpu", m.cpu.renderRecordP95Ms, b.cpu.renderRecordP95Ms));
  checks.push(mk("editorUiP95Ms", "cpu", m.cpu.editorUiP95Ms, b.cpu.editorUiP95Ms));
  const lim = vramLimitMB(b, m.vramBudgetMB);
  checks.push(mk("vramMB", "vram", m.vramUsedMB, lim.limitMB, { note: lim.basis }));
  const failed = checks.filter((x) => x.status === "fail");
  const unmeasured = checks.filter((x) => x.status === "unmeasured");
  return { checks, pass: failed.length === 0, failed, unmeasured, gate: c.gate };
}

/** 超過の大きい順に並べた、縮退の案内(各行の degrade)。 */
export function degradeAdvice(b: Budget, col: ColumnId, failed: Check[]): { id: string; over: number; degrade: string }[] {
  const rows = new Map((b.columns[col]?.rows ?? []).map((r) => [r.id, r]));
  return failed.filter((f) => f.kind === "row" && f.margin !== null)
    .map((f) => ({ id: f.id, over: round(-(f.margin as number), 3) as number, degrade: rows.get(f.id)?.degrade ?? "" }))
    .sort((a, c) => c.over - a.over);
}
