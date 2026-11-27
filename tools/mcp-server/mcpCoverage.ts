// mcp_feature_coverage の雛形(W1-NB7)。エンジンの MCP method(manifest.snapshot.json)ごとに「例が 1 件以上」「searchHints が日本語・英語の両方にある」を調べる。
// 方針は棚卸し(ラチェット): 既存の欠けは mcpCoverage.baseline.json に載せて許し、載っていない method(= 新しく足した method)が欠けていたら赤にする。
// ベースラインは減らす方向にだけ動く(解消した項目が残っていても赤)。W2 の網羅の検査はベースラインを空にして、ここを本格版へ広げる。
import type { Manifest } from "./manifest.ts";

export type CoverageGaps = { noExample: string[]; noHints: string[]; noDoc: string[] };

type DocLike = { method?: string; examples?: unknown[] };
type HintMap = Record<string, string | undefined>;

/** マニフェストの全 method を、カタログの項目・searchHints と突き合わせて欠けを返す(名前順)。 */
export function computeCoverageGaps(manifest: Manifest, docs: DocLike[], hintsJa: HintMap, hintsEn: HintMap): CoverageGaps {
  const byMethod = new Map<string, DocLike>();
  for (const d of docs) if (d.method) byMethod.set(d.method, d);
  const gaps: CoverageGaps = { noExample: [], noHints: [], noDoc: [] };
  for (const name of manifest.methods.keys()) {
    const d = byMethod.get(name);
    if (!d) { gaps.noDoc.push(name); continue; }
    if (!d.examples || d.examples.length === 0) gaps.noExample.push(name);
    if (!(hintsJa[name] ?? "").trim() || !(hintsEn[name] ?? "").trim()) gaps.noHints.push(name);
  }
  for (const k of Object.keys(gaps) as (keyof CoverageGaps)[]) gaps[k].sort();
  return gaps;
}

export type BaselineVerdict = { newGaps: string[]; staleBaseline: string[] };

/** 欠け(gaps)をベースラインと比べる。newGaps = ベースラインに無い欠け(赤)、staleBaseline = もう欠けていない/消えた method(赤。ベースラインから消す)。 */
export function judgeAgainstBaseline(kind: string, gaps: string[], baseline: string[], existing: Set<string>): BaselineVerdict {
  const g = new Set(gaps), b = new Set(baseline);
  return {
    newGaps: gaps.filter((n) => !b.has(n)).map((n) => `${kind}: ${n}`),
    staleBaseline: baseline.filter((n) => !g.has(n) || !existing.has(n)).map((n) => `${kind}: ${n}`),
  };
}
